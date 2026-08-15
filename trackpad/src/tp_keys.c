/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * tp_keys — gesture-remap input processor (v2, DESIGN-trackpad-v2.md §4.1/§4.4).
 * Sibling of tp_pointer, wired as a listener BASE processor on the SAME pointing
 * listener. tp_pointer only touches INPUT_EV_REL, tp_keys only INPUT_EV_KEY, so
 * they never conflict. For the device named by `device-id` and the active layer,
 * it maps the IQS7211E driver's fixed clicks to the store's gesture bindings:
 *
 *   INPUT_BTN_0  single tap      -> GST_TAP   (with double-tap window, below)
 *   INPUT_BTN_1  two-finger tap  -> GST_TAP2
 *   INPUT_BTN_2  press&hold      -> GST_HOLD  (down/up follow the BTN value)
 *
 * Fail-open: an unset slot (or a NONE binding) is NOT consumed -> the driver's
 * default click passes through unchanged (CONTINUE). A configured slot consumes
 * both edges (STOP) and fires the runtime-built binding via msgq -> system work
 * queue, NEVER synchronously from the input thread (same discipline as tp_pointer).
 *
 * Double-tap (GST_DTAP, §4.4): detected here, driver unmodified. When (and only
 * when) a DTAP binding exists for the layer/device, the single tap is deferred by
 * the window (k_work_delayable): a 2nd BTN_0 press inside the window fires DTAP
 * and cancels the deferred single; otherwise the single fires on expiry. No DTAP
 * binding => no deferral => the single tap fires immediately (zero added latency).
 *
 * NOTE: the v2 wire (tpConfigV2.ts) carries { tap, tap2, hold, dtap } (16B). DTAP
 * is detected here via the BTN_0 timing window (driver unmodified). HOLD consumes
 * INPUT_BTN_2, which the iqs7211e driver emits on a press&hold (see its
 * iqs7211e_hold_work_handler, added for torabo v2): BTN_2 press when a finger is
 * held ~350ms, release on finger-lift. So all four gesture slots are live.
 */

#define DT_DRV_COMPAT zmk_input_processor_tp_keys

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#include <zmk_trackpad_config/config.h>
#include <zmk_trackpad_config/binding.h>

LOG_MODULE_DECLARE(tp_config, CONFIG_ZMK_TRACKPAD_CONFIG_LOG_LEVEL);

#define TP_KEYS_QUEUE_LEN 16
#define TP_DTAP_WINDOW_MS CONFIG_ZMK_TRACKPAD_CONFIG_DTAP_WINDOW_MS

struct tp_keys_config {
    uint8_t index;    /* DT instance index (virtual key position) */
    uint8_t device_id;
    const char *const *beh_refs; /* TP_REF_* behavior device names (binding.h) */
};

struct tp_keys_data {
    struct k_work_delayable tap_defer; /* deferred single-tap when DTAP is armed */
    struct zmk_behavior_binding pending_binding; /* single tap to fire on expiry */
    uint32_t pending_pos;
    bool pending_active; /* pending single tap actually fires something */
    bool pending_valid;  /* a first tap is awaiting its window */
    int64_t last_tap_time;
    bool own_btn0, own_btn1, own_btn2; /* we consumed the press => swallow release */
};

/* ---- non-blocking fire path (msgq -> system work queue) ------------------- */

enum tp_keys_action {
    TP_ACT_TAP = 0, /* press then release (a click) */
    TP_ACT_PRESS,   /* press only (hold down) */
    TP_ACT_RELEASE, /* release only (hold up) */
};

struct tp_keys_req {
    struct zmk_behavior_binding binding; /* runtime-built; carried by value */
    uint32_t position;
    uint8_t action; /* enum tp_keys_action */
};

K_MSGQ_DEFINE(tp_keys_msgq, sizeof(struct tp_keys_req), TP_KEYS_QUEUE_LEN, 4);

static void tp_keys_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct tp_keys_req req;
    while (k_msgq_get(&tp_keys_msgq, &req, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding_event ev = {
            .position = req.position,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        if (req.action != TP_ACT_RELEASE) {
            (void)zmk_behavior_invoke_binding(&req.binding, ev, true); /* press */
        }
        if (req.action != TP_ACT_PRESS) {
            (void)zmk_behavior_invoke_binding(&req.binding, ev, false); /* release */
        }
    }
}

static K_WORK_DEFINE(tp_keys_work, tp_keys_work_cb);

static void tp_keys_enqueue(const struct zmk_behavior_binding *binding, uint32_t position,
                            uint8_t action) {
    struct tp_keys_req req = {.binding = *binding, .position = position, .action = action};
    (void)k_msgq_put(&tp_keys_msgq, &req, K_NO_WAIT); /* drop if full; bounded */
    (void)k_work_submit(&tp_keys_work);
}

/* Build + enqueue a descriptor fire (NONE => nothing, fail-open). */
static void tp_keys_fire(const struct tp_keys_config *cfg, uint32_t position,
                         const struct tp_binding *desc, uint8_t action) {
    if (!tp_binding_active(desc)) {
        return;
    }
    struct zmk_behavior_binding b = tp_make_binding(cfg->beh_refs, desc);
    tp_keys_enqueue(&b, position, action);
}

/* Deferred single-tap: the DTAP window expired without a second tap. Runs on the
 * system work queue, so it is safe to hand off to the fire path from here. */
static void tp_keys_tap_defer_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct tp_keys_data *data = CONTAINER_OF(dwork, struct tp_keys_data, tap_defer);
    if (!data->pending_valid) {
        return;
    }
    data->pending_valid = false;
    if (data->pending_active) {
        tp_keys_enqueue(&data->pending_binding, data->pending_pos, TP_ACT_TAP);
    }
}

/* ---- BTN handlers -------------------------------------------------------- */

/* Simple tap remap (two-finger). Consume both edges iff configured. */
static int tp_keys_handle_tap2(const struct tp_keys_config *cfg, struct tp_keys_data *data,
                               uint32_t position, const struct tp_binding *desc, bool press) {
    if (press) {
        if (!tp_binding_active(desc)) {
            data->own_btn1 = false;
            return ZMK_INPUT_PROC_CONTINUE; /* fail-open: default click passes */
        }
        data->own_btn1 = true;
        tp_keys_fire(cfg, position, desc, TP_ACT_TAP);
        return ZMK_INPUT_PROC_STOP;
    }
    if (data->own_btn1) { /* swallow the driver's paired release */
        data->own_btn1 = false;
        return ZMK_INPUT_PROC_STOP;
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

/* Single tap + double-tap window (BTN_0). */
static int tp_keys_handle_tap(const struct tp_keys_config *cfg, struct tp_keys_data *data,
                              uint32_t position, const struct tp_gestures *g, bool press) {
    if (!press) {
        if (data->own_btn0) {
            data->own_btn0 = false;
            return ZMK_INPUT_PROC_STOP; /* swallow paired release */
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const bool have_tap = tp_binding_active(&g->tap);
    const bool have_dtap = tp_binding_active(&g->dtap);
    if (!have_tap && !have_dtap) {
        data->own_btn0 = false;
        return ZMK_INPUT_PROC_CONTINUE; /* fully unset: default left click passes */
    }
    data->own_btn0 = true;

    if (!have_dtap) {
        tp_keys_fire(cfg, position, &g->tap, TP_ACT_TAP); /* no deferral, no latency */
        return ZMK_INPUT_PROC_STOP;
    }

    const int64_t now = k_uptime_get();
    if (data->pending_valid && (now - data->last_tap_time) <= TP_DTAP_WINDOW_MS) {
        /* second tap inside the window => double tap */
        (void)k_work_cancel_delayable(&data->tap_defer);
        data->pending_valid = false;
        tp_keys_fire(cfg, position, &g->dtap, TP_ACT_TAP);
        return ZMK_INPUT_PROC_STOP;
    }

    /* first tap: defer the single by the window (fail-open: if no 2nd tap, the
     * single still fires — no action is lost). */
    data->pending_active = have_tap;
    data->pending_binding = tp_make_binding(cfg->beh_refs, &g->tap);
    data->pending_pos = position;
    data->pending_valid = true;
    data->last_tap_time = now;
    (void)k_work_reschedule(&data->tap_defer, K_MSEC(TP_DTAP_WINDOW_MS));
    return ZMK_INPUT_PROC_STOP;
}

/* Press&hold: down/up follow the BTN value (e.g. &mo holds a layer). */
static int tp_keys_handle_hold(const struct tp_keys_config *cfg, struct tp_keys_data *data,
                               uint32_t position, const struct tp_binding *desc, bool press) {
    if (press) {
        if (!tp_binding_active(desc)) {
            data->own_btn2 = false;
            return ZMK_INPUT_PROC_CONTINUE;
        }
        data->own_btn2 = true;
        tp_keys_fire(cfg, position, desc, TP_ACT_PRESS);
        return ZMK_INPUT_PROC_STOP;
    }
    if (data->own_btn2) {
        data->own_btn2 = false;
        tp_keys_fire(cfg, position, desc, TP_ACT_RELEASE);
        return ZMK_INPUT_PROC_STOP;
    }
    return ZMK_INPUT_PROC_CONTINUE;
}

static int tp_keys_handle_event(const struct device *dev, struct input_event *event,
                                uint32_t param1, uint32_t param2,
                                struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    if (event->type != INPUT_EV_KEY) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct tp_keys_config *cfg = dev->config;
    struct tp_keys_data *data = dev->data;
    const struct tp_snapshot *s = tp_live();
    const uint8_t layer = zmk_keymap_highest_layer_active();
    const struct tp_gestures g = tp_gestures_for(s, cfg->device_id, layer);
    const uint32_t position =
        ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index, cfg->index);
    const bool press = (event->value != 0);

    switch (event->code) {
    case INPUT_BTN_0:
        return tp_keys_handle_tap(cfg, data, position, &g, press);
    case INPUT_BTN_1:
        return tp_keys_handle_tap2(cfg, data, position, &g.tap2, press);
    case INPUT_BTN_2:
        return tp_keys_handle_hold(cfg, data, position, &g.hold, press);
    default:
        return ZMK_INPUT_PROC_CONTINUE;
    }
}

static int tp_keys_init(const struct device *dev) {
    struct tp_keys_data *data = dev->data;
    k_work_init_delayable(&data->tap_defer, tp_keys_tap_defer_cb);
    return 0;
}

static const struct zmk_input_processor_driver_api tp_keys_api = {
    .handle_event = tp_keys_handle_event,
};

#define TP_KEYS_INST(n)                                                                            \
    TP_BEH_REFS_DEFINE(tp_keys_beh_refs_##n, n);                                                    \
    static struct tp_keys_data tp_keys_data_##n;                                                    \
    static const struct tp_keys_config tp_keys_config_##n = {                                       \
        .index = n,                                                                                 \
        .device_id = DT_INST_PROP(n, device_id),                                                    \
        .beh_refs = tp_keys_beh_refs_##n,                                                           \
    };                                                                                              \
    DEVICE_DT_INST_DEFINE(n, tp_keys_init, NULL, &tp_keys_data_##n, &tp_keys_config_##n,            \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tp_keys_api);

DT_INST_FOREACH_STATUS_OKAY(TP_KEYS_INST)
