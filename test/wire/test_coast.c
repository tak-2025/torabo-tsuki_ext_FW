/*
 * coast engine ("inertial scroll") — host golden tests (refactor phase 3).
 *
 * Two things are checked:
 *
 *  1. Direct, hand-checkable assertions on the pure functional core
 *     (torabo_scale_rem / torabo_coast_threshold_fp / torabo_coast_track /
 *     torabo_coast_step): a handful of exact input->output/state pairs, plus
 *     basic invariants (decay reaches zero, terminates well inside
 *     TORABO_COAST_MAX_TICKS, an idle axis is a no-op).
 *
 *  2. A golden PARITY check against `ref_*`, a byte-for-byte copy of the
 *     PRE-extraction arithmetic (as it stood on
 *     refactor/phase2-internal-dedup, i.e. before tp_pointer.c / ztc_pointer.c
 *     shared this code by copy-paste rather than by #include). Thousands of
 *     pseudo-random steps across representative input shapes (steady
 *     velocity, acceleration, deceleration, sign flips, zero/idle-mixed
 *     bursts) are fed to both implementations in lockstep; every intermediate
 *     state (vel/rem/last_ms) and every step's emitted tick count must match
 *     exactly. This is the proof that extracting the engine into
 *     torabo_common/coast.h did not change 1 bit of behavior.
 *
 * TORABO_COAST_CORE_ONLY keeps this file (and the header it includes) free of
 * any Zephyr kernel / input-subsystem dependency: only the pure math (part 2
 * of coast.h — constants, `struct torabo_coast_axis`, torabo_scale_rem,
 * torabo_coast_threshold_fp, torabo_coast_track, torabo_coast_step) is
 * compiled. See coast.h's own doc comment for the full split rationale.
 */
#define TORABO_COAST_CORE_ONLY
#include <torabo_common/coast.h>

#include <stdint.h>

#include "torabo_test.h"

/* ---- reference implementation: verbatim pre-phase-3 arithmetic -----------
 * Copied from features/trackpad/src/tp_pointer.c as it stood on
 * refactor/phase2-internal-dedup (before the coast engine was extracted):
 *   git show refactor/phase2-internal-dedup:features/trackpad/src/tp_pointer.c
 * Renamed REF_* / ref_* only — not one arithmetic operation, cast or rounding
 * direction is changed. features/trackball/src/ztc_pointer.c's copy at the
 * same revision was verified byte-identical modulo naming (a `diff` with the
 * TP_/ZTC_ prefixes normalized showed only comment-text differences), so this
 * single reference stands in for both original copies.
 */
#define REF_TICK_MS 20u
#define REF_IDLE_MS 50u
#define REF_DT_MIN_MS 4u
#define REF_FP_SHIFT 16
#define REF_ONE (1 << REF_FP_SHIFT)
#define REF_VEL_MAX (64 * REF_ONE)
#define REF_EMA_SHIFT 2
#define REF_MIN_VEL (REF_ONE / 8)

struct ref_axis {
    int32_t vel;
    int32_t rem;
    int64_t last_ms;
};

static int16_t ref_scale_rem(int16_t value, uint8_t div, int16_t *rem) {
    if (div < 1) {
        div = 1;
    }
    int16_t num = (int16_t)(value + *rem);
    int16_t out = (int16_t)(num / (int16_t)div);
    *rem = (int16_t)(num - (int16_t)(out * (int16_t)div));
    return out;
}

static void ref_coast_track(struct ref_axis *ax, int32_t units_fp, int64_t now) {
    int64_t dt = now - ax->last_ms;
    const bool restart = (dt > (int64_t)REF_IDLE_MS) || (dt < 0);
    ax->last_ms = now;
    if (restart) {
        dt = REF_IDLE_MS;
    } else if (dt < (int64_t)REF_DT_MIN_MS) {
        dt = REF_DT_MIN_MS;
    }
    int64_t sample = ((int64_t)units_fp * (int64_t)REF_TICK_MS) / dt;
    sample = CLAMP(sample, -(int64_t)REF_VEL_MAX, (int64_t)REF_VEL_MAX);
    ax->vel = restart ? (int32_t)sample
                      : ax->vel + (int32_t)((sample - (int64_t)ax->vel) >> REF_EMA_SHIFT);
}

static int16_t ref_coast_step(struct ref_axis *ax, int32_t retain) {
    if (ax->vel == 0) {
        ax->rem = 0;
        return 0;
    }
    ax->vel = (int32_t)(((int64_t)ax->vel * retain) / 256);
    if (ax->vel > -REF_MIN_VEL && ax->vel < REF_MIN_VEL) {
        ax->vel = 0;
        ax->rem = 0;
        return 0;
    }
    int32_t acc = ax->vel + ax->rem;
    int32_t out = acc / REF_ONE;
    ax->rem = acc - out * REF_ONE;
    return (int16_t)CLAMP(out, INT16_MIN, INT16_MAX);
}

/* ---- tiny deterministic PRNG (xorshift32) --------------------------------
 * Not libc rand(): a fixed seed must produce the exact same sequence on every
 * host/compiler/libc, so the golden run below is reproducible byte-for-byte.
 */
static uint32_t prng_state;

static uint32_t prng_next(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

static int32_t prng_range(int32_t lo, int32_t hi) { /* inclusive */
    uint32_t span = (uint32_t)(hi - lo + 1);
    return lo + (int32_t)(prng_next() % span);
}

/* ---- part 1: direct, hand-checkable assertions ---------------------------- */

static void test_coast_direct(void) {
    /* scale_rem: mirrors ZMK's scaler (mul=1) with a persisted remainder. */
    int16_t rem = 0;
    T_EQ_INT(torabo_scale_rem(10, 3, &rem), 3, "scale_rem(10,3) 1st call -> 3 (10/3)");
    T_EQ_INT(rem, 1, "scale_rem(10,3) 1st call leaves rem=1");
    T_EQ_INT(torabo_scale_rem(10, 3, &rem), 3, "scale_rem(10,3) 2nd call -> 3 (11/3)");
    T_EQ_INT(rem, 2, "scale_rem(10,3) 2nd call leaves rem=2");
    T_EQ_INT(torabo_scale_rem(10, 3, &rem), 4, "scale_rem(10,3) 3rd call -> 4 (12/3)");
    T_EQ_INT(rem, 0, "scale_rem(10,3) 3rd call leaves rem=0");

    int16_t rem_neg = 0;
    T_EQ_INT(torabo_scale_rem(-10, 3, &rem_neg), -3, "scale_rem(-10,3) truncates toward 0");
    T_EQ_INT(rem_neg, -1, "scale_rem(-10,3) remainder keeps the sign of value");

    int16_t rem_div0 = 0;
    T_EQ_INT(torabo_scale_rem(7, 0, &rem_div0), 7, "scale_rem div<1 floors div to 1 (7/1=7)");

    /* threshold_fp: per_s ticks/second -> Q16 ticks/period. TICKS_PER_S=50 at
     * TICK_MS=20, so per_s==TICKS_PER_S maps to exactly ONE. */
    T_EQ_INT(torabo_coast_threshold_fp(50), TORABO_COAST_ONE, "threshold_fp(50) == ONE tick/period");
    T_EQ_INT(torabo_coast_threshold_fp(0), 0, "threshold_fp(0) == 0");
    T_EQ_INT(torabo_coast_threshold_fp(25), TORABO_COAST_ONE / 2, "threshold_fp(25) == half a tick/period");

    /* track(): last_ms starts at 0, so a first sample at now=1000 sees
     * dt=1000 > IDLE_MS(50) -> restart, dt forced to IDLE_MS.
     * sample = (ONE*TICK_MS)/IDLE_MS = (65536*20)/50 = 26214 (int64 trunc). */
    struct torabo_coast_axis ax = {0};
    torabo_coast_track(&ax, TORABO_COAST_ONE, 1000);
    T_EQ_INT(ax.vel, 26214, "track() seeds vel directly from a restart sample");
    T_EQ_INT(ax.last_ms, 1000, "track() always stamps last_ms, restart or not");

    /* step() decays vel by `retain`/256 each call and eventually floors to
     * exactly 0 (never oscillates), well inside the MAX_TICKS hard cap. */
    struct torabo_coast_axis dec = {.vel = TORABO_COAST_ONE * 10, .rem = 0, .last_ms = 0};
    uint32_t steps_to_zero = 0;
    while (dec.vel != 0 && steps_to_zero < TORABO_COAST_MAX_TICKS) {
        torabo_coast_step(&dec, 160 /* friction=32: fastest decay */);
        steps_to_zero++;
    }
    T_CHECK(dec.vel == 0, "step() with fast decay reaches vel=0 within MAX_TICKS");
    T_CHECK(steps_to_zero < TORABO_COAST_MAX_TICKS, "step() decay terminates well before the hard cap");
    T_EQ_INT(dec.rem, 0, "step() clears rem in the same call vel floors to 0");

    /* step() with vel already 0 is a pure no-op that also clears rem. */
    struct torabo_coast_axis idle = {.vel = 0, .rem = 12345, .last_ms = 0};
    int16_t out = torabo_coast_step(&idle, 253);
    T_EQ_INT(out, 0, "step() with vel==0 emits nothing");
    T_EQ_INT(idle.rem, 0, "step() with vel==0 clears a stale rem");
}

/* ---- part 2: golden parity vs. the pre-extraction reference --------------- */

enum drive_shape {
    SHAPE_STEADY = 0, /* constant velocity, fixed period */
    SHAPE_ACCEL,      /* ramps units_fp up, jittered period */
    SHAPE_DECEL,      /* ramps units_fp down toward 0 */
    SHAPE_SIGN_FLIP,  /* direction reversal (diagonal-flick style) */
    SHAPE_ZERO_MIXED, /* zeros and idle-length gaps mixed in */
    SHAPE_COUNT
};

static void test_coast_parity(void) {
    struct torabo_coast_axis new_ax = {0};
    struct ref_axis old_ax = {0};
    int64_t now = 0;
    int32_t units = 4096; /* start small: ~0.06 wheel ticks per event */
    int32_t mismatches_track = 0;
    int32_t mismatches_step = 0;

    prng_state = 0x9e3779b9u; /* fixed seed: identical sequence every run */

    const int32_t N = 20000;
    for (int32_t i = 0; i < N; i++) {
        enum drive_shape shape = (enum drive_shape)prng_range(0, SHAPE_COUNT - 1);
        int32_t dt_ms;
        switch (shape) {
        case SHAPE_STEADY:
            dt_ms = 20;
            break;
        case SHAPE_ACCEL:
            units += prng_range(0, 2048);
            dt_ms = prng_range(4, 40);
            break;
        case SHAPE_DECEL:
            units = (units > 512) ? units - prng_range(0, 512) : units;
            dt_ms = prng_range(4, 60);
            break;
        case SHAPE_SIGN_FLIP:
            units = -units;
            dt_ms = prng_range(4, 100);
            break;
        case SHAPE_ZERO_MIXED:
        default:
            units = (i % 5 == 0) ? 0 : units;
            /* Occasionally below DT_MIN (floor branch), occasionally above
             * IDLE_MS (restart branch): both rounding edges get exercised. */
            dt_ms = prng_range(1, 120);
            break;
        }
        /* Keep the sweep physically representative (bounded velocity range)
         * without ever relying on it for overflow safety: coast_track always
         * promotes to int64_t before multiplying, so this clamp changes
         * nothing about correctness, only realism of the input shape. */
        units = (int32_t)CLAMP(units, -(200 * TORABO_COAST_ONE), 200 * TORABO_COAST_ONE);
        now += dt_ms;

        torabo_coast_track(&new_ax, units, now);
        ref_coast_track(&old_ax, units, now);
        if (new_ax.vel != old_ax.vel || new_ax.rem != old_ax.rem || new_ax.last_ms != old_ax.last_ms) {
            mismatches_track++;
        }

        /* Interleave step() at a sweep of friction settings (1..32), as the
         * real work callback does every TICK_MS while gliding. */
        int32_t friction = 1 + (i % 32);
        int32_t retain = 256 - 3 * friction;
        int16_t new_out = torabo_coast_step(&new_ax, retain);
        int16_t old_out = ref_coast_step(&old_ax, retain);
        if (new_out != old_out || new_ax.vel != old_ax.vel || new_ax.rem != old_ax.rem) {
            mismatches_step++;
        }
    }

    T_EQ_INT(mismatches_track, 0, "coast_track: state matches the pre-extraction reference (20000 steps)");
    T_EQ_INT(mismatches_step, 0, "coast_step: state+output match the pre-extraction reference (20000 steps)");

    /* Also run scale_rem through the same PRNG stream, since it feeds the
     * exact values coast_track sees in production (tp_pointer.c / ztc_pointer.c
     * both scale before tracking). */
    int16_t new_rem = 0, old_rem = 0;
    int32_t mismatches_scale = 0;
    for (int32_t i = 0; i < N; i++) {
        int16_t value = (int16_t)prng_range(-1000, 1000);
        uint8_t div = (uint8_t)prng_range(0, 40); /* includes the div<1 floor case */
        int16_t new_v = torabo_scale_rem(value, div, &new_rem);
        int16_t old_v = ref_scale_rem(value, div, &old_rem);
        if (new_v != old_v || new_rem != old_rem) {
            mismatches_scale++;
        }
    }
    T_EQ_INT(mismatches_scale, 0, "scale_rem: matches the pre-extraction reference (20000 steps)");
}

void test_coast(void) {
    torabo_test_begin("coast engine (pure core, refactor phase 3)");
    test_coast_direct();
    test_coast_parity();
}
