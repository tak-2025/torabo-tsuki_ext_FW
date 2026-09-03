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
 *
 * v3 adds inertial scroll ("coast") to the SCROLL role only — see the block
 * comment above the engine below. MOVE and OFF are untouched by it.
 */

#define DT_DRV_COMPAT zmk_input_processor_ztc_pointer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>

#include <zmk/keymap.h>
#include <torabo_common/coast.h>
#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

/* ---- inertial scroll ("coast") -------------------------------------------
 * v3 adds inertial scroll ("coast") to the SCROLL role only; MOVE and OFF are
 * untouched by it. The engine (constants, arithmetic and the work-queue
 * scheduling) is byte-identical to trackpad's and lives in the shared header
 * torabo_common/coast.h (refactor phase 3) — see that header's doc comment
 * for the full design rationale (why it runs only on the central, how it
 * measures/starts/stops/emits, the Q16 arithmetic).
 */

struct ztc_pointer_data {
    int16_t rem_x;
    int16_t rem_y;
    uint8_t role_x, role_y; /* last role seen per axis (cancel the glide on change) */
    struct torabo_coast_state coast;
};

static int ztc_pointer_handle_event(const struct device *dev, struct input_event *event,
                                    uint32_t param1, uint32_t param2,
                                    struct zmk_input_processor_state *state) {
    struct ztc_pointer_data *data = dev->data;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    const bool is_x = (event->code == INPUT_REL_X);
    const bool is_y = (event->code == INPUT_REL_Y);
    if (!is_x && !is_y) {
        /* Includes our own synthetic REL_WHEEL / REL_HWHEEL: passed straight
         * through, never re-scaled, never fed back into the velocity tracker. */
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Real ball motion of ANY role ends a glide immediately. */
    if (atomic_get(&data->coast.phase) == TORABO_COAST_RUNNING) {
        torabo_coast_cancel(&data->coast);
    }

    const struct ztc_snapshot *s = ztc_live();
    const uint8_t layer = zmk_keymap_highest_layer_active(); /* index */
    const struct ztc_axis_cfg a = ztc_axis_for(s, layer, is_x);
    const struct ztc_coast_cfg cc = ztc_coast_for(s);

    uint8_t *role_seen = is_x ? &data->role_x : &data->role_y;
    if (*role_seen != a.role) { /* role changed (layer switch etc.): drop stale state */
        *role_seen = a.role;
        torabo_coast_cancel(&data->coast); /* velocity from another role means nothing here */
    }

    int16_t v = event->value;
    if (a.direction) {
        v = (v == INT16_MIN) ? INT16_MAX : (int16_t)-v; /* guard overflow */
    }
    const int16_t raw = v; /* pre-divide, post-direction: the coast tracker's input */
    v = torabo_scale_rem(v, a.speed_div, is_x ? &data->rem_x : &data->rem_y);
    event->value = v;

    switch (a.role) {
    case ZTC_ROLE_SCROLL: {
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
        const uint8_t div = (a.speed_div < ZTC_SPEED_MIN) ? (uint8_t)ZTC_SPEED_MIN : a.speed_div;
        if (cc.enable) {
            /* Track the UNROUNDED output velocity (Q16 wheel ticks), then re-arm
             * the idle detector; the glide itself starts only if that timer
             * actually fires. Cheap enough to do on every scroll event. */
            data->coast.src = event->dev;
            data->coast.friction = cc.friction;
            data->coast.threshold = cc.threshold;
            torabo_coast_track(is_x ? &data->coast.x : &data->coast.y,
                               (int32_t)(((int64_t)raw * TORABO_COAST_ONE) / div), k_uptime_get());
            atomic_set(&data->coast.phase, TORABO_COAST_ARMED);
            k_work_reschedule(&data->coast.work, K_MSEC(TORABO_COAST_IDLE_MS));
        } else if (atomic_get(&data->coast.phase) != TORABO_COAST_IDLE) {
            torabo_coast_cancel(&data->coast); /* switched off from the app mid-scroll */
        }
        break;
    }
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

/* Per-instance init: only the coast timer needs it; everything else is zeroed. */
static int ztc_pointer_init(const struct device *dev) {
    struct ztc_pointer_data *data = dev->data;
    k_work_init_delayable(&data->coast.work, torabo_coast_work_cb);
    return 0;
}

#define ZTC_POINTER_INST(n)                                                                        \
    static struct ztc_pointer_data ztc_pointer_data_##n;                                           \
    DEVICE_DT_INST_DEFINE(n, ztc_pointer_init, NULL, &ztc_pointer_data_##n, NULL, POST_KERNEL,     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ztc_pointer_api);

DT_INST_FOREACH_STATUS_OKAY(ZTC_POINTER_INST)
