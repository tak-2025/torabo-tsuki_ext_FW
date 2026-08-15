/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * ztc_pointer — the configurable per-axis pointing processor (v2).
 * docs/DESIGN_v2.md §11.C. Wired as the input-listener BASE processor so it runs
 * on EVERY pointing event regardless of layer => structurally fail-open.
 *
 * Per axis (X or Y) of the active layer: direction(invert) -> speed(divide with
 * per-axis remainder, never truncates slow motion to 0) -> role:
 *   MOVE   keep REL_X/REL_Y
 *   SCROLL remap REL_X->REL_HWHEEL / REL_Y->REL_WHEEL
 *   OFF    value=0 (NOT STOP; a 0-value REL is harmless and never drops the event)
 * Always returns ZMK_INPUT_PROC_CONTINUE. Never STOP, never negative.
 * Any doubt (unknown role / out-of-range layer / empty store) => MOVE.
 */

#define DT_DRV_COMPAT zmk_input_processor_ztc_pointer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

struct ztc_pointer_data {
    int16_t rem_x;
    int16_t rem_y;
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

static int ztc_pointer_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    const bool is_x = (event->code == INPUT_REL_X);
    const bool is_y = (event->code == INPUT_REL_Y);
    if (!is_x && !is_y) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct ztc_pointer_data *data = dev->data;
    const struct ztc_snapshot *s = ztc_live();
    const uint8_t layer = zmk_keymap_highest_layer_active(); /* index */
    const struct ztc_axis_cfg a = ztc_axis_for(s, layer, is_x);

    int16_t v = event->value;
    if (a.direction) {
        v = (v == INT16_MIN) ? INT16_MAX : (int16_t)-v; /* guard overflow */
    }
    v = scale_rem(v, a.speed_div, is_x ? &data->rem_x : &data->rem_y);
    event->value = v;

    switch (a.role) {
    case ZTC_ROLE_SCROLL:
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
        break;
    case ZTC_ROLE_OFF:
        event->value = 0; /* suppress this axis safely; never STOP */
        break;
    case ZTC_ROLE_MOVE:
    default:
        break; /* keep REL_X / REL_Y */
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api ztc_pointer_api = {
    .handle_event = ztc_pointer_handle_event,
};

#define ZTC_POINTER_INST(n)                                                                        \
    static struct ztc_pointer_data ztc_pointer_data_##n;                                           \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &ztc_pointer_data_##n, NULL, POST_KERNEL,                 \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ztc_pointer_api);

DT_INST_FOREACH_STATUS_OKAY(ZTC_POINTER_INST)
