/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * tp_pointer — the device-aware configurable trackpad processor (v2).
 * Wired as the input-listener BASE processor so it runs on EVERY pointing event
 * regardless of layer => structurally fail-open. Its `device-id` selects which
 * store device (0=left pad, 1=right ext pad) to read, so one instance drives one
 * pad; two instances (left/right) keep independent accumulators.
 *
 * Per axis of the active layer, from tp_live():
 *   MOVE     direction(invert) -> step(divide, kept remainder) -> REL_X/Y
 *   SCROLL   same, but remap REL_X->HWHEEL / REL_Y->WHEEL
 *   OFF      value=0 (never STOP)
 *   ENCODER  accumulate the (optionally inverted) axis; every `step` units fire
 *            the per-axis pos/neg binding (§2.1) and CONSUME the axis (STOP).
 * Continuous roles CONTINUE; any doubt (unknown role / bad layer / empty store)
 * => MOVE. v1's fixed discrete roles are gone: the app now assigns ENCODER with
 * an arbitrary pos/neg binding pair, built at runtime from the store (§4.2), so
 * any keycode is assignable — while continuous behavior is byte-identical to v1.
 */

#define DT_DRV_COMPAT zmk_input_processor_tp_pointer

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

/* Bound taps generated from a single event, so a spurious huge delta can't spin. */
#define TP_ENCODER_MAX_TAPS 10

struct tp_pointer_config {
    uint8_t index;    /* DT instance index (virtual key position) */
    uint8_t device_id;
    const char *const *beh_refs; /* TP_REF_* behavior device names (binding.h) */
};

struct tp_pointer_data {
    int16_t rem_x, rem_y;   /* continuous scaling remainder, per axis */
    int32_t acc_x, acc_y;   /* discrete accumulator, per axis */
    uint8_t role_x, role_y; /* last role seen per axis (reset accum on change) */
};

/* value/div with persisted remainder (mirrors ZMK scaler scale_val, mul=1). */
static int16_t scale_rem(int16_t value, uint8_t div, int16_t *rem) {
    if (div < 1) {
        div = 1;
    }
    int16_t num = (int16_t)(value + *rem);
    int16_t out = (int16_t)(num / (int16_t)div);
    *rem = (int16_t)(num - (int16_t)(out * (int16_t)div));
    return out;
}

/*
 * Discrete taps must NOT be invoked synchronously from here: this handler runs on
 * the input-processor thread (for the split/left pad, the BLE RX context), where
 * a synchronous behavior invoke -> HID send can deadlock or overflow the stack
 * and drop the keyboard. So queue the tap and fire it from the system work queue,
 * per DESIGN-trackpad §4 (same non-blocking discipline as ztc_temp_layer). The
 * binding is built at runtime from the store, so it is queued BY VALUE.
 */
#define TP_TAP_QUEUE_LEN 16

struct tp_tap_req {
    struct zmk_behavior_binding binding; /* runtime-built; carried by value */
    uint32_t position;
};

K_MSGQ_DEFINE(tp_tap_msgq, sizeof(struct tp_tap_req), TP_TAP_QUEUE_LEN, 4);

static void tp_tap_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct tp_tap_req req;
    while (k_msgq_get(&tp_tap_msgq, &req, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding_event ev = {
            .position = req.position,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        (void)zmk_behavior_invoke_binding(&req.binding, ev, true);  /* press */
        (void)zmk_behavior_invoke_binding(&req.binding, ev, false); /* release */
    }
}

static K_WORK_DEFINE(tp_tap_work, tp_tap_work_cb);

static void tp_tap(const struct tp_pointer_config *cfg,
                   const struct zmk_input_processor_state *state,
                   const struct tp_binding *desc) {
    if (!tp_binding_active(desc)) {
        return; /* NONE => nothing to fire (fail-open) */
    }
    struct tp_tap_req req = {
        .binding = tp_make_binding(cfg->beh_refs, desc),
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index,
                                                                     cfg->index),
    };
    (void)k_msgq_put(&tp_tap_msgq, &req, K_NO_WAIT); /* drop if full; taps are bounded */
    (void)k_work_submit(&tp_tap_work);
}

static int tp_pointer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    const bool is_x = (event->code == INPUT_REL_X);
    const bool is_y = (event->code == INPUT_REL_Y);
    if (!is_x && !is_y) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    const struct tp_pointer_config *cfg = dev->config;
    struct tp_pointer_data *data = dev->data;
    const struct tp_snapshot *s = tp_live();
    const uint8_t layer = zmk_keymap_highest_layer_active(); /* index */
    const struct tp_axis_cfg a = tp_axis_for(s, cfg->device_id, layer, is_x);

    int16_t *rem = is_x ? &data->rem_x : &data->rem_y;
    int32_t *acc = is_x ? &data->acc_x : &data->acc_y;
    uint8_t *role_seen = is_x ? &data->role_x : &data->role_y;
    if (*role_seen != a.role) { /* role changed (layer switch etc.): drop stale state */
        *rem = 0;
        *acc = 0;
        *role_seen = a.role;
    }

    int16_t v = event->value;
    if (a.direction) {
        v = (v == INT16_MIN) ? INT16_MAX : (int16_t)-v; /* guard overflow */
    }

    switch (a.role) {
    case TP_ROLE_MOVE:
        event->value = scale_rem(v, a.step, rem);
        return ZMK_INPUT_PROC_CONTINUE; /* keep REL_X / REL_Y */
    case TP_ROLE_SCROLL:
        event->value = scale_rem(v, a.step, rem);
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
        return ZMK_INPUT_PROC_CONTINUE;
    case TP_ROLE_OFF:
        event->value = 0; /* suppress this axis safely; never STOP */
        return ZMK_INPUT_PROC_CONTINUE;
    case TP_ROLE_ENCODER:
        break; /* discrete taps below */
    default:
        return ZMK_INPUT_PROC_CONTINUE; /* fail-open: unknown role => move-through */
    }

    /* ENCODER: accumulate and tap pos/neg every `step` units, consuming the axis. */
    const int32_t thr = (a.step < 1) ? 1 : a.step;
    *acc += v;

    int taps = 0;
    while (*acc >= thr && taps < TP_ENCODER_MAX_TAPS) {
        tp_tap(cfg, state, &a.pos);
        *acc -= thr;
        taps++;
    }
    while (*acc <= -thr && taps < TP_ENCODER_MAX_TAPS) {
        tp_tap(cfg, state, &a.neg);
        *acc += thr;
        taps++;
    }
    /* Never carry more than one step of residue (bounds runaway after a spike). */
    if (*acc >= thr) {
        *acc = thr - 1;
    } else if (*acc <= -thr) {
        *acc = -(thr - 1);
    }

    return ZMK_INPUT_PROC_STOP; /* this axis drives taps, not the cursor: consume it */
}

static const struct zmk_input_processor_driver_api tp_pointer_api = {
    .handle_event = tp_pointer_handle_event,
};

#define TP_POINTER_INST(n)                                                                         \
    TP_BEH_REFS_DEFINE(tp_pointer_beh_refs_##n, n);                                                 \
    static struct tp_pointer_data tp_pointer_data_##n;                                              \
    static const struct tp_pointer_config tp_pointer_config_##n = {                                \
        .index = n,                                                                                \
        .device_id = DT_INST_PROP(n, device_id),                                                   \
        .beh_refs = tp_pointer_beh_refs_##n,                                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &tp_pointer_data_##n, &tp_pointer_config_##n, POST_KERNEL,\
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tp_pointer_api);

DT_INST_FOREACH_STATUS_OKAY(TP_POINTER_INST)
