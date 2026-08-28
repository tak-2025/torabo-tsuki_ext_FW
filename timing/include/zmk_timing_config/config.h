/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Runtime timing settings — hold-tap parameters for the two stock hold-tap
 * nodes (&mt / &lt) plus the matrix kscan debounce windows, editable live from
 * the app. docs/DESIGN-timing.md is the single source of truth for the wire.
 *
 * SAFETY MODEL (same rules as the trackpad store):
 *   - the hold-tap decision path takes ONE published snapshot per key press and
 *     keeps it for the whole undecided window (latched at press time), so a
 *     setting written mid-decision can never change the rules half way through;
 *   - the kscan path reads the effective debounce config on every scan, so a
 *     write applies from the next scan with no re-init;
 *   - until a valid wire has been applied, BOTH override hooks decline and the
 *     firmware runs on its devicetree values, bit-for-bit as before.
 *
 * The parameter struct is `struct zmk_torabo_ht_params` from the zmk fork's
 * <zmk/torabo_timing.h> rather than a private copy: it IS the hand-off type, and
 * a second definition would be one more thing to keep in sync.
 */

#pragma once

#include <stdbool.h>
#include <zephyr/types.h>

#include <zmk/torabo_timing.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- wire v1 (DESIGN-timing.md §"Wire v1") — 96 B fixed, little-endian ----
 *
 *   header (8B): version=1 | ht_node_count=2 | ht_pos_slots=32 |
 *                debounce_press_ms | debounce_release_ms | rsv[3]
 *   ht block x2 (44B each), block 0 = mt (mod_tap), block 1 = lt (layer_tap):
 *     +0 u16 tapping_term_ms | +2 u16 quick_tap_ms | +4 u16 require_prior_idle_ms
 *     +6 u8 flavor | +7 u8 flags | +8 u8 pos_count | +9 rsv
 *     +10 u8[32] positions | +42 u16 rsv
 *
 * The length is FIXED: a wire that is not exactly TMG_WIRE_LEN bytes, or whose
 * header does not describe this exact shape, is rejected rather than
 * interpreted. Future growth bumps the version and spends the reserved bytes.
 */
#define TMG_WIRE_VERSION 1u
#define TMG_WIRE_HDR 8u
#define TMG_HT_NODES 2u
#define TMG_HT_POS_SLOTS 32u
#define TMG_HT_BLOCK 44u
#define TMG_WIRE_LEN (TMG_WIRE_HDR + TMG_HT_NODES * TMG_HT_BLOCK) /* 96 */
#define TMG_WIRE_CAP TMG_WIRE_LEN

/* ht block offsets, so encode/decode/reassembly all quote the same numbers. */
#define TMG_HT_OFF_TAPPING_TERM 0u
#define TMG_HT_OFF_QUICK_TAP 2u
#define TMG_HT_OFF_PRIOR_IDLE 4u
#define TMG_HT_OFF_FLAVOR 6u
#define TMG_HT_OFF_FLAGS 7u
#define TMG_HT_OFF_POS_COUNT 8u
#define TMG_HT_OFF_POSITIONS 10u

/* Wire block index per hold-tap node. Fixed by the wire, NOT discovered. */
#define TMG_NODE_MT 0u /* devicetree node "mod_tap" (&mt) */
#define TMG_NODE_LT 1u /* devicetree node "layer_tap" (&lt) */

/* Devicetree node names the two blocks bind to. */
#define TMG_NODE_NAME_MT "mod_tap"
#define TMG_NODE_NAME_LT "layer_tap"

/* Clamps. A value outside these is pulled in, not rejected: the wire as a whole
 * is either well-formed (and every field is made safe) or refused outright. */
#define TMG_TAPPING_TERM_MIN 10u
#define TMG_TAPPING_TERM_MAX 2000u
#define TMG_DEBOUNCE_MIN 1u
#define TMG_DEBOUNCE_MAX 100u

/* u16 sentinel for "feature disabled" (quick-tap / require-prior-idle), decoded
 * to the -1 that struct behavior_hold_tap_config expects. */
#define TMG_U16_DISABLED 0xFFFFu

/* All flavors the wire may name; anything else falls back to the node default. */
#define TMG_FLAVOR_MAX 3u

/* Every flags bit wire v1 defines (ZMK_TORABO_HT_FLAG_* in <zmk/torabo_timing.h>). */
#define TMG_FLAGS_MASK                                                                             \
    (ZMK_TORABO_HT_FLAG_RETRO_TAP | ZMK_TORABO_HT_FLAG_HOLD_TRIGGER_ON_RELEASE |                   \
     ZMK_TORABO_HT_FLAG_HOLD_WHILE_UNDECIDED)

/* ---- live (in-RAM) snapshot --------------------------------------------- */

struct tmg_snapshot {
    struct zmk_torabo_ht_params ht[TMG_HT_NODES];
    uint8_t debounce_press_ms;
    uint8_t debounce_release_ms;
};

/**
 * Current published snapshot. NEVER NULL; valid from C-init onwards. Grab it
 * once and use that one pointer for the whole operation.
 */
const struct tmg_snapshot *tmg_live(void);

/**
 * Apply a complete wire blob: validates version/shape/length, clamps every
 * field into a shadow, then publishes with a single atomic swap. Returns 0, or
 * a negative errno with the store completely unchanged.
 */
int tmg_apply_wire(const uint8_t *buf, uint16_t len);

/**
 * Encode the live values (devicetree defaults until something has been written)
 * into a wire blob for READ. Returns 0, or -ENOMEM if cap < TMG_WIRE_LEN.
 */
int tmg_encode_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/**
 * Total wire length a blob starting with this header claims, or 0 if the header
 * is not a plausible start of one. Used by the GATT write assembler for framing;
 * tmg_apply_wire still re-validates whatever it assembles.
 * @param hdr at least TMG_WIRE_HDR readable bytes.
 */
uint16_t tmg_expected_len(const uint8_t *hdr);

/** Persist the live values to NVS under "tmg/wire" (no-op without CONFIG_SETTINGS). */
int tmg_save(void);

#ifdef __cplusplus
}
#endif
