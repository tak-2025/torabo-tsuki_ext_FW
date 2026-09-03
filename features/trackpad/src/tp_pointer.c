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
 *
 * v3 adds inertial scroll ("coast") to the SCROLL role only — see the block
 * comment above the engine below. MOVE / OFF / ENCODER are untouched by it.
 */

#define DT_DRV_COMPAT zmk_input_processor_tp_pointer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
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

/* ---- inertial scroll ("coast") -------------------------------------------
 *
 * WHAT: after the finger leaves the pad, keep scrolling and let it die away,
 * instead of stopping dead the instant the last event arrives.
 *
 * WHERE IT RUNS: entirely on the CENTRAL. An input processor only executes where
 * its input listener does, and there is exactly one listener per pointing device
 * on the central — a peripheral-side pad's events are re-reported locally there
 * by zmk_input_split before any processor sees them. So the timer, the state and
 * the synthetic events below never exist on a peripheral build, and no split
 * traffic is involved in coasting.
 *
 * HOW IT MEASURES: per axis, an EMA over "output wheel ticks per TICK_MS", fed
 * from the UNROUNDED result of the same divide the visible event gets. Feeding it
 * the rounded value would read 0 for any drag slower than one tick per event. The
 * two axes share the user's numbers but keep separate velocity, remainder and
 * stop decisions, so a diagonal flick decays along its own direction rather than
 * snapping to an axis.
 *
 * HOW IT STOPS BEING A DRAG: there is no "finger lifted" event to hang this on
 * (the pad simply goes quiet), so the trigger is an idle window: every scroll
 * sample re-arms a IDLE_MS timer, and only when that timer actually expires do we
 * decide whether to glide. The same mechanism serves the trackball, where "idle"
 * is the only possible signal.
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
#define TP_COAST_TICK_MS 20u   /* emission period; 50 Hz, matches typical wheel rate */
#define TP_COAST_IDLE_MS 50u   /* quiet time that counts as "the finger left" */
#define TP_COAST_DT_MIN_MS 4u  /* floor on the inter-sample gap (bounds the estimate) */
#define TP_COAST_FP_SHIFT 16
#define TP_COAST_ONE (1 << TP_COAST_FP_SHIFT)
#define TP_COAST_VEL_MAX (64 * TP_COAST_ONE) /* keeps vel*retain inside int32 */
#define TP_COAST_EMA_SHIFT 2                 /* alpha = 1/4 */
#define TP_COAST_MIN_VEL (TP_COAST_ONE / 8)  /* floor: below this an axis is stopped */
#define TP_COAST_MAX_TICKS 250u              /* hard 5 s cap; a glide always terminates */
#define TP_COAST_TICKS_PER_S (1000u / TP_COAST_TICK_MS)

enum tp_coast_phase {
    TP_COAST_IDLE = 0,    /* nothing pending */
    TP_COAST_ARMED = 1,   /* scrolling; waiting to see if the finger left */
    TP_COAST_RUNNING = 2, /* gliding; emitting synthetic wheel events */
};

struct tp_coast_axis {
    int32_t vel;     /* Q16 output ticks per TP_COAST_TICK_MS */
    int32_t rem;     /* Q16 emission remainder */
    int64_t last_ms; /* uptime of the last sample ON THIS AXIS (dt is per axis:
                      * X and Y of one report share a millisecond) */
};

struct tp_coast_state {
    struct k_work_delayable work;
    const struct device *src; /* input device to re-inject on (captured from events) */
    struct tp_coast_axis x, y;
    atomic_t phase;    /* enum tp_coast_phase */
    uint16_t ticks;    /* ticks emitted in the current glide */
    uint8_t friction;  /* snapshotted at glide start, so a live edit can't jolt it */
    uint8_t threshold; /* ditto */
};

struct tp_pointer_data {
    int16_t rem_x, rem_y;   /* continuous scaling remainder, per axis */
    int32_t acc_x, acc_y;   /* discrete accumulator, per axis */
    uint8_t role_x, role_y; /* last role seen per axis (reset accum on change) */
    struct tp_coast_state coast;
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

static void coast_reset(struct tp_coast_state *c) {
    atomic_set(&c->phase, TP_COAST_IDLE);
    c->x.vel = c->x.rem = 0;
    c->y.vel = c->y.rem = 0;
    c->ticks = 0;
}

static void coast_cancel(struct tp_coast_state *c) {
    /* Order matters: drop the phase FIRST, so a tick that is already queued or
     * already running observes IDLE and bails; the cancel then only has to stop a
     * not-yet-due reschedule. The residual race can leak at most one tick's worth
     * of wheel motion, which is a fraction of a line. */
    coast_reset(c);
    (void)k_work_cancel_delayable(&c->work);
}

/* Start threshold in wheel ticks/second -> Q16 ticks per emission period. */
static inline int32_t coast_threshold_fp(uint8_t per_s) {
    return (int32_t)(((uint32_t)per_s * (uint32_t)TP_COAST_ONE) / TP_COAST_TICKS_PER_S);
}

/* Feed one SCROLL sample. `units_fp` is the output value in Q16 wheel ticks. */
static void coast_track(struct tp_coast_axis *ax, int32_t units_fp, int64_t now) {
    int64_t dt = now - ax->last_ms;
    /* A gap longer than the idle window is a NEW gesture, not a continuation. */
    const bool restart = (dt > (int64_t)TP_COAST_IDLE_MS) || (dt < 0);
    ax->last_ms = now;
    if (restart) {
        dt = TP_COAST_IDLE_MS; /* unknowable; the window is the conservative guess */
    } else if (dt < (int64_t)TP_COAST_DT_MIN_MS) {
        dt = TP_COAST_DT_MIN_MS;
    }
    int64_t sample = ((int64_t)units_fp * (int64_t)TP_COAST_TICK_MS) / dt;
    sample = CLAMP(sample, -(int64_t)TP_COAST_VEL_MAX, (int64_t)TP_COAST_VEL_MAX);
    /* Seed the average from this sample on a restart rather than easing in from a
     * stale one, so the first flick after a pause is not read as half speed. */
    ax->vel = restart ? (int32_t)sample
                      : ax->vel + (int32_t)((sample - (int64_t)ax->vel) >> TP_COAST_EMA_SHIFT);
}

/* One decay step; returns the wheel ticks to emit for this axis this period. */
static int16_t coast_step(struct tp_coast_axis *ax, int32_t retain) {
    if (ax->vel == 0) {
        ax->rem = 0;
        return 0;
    }
    ax->vel = (int32_t)(((int64_t)ax->vel * retain) / 256); /* truncates toward 0 */
    if (ax->vel > -TP_COAST_MIN_VEL && ax->vel < TP_COAST_MIN_VEL) {
        ax->vel = 0; /* below the perceptible floor: this axis is done */
        ax->rem = 0;
        return 0;
    }
    int32_t acc = ax->vel + ax->rem;
    int32_t out = acc / TP_COAST_ONE; /* toward zero => symmetric in both directions */
    ax->rem = acc - out * TP_COAST_ONE;
    return (int16_t)CLAMP(out, INT16_MIN, INT16_MAX);
}

static void tp_coast_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct tp_coast_state *c = CONTAINER_OF(dwork, struct tp_coast_state, work);

    switch (atomic_get(&c->phase)) {
    case TP_COAST_ARMED: {
        /* The pad went quiet. Glide only if it was still moving fast enough. The
         * threshold is checked on the faster axis and applied to the pair, so a
         * diagonal flick keeps its direction instead of losing its minor axis. */
        const int32_t thr = coast_threshold_fp(c->threshold);
        const int32_t mx = (c->x.vel < 0) ? -c->x.vel : c->x.vel;
        const int32_t my = (c->y.vel < 0) ? -c->y.vel : c->y.vel;
        if (!c->src || (mx < thr && my < thr)) {
            coast_reset(c);
            return;
        }
        if (mx < TP_COAST_MIN_VEL) {
            c->x.vel = 0;
        }
        if (my < TP_COAST_MIN_VEL) {
            c->y.vel = 0;
        }
        c->x.rem = c->y.rem = 0;
        c->ticks = 0;
        atomic_set(&c->phase, TP_COAST_RUNNING);
        k_work_reschedule(&c->work, K_MSEC(TP_COAST_TICK_MS));
        return;
    }
    case TP_COAST_RUNNING:
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

    if ((c->x.vel != 0 || c->y.vel != 0) && ++c->ticks < TP_COAST_MAX_TICKS) {
        k_work_reschedule(&c->work, K_MSEC(TP_COAST_TICK_MS));
    } else {
        coast_reset(c);
    }
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
    const struct tp_pointer_config *cfg = dev->config;
    struct tp_pointer_data *data = dev->data;

    if (event->type == INPUT_EV_KEY) {
        /* A finger coming back down (the driver's tap/hold button) should kill a
         * glide even before it moves. What the button DOES is tp_keys' business;
         * here it is only a "the user is on the pad again" signal. */
        if (event->value && atomic_get(&data->coast.phase) == TP_COAST_RUNNING) {
            coast_cancel(&data->coast);
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }
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

    /* Real pointer motion of ANY role ends a glide immediately. */
    if (atomic_get(&data->coast.phase) == TP_COAST_RUNNING) {
        coast_cancel(&data->coast);
    }

    const struct tp_snapshot *s = tp_live();
    const uint8_t layer = zmk_keymap_highest_layer_active(); /* index */
    const struct tp_axis_cfg a = tp_axis_for(s, cfg->device_id, layer, is_x);
    const struct tp_coast_cfg cc = tp_coast_for(s, cfg->device_id);

    int16_t *rem = is_x ? &data->rem_x : &data->rem_y;
    int32_t *acc = is_x ? &data->acc_x : &data->acc_y;
    uint8_t *role_seen = is_x ? &data->role_x : &data->role_y;
    if (*role_seen != a.role) { /* role changed (layer switch etc.): drop stale state */
        *rem = 0;
        *acc = 0;
        *role_seen = a.role;
        coast_cancel(&data->coast); /* velocity from another role means nothing here */
    }

    int16_t v = event->value;
    if (a.direction) {
        v = (v == INT16_MIN) ? INT16_MAX : (int16_t)-v; /* guard overflow */
    }

    switch (a.role) {
    case TP_ROLE_MOVE:
        event->value = scale_rem(v, a.step, rem);
        return ZMK_INPUT_PROC_CONTINUE; /* keep REL_X / REL_Y */
    case TP_ROLE_SCROLL: {
        const uint8_t div = (a.step < TP_STEP_MIN) ? (uint8_t)TP_STEP_MIN : a.step;
        event->value = scale_rem(v, a.step, rem);
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
        if (cc.enable) {
            /* Track the UNROUNDED output velocity (Q16 wheel ticks), then re-arm
             * the lift detector; the glide itself starts only if that timer
             * actually fires. Cheap enough to do on every scroll event. */
            data->coast.src = event->dev;
            data->coast.friction = cc.friction;
            data->coast.threshold = cc.threshold;
            coast_track(is_x ? &data->coast.x : &data->coast.y,
                        (int32_t)(((int64_t)v * TP_COAST_ONE) / div), k_uptime_get());
            atomic_set(&data->coast.phase, TP_COAST_ARMED);
            k_work_reschedule(&data->coast.work, K_MSEC(TP_COAST_IDLE_MS));
        } else if (atomic_get(&data->coast.phase) != TP_COAST_IDLE) {
            coast_cancel(&data->coast); /* switched off from the app mid-scroll */
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }
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

/* Per-instance init: only the coast timer needs it; everything else is zeroed. */
static int tp_pointer_init(const struct device *dev) {
    struct tp_pointer_data *data = dev->data;
    k_work_init_delayable(&data->coast.work, tp_coast_work_cb);
    return 0;
}

#define TP_POINTER_INST(n)                                                                         \
    TP_BEH_REFS_DEFINE(tp_pointer_beh_refs_##n, n);                                                 \
    static struct tp_pointer_data tp_pointer_data_##n;                                              \
    static const struct tp_pointer_config tp_pointer_config_##n = {                                \
        .index = n,                                                                                \
        .device_id = DT_INST_PROP(n, device_id),                                                   \
        .beh_refs = tp_pointer_beh_refs_##n,                                                        \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, tp_pointer_init, NULL, &tp_pointer_data_##n,                          \
                          &tp_pointer_config_##n, POST_KERNEL,                                     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tp_pointer_api);

DT_INST_FOREACH_STATUS_OKAY(TP_POINTER_INST)
