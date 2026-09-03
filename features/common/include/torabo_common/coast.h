/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * torabo_common/coast.h — shared inertial-scroll ("coast") engine.
 *
 * Extracted verbatim (refactor phase 3, PLAN-ext-fw-refactor.md) from the two
 * previously byte-identical copies in features/trackpad/src/tp_pointer.c and
 * features/trackball/src/ztc_pointer.c. Every constant, rounding direction,
 * EMA update order and input_report_rel() call order is preserved exactly —
 * this header changes NOTHING about behavior, only where the code lives.
 *
 * WHAT: after the finger/ball stops moving, keep scrolling and let it die
 * away, instead of stopping dead the instant the last event arrives.
 *
 * WHERE IT RUNS: entirely on the CENTRAL. An input processor only executes
 * where its input listener does, and there is exactly one listener per
 * pointing device on the central — a peripheral-side sensor's events are
 * re-reported locally there by zmk_input_split before any processor sees
 * them. So the timer, the state and the synthetic events below never exist
 * on a peripheral build, and no split traffic is involved in coasting.
 *
 * HOW IT MEASURES: per axis, an EMA over "output wheel ticks per TICK_MS",
 * fed from the UNROUNDED result of the same divide the visible event gets.
 * Feeding it the rounded value would read 0 for any drag/roll slower than
 * one tick per event. The two axes share the user's numbers but keep
 * separate velocity, remainder and stop decisions, so a diagonal flick
 * decays along its own direction rather than snapping to an axis.
 *
 * HOW IT STOPS BEING A DRAG: there is no single universal "lifted" event to
 * hang this on (a ball never has one at all; a pad's tap/hold button is
 * handled separately by the caller before this engine sees anything), so the
 * trigger is an idle window: every scroll sample re-arms an IDLE_MS timer,
 * and only when that timer actually expires do we decide whether to glide.
 *
 * HOW IT EMITS: input_report_rel() back onto the SOURCE input device, from
 * the system work queue — exactly what upstream ZMK's own synthetic pointer
 * source (behavior_input_two_axis) does, so the threading and HID-report
 * assumptions are already proven in this codebase. Re-injecting (rather than
 * poking the HID endpoint directly) keeps the events serialised with real
 * ones by the input subsystem, and keeps smooth-scrolling resolution
 * multipliers, endpoint selection and every other listener behavior
 * identical to a real scroll. Re-entrancy is a non-issue by construction: we
 * emit REL_WHEEL / REL_HWHEEL, and each caller's own processor returns
 * CONTINUE untouched for any code that is not REL_X/REL_Y, so a coast event
 * can neither be re-scaled nor feed the velocity tracker.
 *
 * ARITHMETIC: Q16 fixed point in int32 throughout (no float on this path),
 * with a kept remainder so a velocity below one tick per period still
 * accumulates into occasional single ticks instead of vanishing.
 *
 * STRUCTURE OF THIS HEADER (host-test split, PLAN §Phase 3):
 *   1. Constants and the per-axis sample state (`struct torabo_coast_axis`).
 *   2. Pure functional core: torabo_scale_rem / torabo_coast_threshold_fp /
 *      torabo_coast_track / torabo_coast_step. No work-queue or input
 *      subsystem dependency — only CLAMP from <zephyr/sys/util.h>, which is
 *      stubbed for host builds in test/wire/stubs/. This is the part
 *      test/wire/test_coast.c compiles and drives on the host.
 *   3. OS-dependent scheduling: `struct torabo_coast_state`,
 *      torabo_coast_reset/cancel, torabo_coast_work_cb. Needs
 *      k_work_delayable, atomic_t, struct device and input_report_rel() —
 *      real firmware only. Skipped entirely when TORABO_COAST_CORE_ONLY is
 *      #defined before this header is included, so a host test can reach
 *      part 2 without stubbing the Zephyr work-queue / input subsystems.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h> /* CLAMP */

/* ---- 1. constants & the per-axis sample state ----------------------------- */

#define TORABO_COAST_TICK_MS 20u  /* emission period; 50 Hz, matches typical wheel rate */
#define TORABO_COAST_IDLE_MS 50u  /* quiet time that counts as "motion stopped" */
#define TORABO_COAST_DT_MIN_MS 4u /* floor on the inter-sample gap (bounds the estimate) */
#define TORABO_COAST_FP_SHIFT 16
#define TORABO_COAST_ONE (1 << TORABO_COAST_FP_SHIFT)
#define TORABO_COAST_VEL_MAX (64 * TORABO_COAST_ONE) /* keeps vel*retain inside int32 */
#define TORABO_COAST_EMA_SHIFT 2                      /* alpha = 1/4 */
#define TORABO_COAST_MIN_VEL (TORABO_COAST_ONE / 8)   /* floor: below this an axis is stopped */
#define TORABO_COAST_MAX_TICKS 250u                   /* hard 5 s cap; a glide always terminates */
#define TORABO_COAST_TICKS_PER_S (1000u / TORABO_COAST_TICK_MS)

enum torabo_coast_phase {
    TORABO_COAST_IDLE = 0,    /* nothing pending */
    TORABO_COAST_ARMED = 1,   /* scrolling; waiting to see if motion stopped */
    TORABO_COAST_RUNNING = 2, /* gliding; emitting synthetic wheel events */
};

struct torabo_coast_axis {
    int32_t vel;     /* Q16 output ticks per TORABO_COAST_TICK_MS */
    int32_t rem;     /* Q16 emission remainder */
    int64_t last_ms; /* uptime of the last sample ON THIS AXIS (dt is per axis:
                      * X and Y of one report share a millisecond) */
};

/* ---- 2. pure functional core (host-testable; no OS dependency) ----------- */

/* value/div with persisted remainder (mirrors ZMK scaler scale_val, mul=1). */
static inline int16_t torabo_scale_rem(int16_t value, uint8_t div, int16_t *rem) {
    if (div < 1) {
        div = 1;
    }
    int16_t num = (int16_t)(value + *rem);
    int16_t out = (int16_t)(num / (int16_t)div);
    *rem = (int16_t)(num - (int16_t)(out * (int16_t)div));
    return out;
}

/* Start threshold in wheel ticks/second -> Q16 ticks per emission period. */
static inline int32_t torabo_coast_threshold_fp(uint8_t per_s) {
    return (int32_t)(((uint32_t)per_s * (uint32_t)TORABO_COAST_ONE) / TORABO_COAST_TICKS_PER_S);
}

/* Feed one SCROLL sample. `units_fp` is the output value in Q16 wheel ticks. */
static inline void torabo_coast_track(struct torabo_coast_axis *ax, int32_t units_fp, int64_t now) {
    int64_t dt = now - ax->last_ms;
    /* A gap longer than the idle window is a NEW gesture, not a continuation. */
    const bool restart = (dt > (int64_t)TORABO_COAST_IDLE_MS) || (dt < 0);
    ax->last_ms = now;
    if (restart) {
        dt = TORABO_COAST_IDLE_MS; /* unknowable; the window is the conservative guess */
    } else if (dt < (int64_t)TORABO_COAST_DT_MIN_MS) {
        dt = TORABO_COAST_DT_MIN_MS;
    }
    int64_t sample = ((int64_t)units_fp * (int64_t)TORABO_COAST_TICK_MS) / dt;
    sample = CLAMP(sample, -(int64_t)TORABO_COAST_VEL_MAX, (int64_t)TORABO_COAST_VEL_MAX);
    /* Seed the average from this sample on a restart rather than easing in from a
     * stale one, so the first flick after a pause is not read as half speed. */
    ax->vel = restart ? (int32_t)sample
                      : ax->vel + (int32_t)((sample - (int64_t)ax->vel) >> TORABO_COAST_EMA_SHIFT);
}

/* One decay step; returns the wheel ticks to emit for this axis this period. */
static inline int16_t torabo_coast_step(struct torabo_coast_axis *ax, int32_t retain) {
    if (ax->vel == 0) {
        ax->rem = 0;
        return 0;
    }
    ax->vel = (int32_t)(((int64_t)ax->vel * retain) / 256); /* truncates toward 0 */
    if (ax->vel > -TORABO_COAST_MIN_VEL && ax->vel < TORABO_COAST_MIN_VEL) {
        ax->vel = 0; /* below the perceptible floor: this axis is done */
        ax->rem = 0;
        return 0;
    }
    int32_t acc = ax->vel + ax->rem;
    int32_t out = acc / TORABO_COAST_ONE; /* toward zero => symmetric in both directions */
    ax->rem = acc - out * TORABO_COAST_ONE;
    return (int16_t)CLAMP(out, INT16_MIN, INT16_MAX);
}

#ifndef TORABO_COAST_CORE_ONLY
/* ---- 3. OS-dependent scheduling (real firmware only) ----------------------
 * Needs k_work_delayable / atomic_t / struct device / input_report_rel().
 * #define TORABO_COAST_CORE_ONLY before #include-ing this header to skip this
 * whole section (see part 2's doc comment and test/wire/test_coast.c).
 */
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

struct torabo_coast_state {
    struct k_work_delayable work;
    const struct device *src; /* input device to re-inject on (captured from events) */
    struct torabo_coast_axis x, y;
    atomic_t phase;    /* enum torabo_coast_phase */
    uint16_t ticks;    /* ticks emitted in the current glide */
    uint8_t friction;  /* snapshotted at glide start, so a live edit can't jolt it */
    uint8_t threshold; /* ditto */
};

static inline void torabo_coast_reset(struct torabo_coast_state *c) {
    atomic_set(&c->phase, TORABO_COAST_IDLE);
    c->x.vel = c->x.rem = 0;
    c->y.vel = c->y.rem = 0;
    c->ticks = 0;
}

static inline void torabo_coast_cancel(struct torabo_coast_state *c) {
    /* Order matters: drop the phase FIRST, so a tick that is already queued or
     * already running observes IDLE and bails; the cancel then only has to stop a
     * not-yet-due reschedule. The residual race can leak at most one tick's worth
     * of wheel motion, which is a fraction of a line. */
    torabo_coast_reset(c);
    (void)k_work_cancel_delayable(&c->work);
}

static inline void torabo_coast_work_cb(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct torabo_coast_state *c = CONTAINER_OF(dwork, struct torabo_coast_state, work);

    switch (atomic_get(&c->phase)) {
    case TORABO_COAST_ARMED: {
        /* Motion stopped. Glide only if it was still moving fast enough. The
         * threshold is checked on the faster axis and applied to the pair, so a
         * diagonal flick keeps its direction instead of losing its minor axis. */
        const int32_t thr = torabo_coast_threshold_fp(c->threshold);
        const int32_t mx = (c->x.vel < 0) ? -c->x.vel : c->x.vel;
        const int32_t my = (c->y.vel < 0) ? -c->y.vel : c->y.vel;
        if (!c->src || (mx < thr && my < thr)) {
            torabo_coast_reset(c);
            return;
        }
        if (mx < TORABO_COAST_MIN_VEL) {
            c->x.vel = 0;
        }
        if (my < TORABO_COAST_MIN_VEL) {
            c->y.vel = 0;
        }
        c->x.rem = c->y.rem = 0;
        c->ticks = 0;
        atomic_set(&c->phase, TORABO_COAST_RUNNING);
        k_work_reschedule(&c->work, K_MSEC(TORABO_COAST_TICK_MS));
        return;
    }
    case TORABO_COAST_RUNNING:
        break;
    default:
        return; /* cancelled after this run was queued */
    }

    /* friction 1..32 => keep (256-3f)/256 of the velocity each period, i.e. 0.988
     * (a long glide, seconds) down to 0.625 (stops in ~0.2 s). */
    const int32_t retain = 256 - 3 * (int32_t)c->friction;
    const int16_t out_x = torabo_coast_step(&c->x, retain);
    const int16_t out_y = torabo_coast_step(&c->y, retain);

    /* sync on the LAST value of the pair, so both axes land in one HID report. */
    if (out_x != 0) {
        (void)input_report_rel(c->src, INPUT_REL_HWHEEL, out_x, out_y == 0, K_NO_WAIT);
    }
    if (out_y != 0) {
        (void)input_report_rel(c->src, INPUT_REL_WHEEL, out_y, true, K_NO_WAIT);
    }

    if ((c->x.vel != 0 || c->y.vel != 0) && ++c->ticks < TORABO_COAST_MAX_TICKS) {
        k_work_reschedule(&c->work, K_MSEC(TORABO_COAST_TICK_MS));
    } else {
        torabo_coast_reset(c);
    }
}

#endif /* TORABO_COAST_CORE_ONLY */
