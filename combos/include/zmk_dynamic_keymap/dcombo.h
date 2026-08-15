/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Dynamic (NVS-backed, live-editable) combos. See docs/DESIGN-combos.md.
 *
 * A "combo" fires one behavior when a set of key positions is pressed together
 * within a timeout (verbatim ZMK combo semantics; the engine in combo_engine.c
 * is a near-literal copy of zmk/app/src/combo.c with the definition array moved
 * from a DT-const to this NVS-backed RAM store).
 *
 * Transfer (mirrors the dynamic-macro wire, asymmetric so writes never exceed
 * a single ATT write):
 *   READ  -> cb_encode_read_wire(): ALL combo slots at once (Read Blob handles
 *            size). FW -> app.
 *   WRITE -> cb_apply_write_wire(): ONE combo slot, small enough for a single
 *            ATT write. Validates everything; on reject nothing changes.
 *
 * Fail-safe contract: an unloaded / invalid / disabled slot is treated as "no
 * combo" — it never captures a key, so normal typing is never affected.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include <zmk/behavior.h> /* struct zmk_behavior_binding (engine-ready combo) */

#define CB_MAGIC 0x6263u /* "cb" */
#define CB_VERSION 1
#define CB_SLOTS 16   /* M: number of combo slots */
#define CB_MAX_POS 6  /* P: max key positions per combo */

/* target_type: which behavior the combo fires. Resolved in combo_state.c to a
 * concrete behavior device name (fail-open: unknown/absent -> slot disabled). */
#define CB_TGT_KP 0   /* key press     param1 = ZMK keycode (page<<16|id, mods 24..31) */
#define CB_TGT_MO 1   /* momentary lyr param1 = layer */
#define CB_TGT_TO 2   /* to layer      param1 = layer */
#define CB_TGT_TOG 3  /* toggle layer  param1 = layer */
#define CB_TGT_DMAC 4 /* dynamic macro param1 = macro slot (needs torabo-macros) */
#define CB_TGT_MAX CB_TGT_DMAC

/* flags bitmask */
#define CB_FLAG_SLOW_RELEASE 0x01

/* ---- wire layout (little-endian, explicit byte offsets) ------------------ */
/* One slot (26 bytes). Offsets must match the app codec (comboConfig.ts). */
#define CB_W_ENABLED 0     /* u8  */
#define CB_W_POS_COUNT 1   /* u8  0..P */
#define CB_W_POSITIONS 2   /* u8[P] */
#define CB_W_LAYER_MASK 8  /* u32 0 = all layers */
#define CB_W_TIMEOUT 12    /* u16 ms */
#define CB_W_PRIOR_IDLE 14 /* u16 ms */
#define CB_W_FLAGS 16      /* u8  */
#define CB_W_TGT_TYPE 17   /* u8  CB_TGT_* */
#define CB_W_TGT_P1 18     /* u32 */
#define CB_W_TGT_P2 22     /* u32 */
#define CB_WIRE_SLOT 26    /* total per-slot wire bytes */

/* READ: hdr(4) + M slots */
#define CB_READ_HDR 4
#define CB_READ_WIRE_LEN (CB_READ_HDR + CB_SLOTS * CB_WIRE_SLOT)
/* WRITE (one slot): version u8 + slot u8 + one slot */
#define CB_WRITE_HDR 2
#define CB_WRITE_MAX (CB_WRITE_HDR + CB_WIRE_SLOT)

/* ---- engine-ready combo definition (mirrors zmk combo_cfg + enabled) ------ */
struct cb_combo {
    int32_t key_positions[CB_MAX_POS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    bool slow_release;
    bool enabled; /* false => never enters the lookup, never fires */
};

/* ---- store API (combo_state.c) ------------------------------------------- */

/* One-slot WRITE wire -> validate, build cfg, stage pending, persist that slot.
 * Returns 0 on success; on any validation failure nothing changes. */
int cb_apply_write_wire(const uint8_t *buf, uint16_t len);

/* Full READ wire (all slots) into buf (>= CB_READ_WIRE_LEN). */
int cb_encode_read_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* Expected READ wire length. */
uint16_t cb_read_wire_len(void);

/* Engine pull (called at idle, in listener context): if the staged config has
 * changed since *seq, copy all CB_SLOTS combos into out[], update *seq, and
 * return true. Otherwise return false and leave out[] untouched. */
bool cb_fetch_pending(struct cb_combo out[CB_SLOTS], uint32_t *seq);
