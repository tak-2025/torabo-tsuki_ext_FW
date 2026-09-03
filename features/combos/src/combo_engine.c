/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Dynamic combo engine — a near-literal copy of zmk/app/src/combo.c. The combo
 * matching state machine (candidate search, overlap, timeout, activate/release)
 * is UNCHANGED so normal-typing safety is identical to upstream. Only four
 * things differ from the original (see DESIGN-combos.md §5.1):
 *
 *   1. Activates from OUR singleton DT node (compatible "zmk,dynamic-combos"),
 *      not "zmk,combos" — so upstream combo.c stays inert and we are the single
 *      owner of the position-combo listener.
 *   2. The definition array `combos[]` is a fixed-size RAM array (CB_SLOTS),
 *      not a DT-generated const. `combo_lookup` is rebuilt whenever it changes.
 *   3. New config is pulled from the NVS-backed store ONLY at idle
 *      (no keys pressed, no active combos), in this listener's serialized
 *      context — so the engine never observes a half-applied swap.
 *   4. The fired behavior's virtual key position uses our own reserved base
 *      above ZMK's virtual position space (upstream combo range collapses to
 *      0 width when "zmk,combos" is absent, which would collide with the
 *      input-processor positions the trackball uses).
 *
 * Fail-safe: `combos[]` is zero-initialized => every slot enabled=false =>
 * `combo_lookup` empty => no key is ever captured until a valid config loads.
 */

#define DT_DRV_COMPAT zmk_dynamic_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>

#include <zmk_dynamic_keymap/dcombo.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

LOG_MODULE_DECLARE(dcombo_config, CONFIG_ZMK_DYNAMIC_COMBOS_LOG_LEVEL);

/* Always defined upstream (default 4), but guard in case a future ZMK gates it
 * behind the native combos node we deliberately do not instantiate. */
#ifndef CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS
#define CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS 4
#endif

/* Reserved virtual-key-position base for fired combos, placed above all of
 * ZMK's virtual positions (keys + sensors + 0-width combos + input processors).
 * 0x100 is generous: sensors/input-processors number in the single digits. */
#define CB_VKP_BASE (ZMK_KEYMAP_LEN + 0x100)
#define CB_VKP(idx) (CB_VKP_BASE + (idx))

#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(CB_SLOTS, 32)

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[CB_MAX_POS];
};

// Live combo definitions (RAM, NVS-backed). Zero-init => all disabled.
static struct cb_combo combos[CB_SLOTS];
// Sequence of the config snapshot currently loaded into combos[].
static uint32_t applied_seq = 0;

// All matching state is file-local (unlike upstream combo.c, which leaves these
// at external linkage); we are a separate translation unit and never share them.
static uint8_t pressed_keys_count = 0;
// set of keys pressed
static struct zmk_position_state_changed_event pressed_keys[CB_MAX_POS] = {};
// the set of candidate combos based on the currently pressed_keys
static uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
static int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
static uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
static struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
static uint8_t active_combo_count = 0;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
static int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
static int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Rebuild the position->combos lookup from combos[]. Disabled slots and
// out-of-range positions are skipped, so they can never become candidates.
static void rebuild_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
    for (size_t i = 0; i < CB_SLOTS; i++) {
        if (!combos[i].enabled) {
            continue;
        }
        for (size_t kp = 0; kp < combos[i].key_position_len; kp++) {
            int32_t pos = combos[i].key_positions[kp];
            if (pos < 0 || pos >= ZMK_KEYMAP_LEN) {
                continue;
            }
            sys_bitfield_set_bit((mem_addr_t)&combo_lookup[pos], i);
        }
    }
}

// Pull a fresh config snapshot from the store IF it changed. Caller MUST be at
// idle (pressed_keys_count==0 && active_combo_count==0) so no live state
// references a stale combo index across the swap.
static void maybe_reload(void) {
    if (cb_fetch_pending(combos, &applied_seq)) {
        rebuild_lookup();
        LOG_DBG("combo config reloaded (seq=%u)", applied_seq);
    }
}

static bool combo_active_on_layer(const struct cb_combo *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct cb_combo *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct cb_combo *combo = &combos[i];
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct cb_combo *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == CB_MAX_POS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_dyn_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct cb_combo *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = CB_VKP(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct cb_combo *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = CB_VKP(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, combos[active_combo->combo_idx].key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct cb_combo *c = &combos[active_combo->combo_idx];
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        // Idle: safe point to swap in a freshly edited config (see maybe_reload).
        if (active_combo_count == 0) {
            maybe_reload();
        }
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        for (int i = 0; i < ARRAY_SIZE(combos); i++) {
            if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                const struct cb_combo *candidate_combo = &combos[i];
                if (candidate_is_completely_pressed(candidate_combo)) {
                    fully_pressed_combo = i;
                    if (num_candidates == 1) {
                        cleanup();
                    }
                }

                return ret;
            }
        }
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_dyn_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dyn_combo, behavior_dyn_combo_listener);
ZMK_SUBSCRIPTION(dyn_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(dyn_combo, zmk_keycode_state_changed);

static int dyn_combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    // combos[] starts empty (all disabled); the first idle keydown pulls the
    // NVS-loaded config via maybe_reload(). No DT combos to initialize.
    return 0;
}

SYS_INIT(dyn_combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
