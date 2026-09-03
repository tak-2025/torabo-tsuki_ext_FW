/*
 * Host-test stub for <zmk/behavior.h> (zmk/app/include/zmk/behavior.h).
 *
 * Field order/types copied from upstream. CONFIG_ZMK_BEHAVIOR_LOCAL_IDS_IN_BINDINGS
 * is left off here, matching the branch combo_state.c takes without it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define ZMK_BEHAVIOR_OPAQUE 0
#define ZMK_BEHAVIOR_TRANSPARENT 1

typedef uint16_t zmk_behavior_local_id_t;

struct zmk_behavior_binding {
    const char *behavior_dev;
    uint32_t param1;
    uint32_t param2;
};

struct zmk_behavior_binding_event {
    int layer;
    uint32_t position;
    int64_t timestamp;
};
