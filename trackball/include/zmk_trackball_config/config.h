/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime trackball settings — v2 (fail-open, validated, double-buffered).
 * See docs/DESIGN_v2.md §4 (wire) / §11.D (publish) / §11.E (validate).
 *
 * Hard contract: the input thread reads ONE published snapshot per event,
 * locklessly; every accessor fails open to stock MOVE behavior. No value here
 * can make the movement path STOP or scale to zero.
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

struct ztc_snapshot {
    struct ztc_layer_cfg layers[ZTC_MAX_LAYERS];
    uint8_t temp_target;        /* validated < ZTC_MAX_LAYERS */
    uint16_t temp_timeout_ms;   /* clamped ZTC_TIMEOUT_MIN..MAX */
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

/* ---- write path (GATT/NVS): validate a wire blob then atomically publish --- */

/* Apply a complete wire blob (see §4/§11.E). Validates magic/version/length and
 * clamps every field into a stack shadow, then publishes via a single atomic
 * buffer swap. Returns 0 on success, negative on rejection (store unchanged). */
int ztc_apply_wire(const uint8_t *buf, uint16_t len);

/* Encode the current live snapshot into a wire blob (for GATT READ).
 * Writes up to cap bytes, sets *out_len. Returns 0 / negative on too-small cap. */
int ztc_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* Expected wire length for the current ZTC_MAX_LAYERS (header + N*layer). */
uint16_t ztc_wire_len(void);

/* Persist the current live snapshot to NVS (no-op without CONFIG_SETTINGS). */
int ztc_save(void);

#ifdef __cplusplus
}
#endif
