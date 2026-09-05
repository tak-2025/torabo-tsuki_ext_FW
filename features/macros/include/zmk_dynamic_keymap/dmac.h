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
 *   WRITE -> dm_apply_write_wire(): ONE slot's STEPS (v1, permanently — see
 *            below), or one slot's NAME (v2). Validates version/slot/len;
 *            rejects => nothing changes.
 *
 * v2 (PLAN-ext-fw-refactor.md フェーズ8, wire spec confirmed against the app's
 * reference codec torabo-studio/src/dynamic_macros/dmacConfig.ts): the READ
 * wire gets a per-slot NAME block appended after the (byte-identical) v1 slot
 * region, and WRITE gets a second, name-only op. The steps WRITE op keeps
 * saying version 1 forever, so an app that has never heard of names (an old
 * build, an old backup file) can still edit steps without erasing names it
 * cannot see — names only ever change via the dedicated name op.
 *
 *   READ (v2, 1964 B total):
 *     0            magic u16 = 0x6d64
 *     2            version u8 = 2            (DM_VERSION, was 1 pre-phase8)
 *     3            slot_count u8 = 20
 *     4            slot_count * 81 B of slots (identical to v1)
 *     4+20*81=1624 slot_count * 17 B of names: name_len u8 + name[16]
 *
 *   WRITE (name op, v2 only, ALWAYS 20 B, never mixed with steps):
 *     0  version u8 = 2 (DM_VERSION_V2)
 *     1  slot u8
 *     2  kind u8   (DM_WRITE_KIND_STEPS=0 is defined but REJECTED — the app
 *                   never sends it, steps stay on the v1 op; DM_WRITE_KIND_NAME=1)
 *     3  name_len u8 (0..DM_NAME_MAX)
 *     4  name[DM_NAME_MAX] UTF-8, zero-padded past name_len (opaque to the FW)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DM_MAGIC 0x6d64u /* "dm" */
#define DM_VERSION_V1 1  /* steps WRITE op; the only version that op ever sends */
#define DM_VERSION_V2 2  /* READ wire (adds the name block); name-only WRITE op */
/* Current/newest wire version this firmware speaks (what dm_encode_read_wire
 * stamps into the READ header). Kept as its own symbol, like the app's
 * DM_VERSION, so a future bump is a one-line, grep-able change. */
#define DM_VERSION DM_VERSION_V2
#define DM_SLOTS 20  /* K */
#define DM_STEPS 16  /* L (per slot) */
/* Fixed width of a slot's name field, in BYTES (not characters). UTF-8
 * validity and the character-boundary cut are the APP's responsibility
 * (PLAN フェーズ8 検証); the firmware stores DM_NAME_MAX opaque bytes. */
#define DM_NAME_MAX 16

/* step actions */
#define DM_ACT_TAP 0
#define DM_ACT_PRESS 1
#define DM_ACT_RELEASE 2

/* v2 name-op `kind` byte (offset 2). Steps is reserved by the wire spec but
 * never emitted by the app and is REJECTED here on purpose (PLAN フェーズ8,
 * wire 仕様の確定事項 #2: "実装しない分岐を残さない"). */
#define DM_WRITE_KIND_STEPS 0
#define DM_WRITE_KIND_NAME 1

/* ---- wire layout (little-endian, explicit byte offsets) ------------------ */
#define DM_WIRE_STEP 5 /* action u8 + keycode u32 */
/* READ v1 slot region: hdr(4) + K * (used_len u8 + L * step). Byte-identical
 * between v1 and v2 blobs -- this is what lets a v1-only decoder keep reading
 * a v2 image's steps correctly. */
#define DM_READ_HDR 4
#define DM_READ_SLOT (1 + DM_STEPS * DM_WIRE_STEP)         /* 81 */
#define DM_READ_WIRE_LEN_V1 (DM_READ_HDR + DM_SLOTS * DM_READ_SLOT) /* 1624 */
/* READ v2 name block, appended after the v1 slot region: name_len u8 + name[DM_NAME_MAX]. */
#define DM_READ_NAME (1 + DM_NAME_MAX)                       /* 17 */
#define DM_READ_NAMES_LEN (DM_SLOTS * DM_READ_NAME)          /* 340 */
/* Offset of the name block within the READ wire == size of the v1 slot region. */
#define DM_READ_NAMES_BASE DM_READ_WIRE_LEN_V1
/* READ wire as emitted today (v1 slots + v2 names): 1964 B, inside the 2048 B
 * tunnel blob budget without a firmware rebuild (PLAN フェーズ8). */
#define DM_READ_WIRE_LEN (DM_READ_WIRE_LEN_V1 + DM_READ_NAMES_LEN) /* 1964 */
/* WRITE (steps, one slot, v1 forever): version u8 + slot u8 + used_len u8 + used_len * step */
#define DM_WRITE_HDR 3
#define DM_WRITE_MAX (DM_WRITE_HDR + DM_STEPS * DM_WIRE_STEP)
/* WRITE (name, one slot, v2 only): version u8 + slot u8 + kind u8 + name_len u8
 * + name[DM_NAME_MAX] = 20 B, ALWAYS (no shorter form is accepted). */
#define DM_NAME_WRITE_LEN (4 + DM_NAME_MAX)

struct dm_step {
    uint8_t action;   /* DM_ACT_* */
    uint32_t keycode; /* ZMK usage: page<<16 | id, implicit mods in bits 24-31 */
};

struct dm_slot {
    uint8_t len; /* number of valid steps, 0..DM_STEPS */
    struct dm_step steps[DM_STEPS];
    uint8_t name_len;          /* 0..DM_NAME_MAX; 0 = unnamed (PLAN フェーズ8) */
    uint8_t name[DM_NAME_MAX]; /* opaque bytes past name_len are always 0 */
};

/* Live (validated) slot, or NULL if idx out of range. Lock-free reader. */
const struct dm_slot *dm_live_slot(uint8_t idx);

/* One-slot WRITE wire -> validate, publish atomically, persist that slot. */
int dm_apply_write_wire(const uint8_t *buf, uint16_t len);

/* Full READ wire (all slots) into buf (>= DM_READ_WIRE_LEN). */
int dm_encode_read_wire(uint8_t *buf, uint16_t cap, uint16_t *out_len);

/* Expected READ wire length. */
uint16_t dm_read_wire_len(void);
