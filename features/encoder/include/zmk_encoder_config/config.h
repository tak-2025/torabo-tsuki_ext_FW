/*
 * Runtime-configurable rotary encoder (torabo-tsuki).
 *
 * The encoder is NOT a keymap key. Rotation rides ZMK's sensor path and the push
 * button rides the input path, and both look up what to fire in THIS store, which
 * the app edits live over BLE. That is why adding an encoder needs no matrix
 * transform, no physical-layout entry and no keymap bindings.
 *
 * Deliberately standalone: it duplicates a few small types from the trackpad
 * module rather than sharing them, so a change here can never disturb the
 * trackpad wire that is already in the field.
 *
 * Per layer we hold three assignments:
 *   cw  — one detent clockwise
 *   ccw — one detent counter-clockwise
 *   btn — the push button (press+release)
 */

#pragma once

#include <zephyr/types.h>
#include <zmk/keymap.h> /* ZMK_KEYMAP_LAYERS_LEN */

/* Layer count comes from the keymap, never a hand-picked constant. */
#define ENC_MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN

/* ---- binding descriptor (mirrors the trackpad's, kept separate on purpose) --
 * Which ZMK behavior to fire, plus its parameter. The firmware synthesises the
 * real zmk_behavior_binding at runtime (see binding.h), so any keycode is
 * assignable without a compile-time palette. */
enum enc_behavior {
    ENC_BEH_NONE = 0, /* unassigned => fire nothing (fail-open) */
    ENC_BEH_KP = 1,   /* &kp, keyboard page */
    ENC_BEH_CP = 2,   /* &kp with the consumer page (media keys) */
    ENC_BEH_MO = 3,   /* &mo <layer> */
    ENC_BEH_TO = 4,   /* &to <layer> */
    ENC_BEH_TOG = 5,  /* &tog <layer> */
};
#define ENC_BEH_MAX ENC_BEH_TOG

/* Modifier bits for KP/CP (match ZMK MOD_L* order; see binding.h). */
#define ENC_MOD_LCTL 0x01
#define ENC_MOD_LSFT 0x02
#define ENC_MOD_LALT 0x04
#define ENC_MOD_LGUI 0x08

struct enc_binding {
    uint8_t behavior; /* enum enc_behavior */
    uint8_t mods;     /* ENC_MOD_* bitmask, KP/CP only */
    uint16_t param;   /* keycode / consumer usage / layer index */
};

struct enc_layer_cfg {
    struct enc_binding cw;
    struct enc_binding ccw;
    struct enc_binding btn;
};

struct enc_snapshot {
    uint8_t layer_count; /* <= ENC_MAX_LAYERS */
    struct enc_layer_cfg layers[ENC_MAX_LAYERS];
};

/* ---- wire (LE, fixed-length, versioned) ----------------------------------
 * header: magic u16 | version u8 | layer_count u8
 * then layer_count * { cw(4) ccw(4) btn(4) }
 * binding: behavior u8 | mods u8 | param u16
 *
 * 4 + 10 layers * 12 = 124 B at the field layer count, but the wire GROWS with
 * ZMK_KEYMAP_LAYERS_LEN: at 20 layers it is exactly 244 B, which is the largest
 * payload a single ATT write can carry on a 247-byte MTU, and at 21 it no longer
 * fits. The GATT characteristic therefore reassembles chunked writes
 * (torabo_common/wire_asm.h) instead of rejecting offset != 0. */
#define ENC_WIRE_MAGIC 0x6E65 /* "en" little-endian */
#define ENC_WIRE_VERSION 1
#define ENC_WIRE_HDR 4
#define ENC_WIRE_BIND 4
#define ENC_WIRE_LAYER (ENC_WIRE_BIND * 3)
#define ENC_WIRE_CAP (ENC_WIRE_HDR + ENC_MAX_LAYERS * ENC_WIRE_LAYER)

/* ---- API ------------------------------------------------------------------ */

/* Currently published snapshot. NEVER NULL (defaults from C-init). Grab once per
 * event and use that pointer for the whole event. */
const struct enc_snapshot *enc_live(void);

int enc_apply_wire(const uint8_t *buf, uint16_t len);              /* wire -> validate -> publish */
int enc_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len); /* live -> wire (GATT READ) */
uint16_t enc_wire_len_for(uint8_t layer_count);

/* Total wire length a blob starting with this header claims, or 0 if the header
 * is not a plausible start of one (bad magic / unknown version / a layer_count
 * of 0 or above this build's maximum). The single place that knows this
 * arithmetic: enc_apply_wire() uses it for its length check, and the GATT write
 * assembler (torabo_common/wire_asm.h) uses it for chunk framing, so the two can
 * never disagree about where a wire ends.
 * @param hdr at least ENC_WIRE_HDR readable bytes. */
uint16_t enc_expected_len(const uint8_t *hdr);
int enc_save(void); /* persist the live snapshot to NVS */

/* ---- fail-open accessors (header-only, no locking) ------------------------ */

/* Unknown layer => an unassigned binding, i.e. nothing fires. Never traps. */
static inline struct enc_binding enc_binding_for(const struct enc_snapshot *s, uint8_t layer,
                                                 uint8_t which /* 0=cw 1=ccw 2=btn */) {
    const struct enc_binding none = {0};
    if (!s || layer >= s->layer_count || layer >= ENC_MAX_LAYERS) {
        return none;
    }
    const struct enc_layer_cfg *l = &s->layers[layer];
    switch (which) {
    case 0:
        return l->cw;
    case 1:
        return l->ccw;
    case 2:
        return l->btn;
    default:
        return none;
    }
}

#define ENC_CW 0
#define ENC_CCW 1
#define ENC_BTN 2

static inline bool enc_binding_active(const struct enc_binding *b) {
    return b && b->behavior != ENC_BEH_NONE && b->behavior <= ENC_BEH_MAX;
}
