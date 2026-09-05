/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime trackball settings — v3 (fail-open, validated, double-buffered).
 * See docs/DESIGN_v2.md §4 (wire) / §11.D (publish) / §11.E (validate).
 *
 * Hard contract: the input thread reads ONE published snapshot per event,
 * locklessly; every accessor fails open to stock MOVE behavior. No value here
 * can make the movement path STOP or scale to zero.
 *
 * v3 appends ONE inertial-scroll ("coast") parameter set for the whole ball —
 * enable / friction / start threshold — as a 4-byte trailer after the layer
 * array, so every existing offset is unchanged. It only ever affects the SCROLL
 * role; MOVE and OFF are byte-identical to v2. There is deliberately no per-layer
 * and no per-axis coast setting. v2 wires are still accepted on WRITE (they land
 * with coasting DISABLED, i.e. exactly the old behavior); READ always emits v3.
 */

#pragma once

#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <zmk/keymap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size the store to the keymap's layer count so it always matches the temp_layer
 * copy's MAX_LAYERS (ZMK_KEYMAP_LAYERS_LEN). Never a hand-picked constant. */
#define ZTC_MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

enum ztc_role {
    ZTC_ROLE_MOVE = 0,   /* cursor move (zero/unknown => this; fail-open) */
    ZTC_ROLE_SCROLL = 1, /* X=>HWHEEL, Y=>WHEEL */
    ZTC_ROLE_OFF = 2,    /* value forced to 0 (never STOP) */
};

#define ZTC_SPEED_MIN 1u
#define ZTC_SPEED_MAX 32u
#define ZTC_TIMEOUT_MIN 50u
#define ZTC_TIMEOUT_MAX 30000u

/* ------------------------------------------------------------------------
 * Inertial scroll ("coast"), v3 — one set for the ball, SCROLL role only.
 *
 * friction  1..32, same shape/range as speed_div so the app can reuse its
 *           slider. It is the per-tick velocity loss in 1/256ths: the coast
 *           velocity is multiplied by (256 - 3*friction)/256 every
 *           ZTC_COAST_TICK_MS. Small = long glide, large = stops almost at once
 *           (see ztc_pointer.c).
 * threshold 1..255, in OUTPUT wheel ticks per second — the scroll speed the ball
 *           must still have when the user lets go for a glide to start at all.
 *           Measured AFTER speed_div, so it means what the host will actually
 *           see, independent of how the axis is geared.
 * ------------------------------------------------------------------------ */
#define ZTC_COAST_FRICTION_MIN 1u
#define ZTC_COAST_FRICTION_MAX 32u
#define ZTC_COAST_FRICTION_DEFAULT 8u
#define ZTC_COAST_THRESHOLD_MIN 1u
#define ZTC_COAST_THRESHOLD_MAX 255u
#define ZTC_COAST_THRESHOLD_DEFAULT 24u

/* ---- live (in-RAM) snapshot: natural alignment, read locklessly ---------- */

struct ztc_axis_cfg {
    uint8_t role;       /* enum ztc_role; out-of-range treated as MOVE */
    uint8_t direction;  /* 0 normal, 1 reverse(invert) */
    uint8_t speed_div;  /* clamped ZTC_SPEED_MIN..MAX */
};

struct ztc_layer_cfg {
    struct ztc_axis_cfg x;
    struct ztc_axis_cfg y;
    bool temp_enable;   /* does ball-move on this layer trigger temp-layer */
};

/* Inertial scroll, one set for the whole ball (v3). Never per-layer/per-axis. */
struct ztc_coast_cfg {
    uint8_t enable;    /* 0 = off (v2 behavior), 1 = glide after the ball stops */
    uint8_t friction;  /* clamped ZTC_COAST_FRICTION_MIN..MAX */
    uint8_t threshold; /* clamped ZTC_COAST_THRESHOLD_MIN..MAX, wheel ticks/s */
};

struct ztc_snapshot {
    struct ztc_layer_cfg layers[ZTC_MAX_LAYERS];
    uint8_t temp_target;        /* validated < ZTC_MAX_LAYERS */
    uint16_t temp_timeout_ms;   /* clamped ZTC_TIMEOUT_MIN..MAX */
    struct ztc_coast_cfg coast; /* v3; disabled for a v2 wire */
};

/* Current published snapshot pointer. NEVER NULL; valid from C-init (defaults)
 * onward. Grab once per input event and use it throughout. */
const struct ztc_snapshot *ztc_live(void);

/* Fail-open accessors (out-of-range layer => stock-safe default). */
static inline struct ztc_axis_cfg ztc_axis_for(const struct ztc_snapshot *s, uint8_t layer,
                                               bool is_x) {
    static const struct ztc_axis_cfg move_default = {
        .role = ZTC_ROLE_MOVE, .direction = 0, .speed_div = 1};
    if (!s || layer >= ZTC_MAX_LAYERS) {
        return move_default;
    }
    return is_x ? s->layers[layer].x : s->layers[layer].y;
}

static inline bool ztc_temp_enabled(const struct ztc_snapshot *s, uint8_t layer) {
    /* fail-open: unknown layer => behave like stock (enabled where stock had it).
     * Out-of-range simply returns false so we never index past the array; the
     * temp_layer copy additionally bounds-checks. */
    return (s && layer < ZTC_MAX_LAYERS) ? s->layers[layer].temp_enable : false;
}

/* Fail-open coast lookup (empty store => disabled, i.e. pre-v3 behavior). */
static inline struct ztc_coast_cfg ztc_coast_for(const struct ztc_snapshot *s) {
    static const struct ztc_coast_cfg off = {
        .enable = 0,
        .friction = (uint8_t)ZTC_COAST_FRICTION_DEFAULT,
        .threshold = (uint8_t)ZTC_COAST_THRESHOLD_DEFAULT,
    };
    return s ? s->coast : off;
}

/* ---- write path (GATT/NVS): validate a wire blob then atomically publish --- */

/* Apply a complete wire blob (see §4/§11.E). Accepts version 2 (no coast block)
 * and version 3. Validates magic/version/length and clamps every field into a
 * stack shadow, then publishes via a single atomic buffer swap. Returns 0 on
 * success, negative on rejection (store unchanged). */
int ztc_apply_wire(const uint8_t *buf, uint16_t len);

/* Encode the current live snapshot into a wire blob (for GATT READ). Always emits
 * version 3 (coast trailer present).
 * Writes up to cap bytes, sets *out_len. Returns 0 / negative on too-small cap. */
int ztc_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* Expected v3 wire length for the current ZTC_MAX_LAYERS (hdr + N*layer + coast). */
uint16_t ztc_wire_len(void);

/* Total wire length a blob starting with this header claims (v2 or v3), or 0 if
 * the header is not a plausible start of one (bad magic / unknown version). The
 * single place that knows this arithmetic: ztc_apply_wire() uses it for its
 * exact-length check, and the GATT write assembler (torabo_common/wire_asm.h)
 * uses it for chunk framing, so the two can never disagree about where a wire
 * ends. Independent of the declared layer_count — the trackball wire always
 * carries ZTC_MAX_LAYERS slots; apply still validates that byte.
 * @param hdr at least ZTC_WIRE_HDR readable bytes. */
uint16_t ztc_expected_len(const uint8_t *hdr);

/* ---- compile-time wire layout ------------------------------------------- */
#define ZTC_WIRE_HDR 8u    /* magic[2] version layer_count temp_target _rsv timeout[2] */
#define ZTC_WIRE_LAYER 12u /* x{role dir speed _rsv} y{...} temp_enable _rsv[3] */
#define ZTC_WIRE_COAST 4u  /* v3 trailer: enable friction threshold _rsv */

/* Upper bound on the wire size (for GATT/NVS buffers): the v3 length. */
#define ZTC_WIRE_CAP (ZTC_WIRE_HDR + (uint32_t)ZTC_MAX_LAYERS * ZTC_WIRE_LAYER + ZTC_WIRE_COAST)

/* Persist the current live snapshot to NVS (no-op without CONFIG_SETTINGS). */
int ztc_save(void);

#ifdef __cplusplus
}
#endif
