/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Dynamic (NVS-backed, live-editable) macros. See docs/DESIGN.md.
 *
 * A "slot" is an ordered list of steps; each step taps/presses/releases one ZMK
 * keycode (full usage incl. implicit mods). `&dmac <slot>` replays the slot.
 *
 * Transfer (mirrors trackball ztc, but asymmetric so writes never exceed MTU):
 *   READ  -> dm_encode_read_wire(): ALL slots at once (Read Blob handles size).
 *   WRITE -> dm_apply_write_wire(): ONE slot, small enough for a single ATT
 *            write. Validates version/slot/len; rejects => nothing changes.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DM_MAGIC 0x6d64u /* "dm" */
#define DM_VERSION 1
#define DM_SLOTS 20  /* K */
#define DM_STEPS 16  /* L (per slot) */

/* step actions */
#define DM_ACT_TAP 0
#define DM_ACT_PRESS 1
#define DM_ACT_RELEASE 2

/* ---- wire layout (little-endian, explicit byte offsets) ------------------ */
#define DM_WIRE_STEP 5 /* action u8 + keycode u32 */
/* READ: hdr(4) + K * (used_len u8 + L * step) */
#define DM_READ_HDR 4
#define DM_READ_SLOT (1 + DM_STEPS * DM_WIRE_STEP)
#define DM_READ_WIRE_LEN (DM_READ_HDR + DM_SLOTS * DM_READ_SLOT)
/* WRITE (one slot): version u8 + slot u8 + used_len u8 + used_len * step */
#define DM_WRITE_HDR 3
#define DM_WRITE_MAX (DM_WRITE_HDR + DM_STEPS * DM_WIRE_STEP)

struct dm_step {
    uint8_t action;   /* DM_ACT_* */
    uint32_t keycode; /* ZMK usage: page<<16 | id, implicit mods in bits 24-31 */
};

struct dm_slot {
    uint8_t len; /* number of valid steps, 0..DM_STEPS */
    struct dm_step steps[DM_STEPS];
};

/* Live (validated) slot, or NULL if idx out of range. Lock-free reader. */
const struct dm_slot *dm_live_slot(uint8_t idx);

/* One-slot WRITE wire -> validate, publish atomically, persist that slot. */
int dm_apply_write_wire(const uint8_t *buf, uint16_t len);

/* Full READ wire (all slots) into buf (>= DM_READ_WIRE_LEN). */
int dm_encode_read_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* Expected READ wire length. */
uint16_t dm_read_wire_len(void);
