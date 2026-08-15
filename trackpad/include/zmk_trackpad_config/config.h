/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime trackpad settings — per-layer, per-device pointing-function assignment
 * for the mini trackpad(s). Fail-open, validated, double-buffered — same safety
 * rules as the trackball store (see docs/DESIGN-trackpad.md §2/§3/§4 and
 * docs/DESIGN-trackpad-v2.md §2/§3/§4):
 *   - the input path reads ONE published snapshot per event, locklessly;
 *   - every accessor fails open to stock MOVE / NONE behavior;
 *   - no value here can make the movement path STOP or scale to zero.
 *
 * v2 (docs/DESIGN-trackpad-v2.md) generalises v1's fixed discrete roles:
 *   - the axis discrete role collapses to a single ENCODER role that carries a
 *     pos/neg binding pair (swipe up/down each fire an arbitrary behavior+param;
 *     the old Volume/Brightness/Zoom/Browser roles become app-side presets);
 *   - a per-layer gesture slot { tap, tap2, hold }, each an arbitrary binding,
 *     consumed by the tp_keys processor.
 * A binding is { behavior, mods, param } — behavior selects which ZMK behavior
 * the firmware fires (none/kp/cp/mo/to/tog); the firmware builds the binding at
 * runtime (see include/zmk_trackpad_config/binding.h). v1 wire is still accepted
 * on WRITE (upgraded to this model); READ always emits v2.
 */

#pragma once

#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <zmk/keymap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Size the store to the keymap's layer count so it always matches what the app
 * sends (which mirrors what we send on READ). Never a hand-picked constant. */
#define TP_MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

/* Up to 4 pads (left/right each optionally ball+pad); see DESIGN-trackpad §2. */
#define TP_MAX_DEVICES 4

/* Devices exposed by default (in wire order): left pad (id 0) + right ext pad
 * (id 1). The app shows one dropdown entry per exposed device. */
#define TP_DEFAULT_DEVICE_COUNT 2

/* Stable device ids (DESIGN-trackpad §2; must match tpConfig.ts TpDeviceId).
 *
 * NOTE: these names describe only the DEFAULT build (central=right). The id is
 * really just "wire slot": id 0 = the pointing device exported by the peripheral
 * over split, id 1 = the central's extender pad. Which physical half/connector
 * that is depends on the build, so DO NOT infer the label from the id — use the
 * per-device meta byte below, which the app renders. */
enum tp_device_id {
    TP_DEVICE_LEFT_PAD = 0,
    TP_DEVICE_RIGHT_EXT_PAD = 1,
};

/* ------------------------------------------------------------------------
 * Device identity metadata (wire: 2nd byte of the per-device header, formerly
 * _rsv). FW-authoritative and READ-ONLY for the app: the app renders a label
 * from it but never writes it back (deserialize re-derives it from Kconfig).
 *
 * Value comes from CONFIG_ZMK_TRACKPAD_CONFIG_DEV<n>_META, which the firmware
 * builder emits per hardware pattern. 0 => unknown; the app then falls back to
 * a generic "device N" label, so old firmware keeps working unchanged.
 * ------------------------------------------------------------------------ */
#define TP_META_SIDE_SHIFT 0
#define TP_META_SIDE_MASK 0x03
#define TP_META_CONN_SHIFT 2
#define TP_META_CONN_MASK 0x0C
#define TP_META_KIND_SHIFT 4
#define TP_META_KIND_MASK 0x30

enum tp_meta_side { TP_META_SIDE_UNKNOWN = 0, TP_META_SIDE_LEFT = 1, TP_META_SIDE_RIGHT = 2 };
/* Which FFC the device hangs off: the board's own connector, or the extender. */
enum tp_meta_conn { TP_META_CONN_UNKNOWN = 0, TP_META_CONN_STD = 1, TP_META_CONN_EXT = 2 };
enum tp_meta_kind {
    TP_META_KIND_UNKNOWN = 0,
    TP_META_KIND_PAD = 1,
    TP_META_KIND_BALL = 2,
    TP_META_KIND_ENCODER = 3,
};

#define TP_META(side, conn, kind)                                                                  \
    (uint8_t)((((side) << TP_META_SIDE_SHIFT) & TP_META_SIDE_MASK) |                               \
              (((conn) << TP_META_CONN_SHIFT) & TP_META_CONN_MASK) |                               \
              (((kind) << TP_META_KIND_SHIFT) & TP_META_KIND_MASK))

/* Axis role (v2) — must match tpConfigV2.ts TpRole. Unknown => MOVE (fail-open).
 * The v1 discrete roles (Volume..Browser, 3..6) are collapsed into ENCODER, which
 * carries a pos/neg binding pair; the app fills the pair from its presets. */
enum tp_role {
    TP_ROLE_MOVE = 0,    /* cursor move */
    TP_ROLE_SCROLL = 1,  /* X=>HWHEEL, Y=>WHEEL */
    TP_ROLE_OFF = 2,     /* value forced to 0 (never STOP) */
    TP_ROLE_ENCODER = 3, /* accumulate; fire pos/neg binding every `step` units */
};
#define TP_ROLE_MAX TP_ROLE_ENCODER

/* Which ZMK behavior a binding fires — must match tpConfigV2.ts TpBehavior and
 * the reference order in binding.h. Unknown => NONE (fail-open). */
enum tp_behavior {
    TP_BEH_NONE = 0, /* &none */
    TP_BEH_KP = 1,   /* &kp   param = HID keyboard usage, mods in .mods (§4.3) */
    TP_BEH_CP = 2,   /* &kp   param = HID consumer usage (consumer page, §4.3) */
    TP_BEH_MO = 3,   /* &mo   param = layer */
    TP_BEH_TO = 4,   /* &to   param = layer */
    TP_BEH_TOG = 5,  /* &tog  param = layer */
};
#define TP_BEH_MAX TP_BEH_TOG

#define TP_STEP_MIN 1u
#define TP_STEP_MAX 32u

/* ---- live (in-RAM) snapshot: natural alignment, read locklessly ---------- */

/* One fire target. Wire form is 4B (behavior u8, mods u8, param u16 LE); in RAM
 * it is naturally aligned. Out-of-range behavior is coerced to NONE on decode. */
struct tp_binding {
    uint8_t behavior; /* enum tp_behavior; out-of-range treated as NONE */
    uint8_t mods;     /* &kp modifier bits (MOD_L* order); 0 for others */
    uint16_t param;   /* keycode / consumer usage / layer */
};

struct tp_axis_cfg {
    uint8_t role;            /* enum tp_role; out-of-range treated as MOVE */
    uint8_t direction;       /* 0 normal, 1 reverse(invert / swap pos<->neg) */
    uint8_t step;            /* clamped TP_STEP_MIN..MAX (divisor / threshold) */
    struct tp_binding pos;   /* ENCODER: fired on +direction swipe */
    struct tp_binding neg;   /* ENCODER: fired on -direction swipe */
};

struct tp_gestures {
    struct tp_binding tap;  /* GST_TAP  — single tap   (driver BTN_0) */
    struct tp_binding tap2; /* GST_TAP2 — two-finger tap(driver BTN_1) */
    struct tp_binding hold; /* GST_HOLD — press&hold, down/up follow (driver BTN_2) */
    struct tp_binding dtap; /* GST_DTAP — double tap (§4.4), detected in tp_keys via
                             * a timing window on BTN_0. Carried in the wire at
                             * gesture offset +12; tp_keys defers the single tap only
                             * when this is set. */
};

struct tp_layer_cfg {
    struct tp_axis_cfg x;
    struct tp_axis_cfg y;
    struct tp_gestures gestures;
};

struct tp_device_cfg {
    uint8_t device_id; /* enum tp_device_id (wire slot) */
    uint8_t meta;      /* TP_META(side, conn, kind); 0 = unknown. FW-authoritative. */
    struct tp_layer_cfg layers[TP_MAX_LAYERS];
};

struct tp_snapshot {
    uint8_t device_count; /* <= TP_MAX_DEVICES */
    bool has_gestures;    /* whether the gesture section is meaningful (always true
                           * for our own defaults / encode; false only for a v1
                           * blob that never carried gestures) */
    struct tp_device_cfg devices[TP_MAX_DEVICES];
};

/* Current published snapshot pointer. NEVER NULL; valid from C-init (defaults)
 * onward. Grab once per input event and use it throughout. */
const struct tp_snapshot *tp_live(void);

/* Fail-open axis lookup by device id + layer (unknown => stock-safe MOVE). */
static inline struct tp_axis_cfg tp_axis_for(const struct tp_snapshot *s, uint8_t device_id,
                                             uint8_t layer, bool is_x) {
    static const struct tp_axis_cfg move_default = {
        .role = TP_ROLE_MOVE, .direction = 0, .step = 1};
    if (!s || layer >= TP_MAX_LAYERS) {
        return move_default;
    }
    for (uint8_t d = 0; d < s->device_count && d < TP_MAX_DEVICES; d++) {
        if (s->devices[d].device_id == device_id) {
            return is_x ? s->devices[d].layers[layer].x : s->devices[d].layers[layer].y;
        }
    }
    return move_default;
}

/* Fail-open gesture lookup by device id + layer (unknown => all-NONE, i.e. let
 * the driver's default click pass through). Returned by value: the caller keeps
 * its own copy for the whole event, consistent with the lockless snapshot read. */
static inline struct tp_gestures tp_gestures_for(const struct tp_snapshot *s, uint8_t device_id,
                                                 uint8_t layer) {
    static const struct tp_gestures none_gestures = {0}; /* all TP_BEH_NONE */
    if (!s || !s->has_gestures || layer >= TP_MAX_LAYERS) {
        return none_gestures;
    }
    for (uint8_t d = 0; d < s->device_count && d < TP_MAX_DEVICES; d++) {
        if (s->devices[d].device_id == device_id) {
            return s->devices[d].layers[layer].gestures;
        }
    }
    return none_gestures;
}

/* True if a binding actually fires something (i.e. is not NONE / out of range). */
static inline bool tp_binding_active(const struct tp_binding *b) {
    return b && b->behavior != TP_BEH_NONE && b->behavior <= TP_BEH_MAX;
}

/* ---- write path (GATT/NVS): validate a wire blob then atomically publish --- */

/* Apply a complete wire blob (DESIGN-trackpad-v2.md §3). Accepts BOTH version 1
 * (upgraded: discrete roles 3..6 -> ENCODER + preset pos/neg) and version 2.
 * Validates magic/version/length and clamps every field into a stack shadow,
 * then publishes via a single atomic buffer swap. Returns 0 on success, negative
 * on rejection (store unchanged). */
int tp_apply_wire(const uint8_t *buf, uint16_t len);

/* Encode the current live snapshot into a wire blob (for GATT READ). Always emits
 * version 2 with the gesture section present. Writes up to cap bytes, sets
 * *out_len. Returns 0 / negative on too-small cap. */
int tp_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* v2 wire length (gesture section present) for a given device/layer count. */
uint16_t tp_wire_len_for(uint8_t device_count, uint8_t layer_count);

/* ---- compile-time wire layout (DESIGN-trackpad-v2.md §3) ----------------- */
#define TP_WIRE_HDR 6u     /* magic[2] version device_count layer_count flags */
#define TP_WIRE_DEV_HDR 2u /* device_id _rsv */
#define TP_WIRE_BIND 4u    /* behavior mods param(2 LE) */
#define TP_WIRE_AXIS 11u   /* role dir step + pos(4) + neg(4) */
#define TP_WIRE_GEST 16u   /* tap(4) tap2(4) hold(4) dtap(4) */
/* v2 per-layer stride WITH the gesture section (what we always encode). */
#define TP_WIRE_LAYER_V2 (TP_WIRE_AXIS * 2u + TP_WIRE_GEST) /* 38 */
/* v1 layout (accepted on WRITE only): axis is 3B, layer is 6B. */
#define TP_WIRE_AXIS_V1 3u
#define TP_WIRE_LAYER_V1 6u

/* header flags */
#define TP_FLAG_GESTURES 0x01u

/* Compile-time upper bound on the wire size (for static reassembly buffers). */
#define TP_WIRE_CAP                                                                                 \
    (TP_WIRE_HDR +                                                                                  \
     (uint32_t)TP_MAX_DEVICES * (TP_WIRE_DEV_HDR + (uint32_t)TP_MAX_LAYERS * TP_WIRE_LAYER_V2))

/* Persist the current live snapshot to NVS (no-op without CONFIG_SETTINGS). */
int tp_save(void);

#ifdef __cplusplus
}
#endif
