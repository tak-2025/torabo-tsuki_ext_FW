/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * ztc_encoder — turn 1-axis pointer motion (a mini-trackpad swipe) into discrete
 * key/behavior TAPS, in either direction. Accumulates the signed delta of one
 * INPUT_EV_REL code; every `threshold` units it taps a behavior:
 *   accum >= +threshold  -> bindings[0] (the "increase / forward" behavior)
 *   accum <= -threshold  -> bindings[1] (the "decrease / back" behavior)
 * The matched axis event is consumed (STOP) so it never also moves the cursor.
 *
 * This is the building block behind the torabo-trackpad snippet: volume
 * (C_VOLUME_UP/DOWN), brightness, browser back/forward (C_AC_BACK/FORWARD), zoom
 * (LC(EQUAL)/LC(MINUS)), etc. are all just a choice of the two bindings +
 * threshold in devicetree. Fixed per layer via the listener's per-layer
 * children; no runtime store / GATT needed.
 *
 * Unlike the stock zmk,input-processor-behaviors (which matches button
 * press/release events), this generates a full press->release TAP from relative
 * motion and splits by sign, which stock parts cannot do. Self-contained: does
 * NOT depend on the trackball config module (ZMK_TRACKBALL_CONFIG).
 */

#define DT_DRV_COMPAT zmk_input_processor_ztc_encoder

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_REGISTER(ztc_encoder, CONFIG_ZMK_LOG_LEVEL);

/* Bound taps generated from a single event, so a spurious huge delta can't spin. */
#define ZTC_ENCODER_MAX_TAPS 10

struct ztc_encoder_config {
    uint8_t index;
    uint16_t type;
    uint16_t code;
    int32_t threshold;                           /* clamped to >= 1 at use */
    const struct zmk_behavior_binding *bindings; /* [0]=positive, [1]=negative */
};

struct ztc_encoder_data {
    int32_t accum;
};

static void ztc_encoder_tap(const struct ztc_encoder_config *cfg,
                            const struct zmk_input_processor_state *state,
                            const struct zmk_behavior_binding *binding) {
    struct zmk_behavior_binding_event ev = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(state->input_device_index,
                                                                     cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    (void)zmk_behavior_invoke_binding(binding, ev, true);  /* press */
    (void)zmk_behavior_invoke_binding(binding, ev, false); /* release */
}

static int ztc_encoder_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    const struct ztc_encoder_config *cfg = dev->config;
    struct ztc_encoder_data *data = dev->data;

    if (event->type != cfg->type || event->code != cfg->code) {
        return ZMK_INPUT_PROC_CONTINUE; /* not our axis: let it flow */
    }

    const int32_t thr = cfg->threshold < 1 ? 1 : cfg->threshold;
    data->accum += event->value;

    int taps = 0;
    while (data->accum >= thr && taps < ZTC_ENCODER_MAX_TAPS) {
        ztc_encoder_tap(cfg, state, &cfg->bindings[0]);
        data->accum -= thr;
        taps++;
    }
    while (data->accum <= -thr && taps < ZTC_ENCODER_MAX_TAPS) {
        ztc_encoder_tap(cfg, state, &cfg->bindings[1]);
        data->accum += thr;
        taps++;
    }
    /* Never carry more than one step of residue (bounds runaway after a spike). */
    if (data->accum >= thr) {
        data->accum = thr - 1;
    } else if (data->accum <= -thr) {
        data->accum = -(thr - 1);
    }

    /* This axis drives taps, not the cursor: consume it. */
    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api ztc_encoder_api = {
    .handle_event = ztc_encoder_handle_event,
};

#define ZTC_ENCODER_INST(n)                                                                        \
    static const struct zmk_behavior_binding ztc_encoder_bindings_##n[] = {                        \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    BUILD_ASSERT(ARRAY_SIZE(ztc_encoder_bindings_##n) == 2,                                        \
                 "ztc-encoder needs exactly 2 bindings: <positive>, <negative>");                  \
    static struct ztc_encoder_data ztc_encoder_data_##n;                                            \
    static const struct ztc_encoder_config ztc_encoder_config_##n = {                              \
        .index = n,                                                                                \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .code = DT_INST_PROP(n, code),                                                             \
        .threshold = DT_INST_PROP_OR(n, threshold, 8),                                             \
        .bindings = ztc_encoder_bindings_##n,                                                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &ztc_encoder_data_##n, &ztc_encoder_config_##n,           \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ztc_encoder_api);

DT_INST_FOREACH_STATUS_OKAY(ZTC_ENCODER_INST)
