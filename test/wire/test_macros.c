/*
 * macros (dm) wire — v1 steps (asymmetric: READ returns all 20 slots, WRITE
 * carries exactly one slot so it never exceeds a single ATT write) plus the
 * PLAN-ext-fw-refactor.md フェーズ8 v2 extension: a per-slot NAME block
 * appended to the READ wire, and a fixed 20B name-only WRITE op.
 *
 * The v2 golden vectors below are chosen to match, byte for byte, the ones in
 * torabo-studio's app-side reference codec test
 * (torabo-studio/src/dynamic_macros/dmacConfig.test.ts — the app codec this
 * firmware is written against per the plan's "wire 仕様の確定事項"): the same
 * slot 5 / name "abc" READ-offset vector, and the same slot 4 / name
 * "コピー" (UTF-8) name-op bytes. Where marked "shared with dmacConfig.test.ts"
 * below, the literal offsets/bytes are copied from that file so a firmware
 * change and an app change that disagree fail in at least one of the two
 * repos' test suites.
 */

#include <errno.h>
#include <string.h>

#include <zmk_dynamic_keymap/dmac.h>

#include "torabo_test.h"

#define DM_MAGIC_LO 0x64 /* 'd' */
#define DM_MAGIC_HI 0x6D /* 'm' */

/* WRITE wire for one slot's STEPS (v1, permanently):
 * [ver=1][slot][used_len][ action, keycode(4 LE) ]* */
static uint16_t build_write(uint8_t *buf, uint8_t slot, uint8_t used) {
    buf[0] = DM_VERSION_V1;
    buf[1] = slot;
    buf[2] = used;
    for (uint8_t i = 0; i < used; i++) {
        uint8_t *p = &buf[DM_WRITE_HDR + (uint32_t)i * DM_WIRE_STEP];
        p[0] = (uint8_t)(i % 3); /* TAP / PRESS / RELEASE */
        uint32_t kc = 0x00070004u + (uint32_t)i + ((uint32_t)slot << 24);
        p[1] = (uint8_t)kc;
        p[2] = (uint8_t)(kc >> 8);
        p[3] = (uint8_t)(kc >> 16);
        p[4] = (uint8_t)(kc >> 24);
    }
    return (uint16_t)(DM_WRITE_HDR + (uint32_t)used * DM_WIRE_STEP);
}

/* WRITE wire for one slot's NAME (v2 only), always DM_NAME_WRITE_LEN (20) B:
 * [ver=2][slot][kind=1][name_len][name[16] zero-padded]. */
static void build_name_write(uint8_t *buf, uint8_t slot, uint8_t kind, const uint8_t *name,
                              uint8_t name_len) {
    memset(buf, 0, DM_NAME_WRITE_LEN);
    buf[0] = DM_VERSION_V2;
    buf[1] = slot;
    buf[2] = kind;
    buf[3] = name_len;
    if (name_len > 0 && name != NULL) {
        memcpy(&buf[4], name, name_len);
    }
}

void test_macros(void) {
    torabo_test_begin("macros dm wire v1+v2 (PLAN phase 8)");

    static uint8_t read_buf[DM_READ_WIRE_LEN];
    uint8_t wbuf[DM_WRITE_MAX];
    uint16_t out_len = 0;

    T_EQ_INT(dm_read_wire_len(), DM_READ_WIRE_LEN, "dm_read_wire_len() = 1964 (v2, was 1624)");
    T_EQ_INT(DM_READ_WIRE_LEN, 1964, "the v2 READ wire is fixed at 1964B");
    T_CHECK(DM_READ_WIRE_LEN <= 2048,
            "the READ wire fits TUNNEL_BLOB_MAX_SIZE (2048 on the field firmware)");

    /* ---- an untouched store reads back as all-empty, valid header --------- */
    memset(read_buf, 0xAA, sizeof(read_buf));
    T_EQ_INT(dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len), 0, "READ encode succeeds");
    T_EQ_INT(out_len, DM_READ_WIRE_LEN, "READ length is 1964 (v1 slot region + v2 name block)");
    T_EQ_INT(read_buf[0], DM_MAGIC_LO, "header +0 magic lo 0x64");
    T_EQ_INT(read_buf[1], DM_MAGIC_HI, "header +1 magic hi 0x6D");
    T_EQ_INT(read_buf[2], DM_VERSION, "header +2 version 2 (PLAN phase 8, was 1)");
    T_EQ_INT(read_buf[3], DM_SLOTS, "header +3 slot count 20");
    {
        int all_empty = 1;
        for (uint8_t k = 0; k < DM_SLOTS; k++) {
            if (read_buf[DM_READ_HDR + (uint32_t)k * DM_READ_SLOT] != 0) {
                all_empty = 0;
            }
        }
        T_CHECK(all_empty, "every slot starts with used_len 0 (fail-safe: a macro that does nothing)");
    }
    {
        int all_unnamed = 1;
        for (uint8_t k = 0; k < DM_SLOTS; k++) {
            if (read_buf[DM_READ_NAMES_BASE + (uint32_t)k * DM_READ_NAME] != 0) {
                all_unnamed = 0;
            }
        }
        T_CHECK(all_unnamed, "an untouched store's name block is all name_len=0 (unnamed)");
    }

    /* ---- write a slot, read it back in place ------------------------------ */
    const uint8_t slot = 7, used = 5;
    const uint16_t wlen = build_write(wbuf, slot, used);
    T_EQ_INT(dm_apply_write_wire(wbuf, wlen), 0, "one-slot WRITE succeeds");

    memset(read_buf, 0xAA, sizeof(read_buf));
    dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len);
    const uint8_t *sp = &read_buf[DM_READ_HDR + (uint32_t)slot * DM_READ_SLOT];
    T_EQ_INT(sp[0], used, "the slot's used_len is what was written");
    T_EQ_MEM(&sp[1], &wbuf[DM_WRITE_HDR], (size_t)used * DM_WIRE_STEP,
             "the written steps round-trip byte-identically");
    {
        int tail_zero = 1;
        for (size_t i = 1 + (size_t)used * DM_WIRE_STEP; i < DM_READ_SLOT; i++) {
            if (sp[i] != 0) {
                tail_zero = 0;
            }
        }
        T_CHECK(tail_zero, "unused steps in the slot are zeroed, not left stale");
    }
    /* neighbours untouched */
    T_EQ_INT(read_buf[DM_READ_HDR + (uint32_t)(slot - 1) * DM_READ_SLOT], 0,
             "the slot below is untouched");
    T_EQ_INT(read_buf[DM_READ_HDR + (uint32_t)(slot + 1) * DM_READ_SLOT], 0,
             "the slot above is untouched");

    /* the live reader agrees with the wire */
    const struct dm_slot *live = dm_live_slot(slot);
    T_CHECK(live != NULL, "dm_live_slot() returns the slot");
    T_EQ_INT(live->len, used, "live slot length matches");
    T_EQ_INT(live->steps[0].action, DM_ACT_TAP, "live step 0 action");
    T_EQ_INT(live->steps[0].keycode, 0x07070004u, "live step 0 keycode (LE decoded)");
    T_CHECK(dm_live_slot(DM_SLOTS) == NULL, "an out-of-range slot index yields NULL");

    /* ---- a full slot (all 16 steps) is the write-size upper bound --------- */
    const uint16_t full = build_write(wbuf, 0, DM_STEPS);
    T_EQ_INT(full, DM_WRITE_MAX, "a 16-step WRITE is exactly DM_WRITE_MAX (83B)");
    T_EQ_INT(dm_apply_write_wire(wbuf, full), 0, "a full 16-step slot is accepted");

    /* ---- an empty write clears a slot ------------------------------------- */
    const uint16_t empty = build_write(wbuf, slot, 0);
    T_EQ_INT(empty, DM_WRITE_HDR, "an empty WRITE is just the 3-byte header");
    T_EQ_INT(dm_apply_write_wire(wbuf, empty), 0, "an empty WRITE is accepted (clears the slot)");
    T_EQ_INT(dm_live_slot(slot)->len, 0, "the slot is now empty");

    /* ---- boundaries ------------------------------------------------------- */
    build_write(wbuf, 3, 4);
    T_EQ_INT(dm_apply_write_wire(wbuf, DM_WRITE_HDR + 4 * DM_WIRE_STEP), 0, "restore a good write");

    uint8_t bad[DM_WRITE_MAX + 8];
    memcpy(bad, wbuf, DM_WRITE_HDR + 4 * DM_WIRE_STEP);
    bad[0] = 2;
    /* This buffer is steps-shaped (23B: 3-byte header + 4 steps), not a v2 name
     * op (which is ALWAYS exactly DM_NAME_WRITE_LEN=20B) -- so relabelling it
     * version 2 does not make it a valid name op, it just fails the v2 length
     * check instead of the old "unknown version" check. Still -EINVAL either
     * way, which is the point: a steps-shaped write can never be reinterpreted
     * as a name write by changing one byte. */
    T_EQ_INT(dm_apply_write_wire(bad, DM_WRITE_HDR + 4 * DM_WIRE_STEP), -EINVAL,
             "a steps-shaped buffer relabelled version 2 is rejected (wrong length for a name op)");

    memcpy(bad, wbuf, DM_WRITE_HDR + 4 * DM_WIRE_STEP);
    bad[1] = DM_SLOTS;
    T_EQ_INT(dm_apply_write_wire(bad, DM_WRITE_HDR + 4 * DM_WIRE_STEP), -EINVAL,
             "slot index 20 is rejected");

    memcpy(bad, wbuf, DM_WRITE_HDR + 4 * DM_WIRE_STEP);
    bad[2] = DM_STEPS + 1;
    T_EQ_INT(dm_apply_write_wire(bad, DM_WRITE_HDR + 4 * DM_WIRE_STEP), -EINVAL,
             "used_len 17 is rejected");

    T_EQ_INT(dm_apply_write_wire(wbuf, DM_WRITE_HDR + 3 * DM_WIRE_STEP), -EINVAL,
             "a length that disagrees with used_len is rejected");
    T_EQ_INT(dm_apply_write_wire(wbuf, DM_WRITE_HDR - 1), -EINVAL, "sub-header length is rejected");
    T_EQ_INT(dm_apply_write_wire(NULL, DM_WRITE_MAX), -EINVAL, "NULL buffer is rejected");

    T_EQ_INT(dm_live_slot(3)->len, 4, "rejected writes left slot 3 unchanged");

    T_EQ_INT(dm_encode_read_wire(read_buf, DM_READ_WIRE_LEN - 1, &out_len), -ENOMEM,
             "READ into a too-small buffer is rejected");
    T_EQ_INT(dm_encode_read_wire(NULL, DM_READ_WIRE_LEN, &out_len), -ENOMEM,
             "READ into NULL is rejected");

    /* ---- an unknown step action is clamped, never executed blindly -------- */
    build_write(wbuf, 9, 2);
    wbuf[DM_WRITE_HDR] = 200;
    T_EQ_INT(dm_apply_write_wire(wbuf, DM_WRITE_HDR + 2 * DM_WIRE_STEP), 0,
             "an in-shape write with an unknown action is ACCEPTED");
    T_EQ_INT(dm_live_slot(9)->steps[0].action, DM_ACT_TAP, "unknown action clamps to TAP");

    /* =========================================================================
     * v2: per-slot NAMEs (PLAN-ext-fw-refactor.md フェーズ8).
     *
     * The golden bytes below are the SAME vectors dmacConfig.test.ts pins in
     * torabo-studio (the app-side reference codec this firmware must match
     * byte-for-byte): slot 5 named "abc" at READ offset 4+20*81+5*17, and slot 4
     * named "コピー" (UTF-8) via the name-only WRITE op. Shared so a firmware
     * change and an app change that disagree on the wire fail one of the two
     * suites, not neither.
     * ========================================================================= */
    uint8_t nbuf[DM_NAME_WRITE_LEN];

    /* ---- shared vector 1: slot 5 = "abc" (dmacConfig.test.ts "decodes the
     * full 1964 B image at the exact documented offsets") -------------------- */
    static const uint8_t ABC[3] = {0x61, 0x62, 0x63}; /* "abc" */
    build_name_write(nbuf, 5, DM_WRITE_KIND_NAME, ABC, sizeof(ABC));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), 0, "name WRITE for slot 5 (\"abc\") succeeds");

    memset(read_buf, 0xAA, sizeof(read_buf));
    T_EQ_INT(dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len), 0, "READ encode succeeds");
    T_EQ_INT(out_len, 1964, "READ is still the fixed 1964B total with a name set");
    {
        /* o = 4 + 20*81 + 5*17 = 1624 + 85 = 1709, per dmacConfig.test.ts */
        const uint32_t o = DM_READ_NAMES_BASE + 5u * DM_READ_NAME;
        T_EQ_INT(o, 1709, "slot 5's name entry is at the documented offset 1709 (shared vector)");
        T_EQ_INT(read_buf[o], 3, "slot 5 name_len == 3 (shared vector)");
        T_EQ_MEM(&read_buf[o + 1], ABC, sizeof(ABC), "slot 5 name bytes == \"abc\" (shared vector)");
        int pad_zero = 1;
        for (uint32_t i = o + 1 + sizeof(ABC); i < o + DM_READ_NAME; i++) {
            if (read_buf[i] != 0) {
                pad_zero = 0;
            }
        }
        T_CHECK(pad_zero, "the rest of slot 5's fixed 16B name field is zero padding");
    }

    /* ---- shared vector 2: slot 4 = "コピー", UTF-8 9 bytes (dmacConfig.test.ts
     * encodeMacroName "writes multi-byte names as UTF-8, length in BYTES") --- */
    static const uint8_t KOPI[9] = {0xe3, 0x82, 0xb3, 0xe3, 0x83, 0x94, 0xe3, 0x83, 0xbc};
    build_name_write(nbuf, 4, DM_WRITE_KIND_NAME, KOPI, sizeof(KOPI));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), 0,
             "name WRITE for slot 4 (\"コピー\", 9 UTF-8 bytes) succeeds");
    dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len);
    {
        const uint32_t o = DM_READ_NAMES_BASE + 4u * DM_READ_NAME;
        T_EQ_INT(read_buf[o], 9, "slot 4 name_len == 9 (shared vector)");
        T_EQ_MEM(&read_buf[o + 1], KOPI, sizeof(KOPI), "slot 4 name bytes == UTF-8 \"コピー\" (shared vector)");
    }
    T_EQ_INT(dm_live_slot(4)->name_len, 9, "live slot 4 name_len matches");
    T_EQ_MEM(dm_live_slot(4)->name, KOPI, sizeof(KOPI), "live slot 4 name bytes match");

    /* ---- a slot never given a name reads back name_len=0 ------------------- */
    T_EQ_INT(dm_live_slot(6)->name_len, 0, "an untouched slot's live name_len is 0");
    {
        const uint32_t o = DM_READ_NAMES_BASE + 6u * DM_READ_NAME;
        T_EQ_INT(read_buf[o], 0, "an untouched slot's READ name_len is 0");
    }

    /* ---- steps WRITE (v1) leaves an existing name untouched ---------------- *
     * This is the backup-v5 restore guarantee (PLAN §9 / フェーズ8): replaying
     * a v1 backup's per-slot steps writes must never erase a name the backup
     * cannot see. */
    build_write(wbuf, 5, 2); /* slot 5 already has name "abc" from above */
    T_EQ_INT(dm_apply_write_wire(wbuf, DM_WRITE_HDR + 2 * DM_WIRE_STEP), 0,
             "a v1 steps WRITE to a named slot succeeds");
    T_EQ_INT(dm_live_slot(5)->len, 2, "slot 5's steps were updated by the v1 write");
    T_EQ_INT(dm_live_slot(5)->name_len, 3, "slot 5's name (\"abc\") survives the steps WRITE");
    dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len);
    {
        const uint32_t o = DM_READ_NAMES_BASE + 5u * DM_READ_NAME;
        T_EQ_INT(read_buf[o], 3, "the READ name block for slot 5 still says name_len 3 after the steps write");
        T_EQ_MEM(&read_buf[o + 1], ABC, sizeof(ABC), "and the name bytes are still \"abc\"");
    }

    /* ---- clearing a name: an empty name op zeroes name_len ----------------- */
    build_name_write(nbuf, 5, DM_WRITE_KIND_NAME, NULL, 0);
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), 0, "an empty name WRITE (name_len=0) succeeds");
    T_EQ_INT(dm_live_slot(5)->name_len, 0, "slot 5's name is now cleared");
    T_EQ_INT(dm_live_slot(5)->len, 2, "clearing the name does not touch slot 5's steps");

    /* ---- name op accept/reject matrix (PLAN フェーズ8 wire 仕様の確定事項) --- */

    /* len must be EXACTLY 20 -- one byte short or long is rejected. */
    build_name_write(nbuf, 1, DM_WRITE_KIND_NAME, ABC, sizeof(ABC));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN - 1), -EINVAL,
             "a 19B v2 write (one short) is rejected");
    uint8_t nbuf21[DM_NAME_WRITE_LEN + 1];
    memcpy(nbuf21, nbuf, DM_NAME_WRITE_LEN);
    nbuf21[DM_NAME_WRITE_LEN] = 0;
    T_EQ_INT(dm_apply_write_wire(nbuf21, DM_NAME_WRITE_LEN + 1), -EINVAL,
             "a 21B v2 write (one long) is rejected");

    /* kind=0 (DM_WRITE_KIND_STEPS) is defined by the wire spec but the app
     * never sends it and this firmware REJECTS it on purpose (確定事項 #2:
     * "実装しない分岐を残さない"). */
    build_name_write(nbuf, 1, DM_WRITE_KIND_STEPS, ABC, sizeof(ABC));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), -EINVAL,
             "ver=2, kind=0 (steps) is rejected");

    /* any other kind byte is equally invalid. */
    build_name_write(nbuf, 1, 2, ABC, sizeof(ABC));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), -EINVAL, "ver=2, kind=2 (unknown) is rejected");

    /* slot out of range. */
    build_name_write(nbuf, DM_SLOTS, DM_WRITE_KIND_NAME, ABC, sizeof(ABC));
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), -EINVAL, "name WRITE slot index 20 is rejected");

    /* name_len over the 16B field. The app never sends this (フェーズ8 確定事項
     * #4: FW must never OUTPUT name_len>16; on WRITE this firmware rejects it
     * rather than silently clamp, matching the "拒否を推奨" steps-kind choice
     * above). */
    build_name_write(nbuf, 1, DM_WRITE_KIND_NAME, ABC, sizeof(ABC));
    nbuf[3] = DM_NAME_MAX + 1;
    T_EQ_INT(dm_apply_write_wire(nbuf, DM_NAME_WRITE_LEN), -EINVAL, "name_len 17 (> DM_NAME_MAX) is rejected");

    T_EQ_INT(dm_live_slot(1)->name_len, 0, "all the rejected name writes above left slot 1 unchanged");

    /* ---- READ total length is fixed at 1964B regardless of content -------- */
    T_EQ_INT(dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len), 0, "final READ encode succeeds");
    T_EQ_INT(out_len, 1964, "the v2 READ wire is always exactly 1964B");
}
