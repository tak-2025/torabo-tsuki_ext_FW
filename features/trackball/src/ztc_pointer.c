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
#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

/* ---- inertial scroll ("coast") -------------------------------------------
 *
 * WHAT: after the ball stops being turned, keep scrolling and let it die away,
 * instead of stopping dead the instant the last event arrives.
 *
 * WHERE IT RUNS: entirely on the CENTRAL. An input processor only executes where
 * its input listener does, and there is exactly one listener per pointing device
 * on the central — a peripheral-side sensor's events are re-reported locally
 * there by zmk_input_split before any processor sees them. So the timer, the
 * state and the synthetic events below never exist on a peripheral build, and no
 * split traffic is involved in coasting.
 *
 * HOW IT MEASURES: per axis, an EMA over "output wheel ticks per TICK_MS", fed
 * from the UNROUNDED result of the same divide the visible event gets. Feeding it
 * the rounded value would read 0 for any roll slower than one tick per event. The
 * two axes share the user's numbers but keep separate velocity, remainder and
 * stop decisions, so a diagonal flick decays along its own direction rather than
 * snapping to an axis.
 *
 * WHEN IT STARTS: a ball has no "let go" event, so the trigger is an idle window:
 * every scroll sample re-arms an IDLE_MS timer, and only when that timer actually
 * expires do we decide whether to glide.
 *
 * HOW IT EMITS: input_report_rel() back onto the SOURCE input device, from the
 * system work queue — exactly what upstream ZMK's own synthetic pointer source
 * (behavior_input_two_axis) does, so the threading and HID-report assumptions are
 * already proven in this codebase. Re-injecting (rather than poking the HID
 * endpoint directly) keeps the events serialised with real ones by the input
 * subsystem, and keeps smooth-scrolling resolution multipliers, endpoint
 * selection and every other listener behavior identical to a real scroll.
 * Re-entrancy is a non-issue by construction: we emit REL_WHEEL / REL_HWHEEL, and
 * this processor returns CONTINUE untouched for any code that is not REL_X/REL_Y,
 * so a coast event can neither be re-scaled nor feed the velocity tracker.
 *
 * ARITHMETIC: Q16 fixed point in int32 throughout (no float on this path), with
 * a kept remainder so a velocity below one tick per period still accumulates into
 * occasional single ticks instead of vanishing.
 */
#define ZTC_COAST_TICK_MS 20u  /* emission period; 50 Hz, matches typical wheel rate */
#define ZTC_COAST_IDLE_MS 50u  /* quiet time that counts as "the ball stopped" */
#define ZTC_COAST_DT_MIN_MS 4u /* floor on the inter-sample gap (bounds the estimate) */
#define ZTC_COAST_FP_SHIFT 16
#define ZTC_COAST_ONE (1 << ZTC_COAST_FP_SHIFT)
#define ZTC_COAST_VEL_MAX (64 * ZTC_COAST_ONE) /* keeps vel*retain inside int32 */
#define ZTC_COAST_EMA_SHIFT 2                  /* alpha = 1/4 */
#define ZTC_COAST_MIN_VEL (ZTC_COAST_ONE / 8)  /* floor: below this an axis is stopped */
#define ZTC_COAST_MAX_TICKS 250u               /* hard 5 s cap; a glide always terminates */
#define ZTC_COAST_TICKS_PER_S (1000u / ZTC_COAST_TICK_MS)

enum ztc_coast_phase {
    ZTC_COAST_IDLE = 0,    /* nothing pending */
    ZTC_COAST_ARMED = 1,   /* scrolling; waiting to see if the ball stopped */
    ZTC_COAST_RUNNING = 2, /* gliding; emitting synthetic wheel events */
};

struct ztc_coast_axis {
    int32_t vel;     /* Q16 output ticks per ZTC_COAST_TICK_MS */
    int32_t rem;     /* Q16 emission remainder */
    int64_t last_ms; /* uptime of the last sample ON THIS AXIS (dt is per axis:
                      * X and Y of one report share a millisecond) */
};

struct ztc_coast_state {
    struct k_work_delayable work;
    const struct device *src; /* input device to re-inject on (captured from events) */
    struct ztc_coast_axis x, y;
    atomic_t phase;    /* enum ztc_coast_phase */
    uint16_t ticks;    /* ticks emitted in the current glide */
    uint8_t friction;  /* snapshotted at glide start, so a live edit can't jolt it */
    uint8_t threshold; /* ditto */
};

struct ztc_pointer_data {
    int16_t rem_x;
    int16_t rem_y;
    uint8_t role_x, role_y; /* last role seen per axis (cancel the glide on change) */
    struct ztc_coast_state coast;
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

static void coast_reset(struct ztc_coast_state *c) {
    atomic_set(&c->phase, ZTC_COAST_IDLE);
    c->x.vel = c->x.rem = 0;
    c->y.vel = c->y.rem = 0;
    c->ticks = 0;
}

static void coast_cancel(struct ztc_coast_state *c) {
    /* Order matters: drop the phase FIRST, so a tick that is already queued or
     * already running observes IDLE and bails; the cancel then only has to stop a
     * not-yet-due reschedule. The residual race can leak at most one tick's worth
     * of wheel motion, which is a fraction of a line. */
    coast_reset(c);
    (void)k_work_cancel_delayable(&c->work);
}

/* Start threshold in wheel ticks/second -> Q16 ticks per emission period. */
static inline int32_t coast_threshold_fp(uint8_t per_s) {
    return (int32_t)(((uint32_t)per_s * (uint32_t)ZTC_COAST_ONE) / ZTC_COAST_TICKS_PER_S);
}

/* Feed one SCROLL sample. `units_fp` is the output value in Q16 wheel ticks. */
static void coast_track(struct ztc_coast_axis *ax, int32_t units_fp, int64_t now) {
    int64_t dt = now - ax->last_ms;
    /* A gap longer than the idle window is a NEW gesture, not a continuation. */
    const bool restart = (dt > (int64_t)ZTC_COAST_IDLE_MS) || (dt < 0);
    ax->last_ms = now;
    if (restart) {
        dt = ZTC_COAST_IDLE_MS; /* unknowable; the window is the conservative guess */
    } else if (dt < (int64_t)ZTC_COAST_DT_MIN_MS) {
        dt = ZTC_COAST_DT_MIN_MS;
    }
    int64_t sample = ((int64_t)units_fp * (int64_t)ZTC_COAST_TICK_MS) / dt;
    sample = CLAMP(sample, -(int64_t)ZTC_COAST_VEL_MAX, (int64_t)ZTC_COAST_VEL_MAX);
    /* Seed the average from this sample on a restart rather than easing in from a
     * stale one, so the first flick after a pause is not read as half speed. */
    ax->vel = restart ? (int32_t)sample
                      : ax->vel + (int32_t)((sample - (int64_t)ax->vel) >> ZTC_COAST_EMA_SHIFT);
}

/* One decay step; returns the wheel ticks to emit for this axis this period. */
static int16_t coast_step(struct ztc_coast_axis *ax, int32_t retain) {
    if (ax->vel == 0) {
        ax->rem = 0;
        return 0;
    }
    ax->vel = (int32_t)(((int64_t)ax->vel * retain) / 256); /* truncates toward 0 */
    if (ax->vel > -ZTC_COAST_MIN_VEL && ax->vel < ZTC_COAST_MIN_VEL) {
        ax->vel = 0; /* below the perceptible floor: this axis is done */
        ax->rem = 0;
        return 0;
    }
    int32_t acc = ax->vel + ax->rem;
    int32_t out = acc / ZTC_COAST_ONE; /* toward zero => symmetric in both directions */
    ax->rem = acc - out * ZTC_COAST_ONE;
    return (int16_t)CLAMP(out, INT16_MIN, INT16_MAX);
}

static void ztc_coast_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct ztc_coast_state *c = CONTAINER_OF(dwork, struct ztc_coast_state, work);

    switch (atomic_get(&c->phase)) {
    case ZTC_COAST_ARMED: {
        /* The ball went quiet. Glide only if it was still moving fast enough. The
         * threshold is checked on the faster axis and applied to the pair, so a
         * diagonal flick keeps its direction instead of losing its minor axis. */
        const int32_t thr = coast_threshold_fp(c->threshold);
        const int32_t mx = (c->x.vel < 0) ? -c->x.vel : c->x.vel;
        const int32_t my = (c->y.vel < 0) ? -c->y.vel : c->y.vel;
        if (!c->src || (mx < thr && my < thr)) {
            coast_reset(c);
            return;
        }
        if (mx < ZTC_COAST_MIN_VEL) {
            c->x.vel = 0;
        }
        if (my < ZTC_COAST_MIN_VEL) {
            c->y.vel = 0;
        }
        c->x.rem = c->y.rem = 0;
        c->ticks = 0;
        atomic_set(&c->phase, ZTC_COAST_RUNNING);
        k_work_reschedule(&c->work, K_MSEC(ZTC_COAST_TICK_MS));
        return;
    }
    case ZTC_COAST_RUNNING:
        break;
    default:
        return; /* cancelled after this run was queued */
    }

    /* friction 1..32 => keep (256-3f)/256 of the velocity each period, i.e. 0.988
     * (a long glide, seconds) down to 0.625 (stops in ~0.2 s). */
    const int32_t retain = 256 - 3 * (int32_t)c->friction;
    const int16_t out_x = coast_step(&c->x, retain);
    const int16_t out_y = coast_step(&c->y, retain);

    /* sync on the LAST value of the pair, so both axes land in one HID report. */
    if (out_x != 0) {
        (void)input_report_rel(c->src, INPUT_REL_HWHEEL, out_x, out_y == 0, K_NO_WAIT);
    }
    if (out_y != 0) {
        (void)input_report_rel(c->src, INPUT_REL_WHEEL, out_y, true, K_NO_WAIT);
    }

    if ((c->x.vel != 0 || c->y.vel != 0) && ++c->ticks < ZTC_COAST_MAX_TICKS) {
        k_work_reschedule(&c->work, K_MSEC(ZTC_COAST_TICK_MS));
    } else {
        coast_reset(c);
    }
}

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
    if (atomic_get(&data->coast.phase) == ZTC_COAST_RUNNING) {
        coast_cancel(&data->coast);
    }

    const struct ztc_snapshot *s = ztc_live();
    const uint8_t layer = zmk_keymap_highest_layer_active(); /* index */
    const struct ztc_axis_cfg a = ztc_axis_for(s, layer, is_x);
    const struct ztc_coast_cfg cc = ztc_coast_for(s);

    uint8_t *role_seen = is_x ? &data->role_x : &data->role_y;
    if (*role_seen != a.role) { /* role changed (layer switch etc.): drop stale state */
        *role_seen = a.role;
        coast_cancel(&data->coast); /* velocity from another role means nothing here */
    }

    int16_t v = event->value;
    if (a.direction) {
        v = (v == INT16_MIN) ? INT16_MAX : (int16_t)-v; /* guard overflow */
    }
    const int16_t raw = v; /* pre-divide, post-direction: the coast tracker's input */
    v = scale_rem(v, a.speed_div, is_x ? &data->rem_x : &data->rem_y);
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
            coast_track(is_x ? &data->coast.x : &data->coast.y,
                        (int32_t)(((int64_t)raw * ZTC_COAST_ONE) / div), k_uptime_get());
            atomic_set(&data->coast.phase, ZTC_COAST_ARMED);
            k_work_reschedule(&data->coast.work, K_MSEC(ZTC_COAST_IDLE_MS));
        } else if (atomic_get(&data->coast.phase) != ZTC_COAST_IDLE) {
            coast_cancel(&data->coast); /* switched off from the app mid-scroll */
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
    k_work_init_delayable(&data->coast.work, ztc_coast_work_cb);
    return 0;
}

#define ZTC_POINTER_INST(n)                                                                        \
    static struct ztc_pointer_data ztc_pointer_data_##n;                                           \
    DEVICE_DT_INST_DEFINE(n, ztc_pointer_init, NULL, &ztc_pointer_data_##n, NULL, POST_KERNEL,     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ztc_pointer_api);

DT_INST_FOREACH_STATUS_OKAY(ZTC_POINTER_INST)
