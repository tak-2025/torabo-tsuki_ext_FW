/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * `&dmac <slot>` behavior: on press, replay the NVS-stored step list for that
 * slot by queueing key-press taps/holds onto ZMK's behavior queue (same engine
 * the built-in macro uses). Fail-open: empty/out-of-range slots do nothing.
 */

#define DT_DRV_COMPAT zmk_behavior_dynamic_macro

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>

#include <zmk_dynamic_keymap/dmac.h>

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

LOG_MODULE_DECLARE(dmac_config, CONFIG_ZMK_DYNAMIC_KEYMAP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* Studio keymap-editor metadata: one parameter = the macro slot index.
 * Without this the behavior works but never appears in the behavior picker. */
static const struct behavior_parameter_value_metadata dmac_param_values[] = {
    {
        .display_name = "Slot",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
        .range = {.min = 0, .max = DM_SLOTS - 1},
    },
};

static const struct behavior_parameter_metadata_set dmac_param_metadata_set[] = {{
    .param1_values = dmac_param_values,
    .param1_values_len = ARRAY_SIZE(dmac_param_values),
}};

static const struct behavior_parameter_metadata dmac_metadata = {
    .sets_len = ARRAY_SIZE(dmac_param_metadata_set),
    .sets = dmac_param_metadata_set,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

/* Device name of the standard key-press behavior, used to emit each step. */
#define DM_KP_DEV DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))

static int on_dmac_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    const struct dm_slot *slot = dm_live_slot((uint8_t)binding->param1);
    if (!slot || slot->len == 0) {
        return ZMK_BEHAVIOR_OPAQUE; /* fail-open: nothing to play */
    }

    const uint32_t tap_ms = CONFIG_ZMK_DYNAMIC_KEYMAP_STEP_TAP_MS;
    const uint32_t gap_ms = CONFIG_ZMK_DYNAMIC_KEYMAP_STEP_GAP_MS;

    for (uint8_t i = 0; i < slot->len && i < DM_STEPS; i++) {
        struct zmk_behavior_binding kp = {
            .behavior_dev = DM_KP_DEV,
            .param1 = slot->steps[i].keycode,
            .param2 = 0,
        };
        switch (slot->steps[i].action) {
        case DM_ACT_PRESS:
            zmk_behavior_queue_add(&event, kp, true, gap_ms);
            break;
        case DM_ACT_RELEASE:
            zmk_behavior_queue_add(&event, kp, false, gap_ms);
            break;
        case DM_ACT_TAP:
        default:
            zmk_behavior_queue_add(&event, kp, true, tap_ms);
            zmk_behavior_queue_add(&event, kp, false, gap_ms);
            break;
        }
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_dmac_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api dmac_api = {
    .binding_pressed = on_dmac_pressed,
    .binding_released = on_dmac_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &dmac_metadata,
#endif
};

static int dmac_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

#define DMAC_INST(n)                                                                                \
    BEHAVIOR_DT_INST_DEFINE(n, dmac_init, NULL, NULL, NULL, POST_KERNEL,                            \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dmac_api);

DT_INST_FOREACH_STATUS_OKAY(DMAC_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
