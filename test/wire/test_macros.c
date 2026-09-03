/*
 * macros (dm) wire v1 — asymmetric: READ returns all 20 slots (1624 B), WRITE
 * carries exactly one slot so it never exceeds a single ATT write.
 *
 * PLAN phase 8 will extend the READ wire to v2 by APPENDING a name block; this
 * test pins v1 so that extension has to be additive.
 */

#include <errno.h>
#include <string.h>

#include <zmk_dynamic_keymap/dmac.h>

#include "torabo_test.h"

#define DM_MAGIC_LO 0x64 /* 'd' */
#define DM_MAGIC_HI 0x6D /* 'm' */

/* WRITE wire for one slot: [ver][slot][used_len][ action, keycode(4 LE) ]* */
static uint16_t build_write(uint8_t *buf, uint8_t slot, uint8_t used) {
    buf[0] = DM_VERSION;
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

void test_macros(void) {
    torabo_test_begin("macros dm wire v1");

    static uint8_t read_buf[DM_READ_WIRE_LEN];
    uint8_t wbuf[DM_WRITE_MAX];
    uint16_t out_len = 0;

    T_EQ_INT(dm_read_wire_len(), DM_READ_WIRE_LEN, "dm_read_wire_len() = 1624");
    T_CHECK(DM_READ_WIRE_LEN <= 2048,
            "the READ wire fits TUNNEL_BLOB_MAX_SIZE (2048 on the field firmware)");

    /* ---- an untouched store reads back as all-empty, valid header --------- */
    memset(read_buf, 0xAA, sizeof(read_buf));
    T_EQ_INT(dm_encode_read_wire(read_buf, sizeof(read_buf), &out_len), 0, "READ encode succeeds");
    T_EQ_INT(out_len, DM_READ_WIRE_LEN, "READ length is 1624");
    T_EQ_INT(read_buf[0], DM_MAGIC_LO, "header +0 magic lo 0x64");
    T_EQ_INT(read_buf[1], DM_MAGIC_HI, "header +1 magic hi 0x6D");
    T_EQ_INT(read_buf[2], DM_VERSION, "header +2 version 1");
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
    T_EQ_INT(dm_apply_write_wire(bad, DM_WRITE_HDR + 4 * DM_WIRE_STEP), -EINVAL,
             "WRITE version 2 is rejected");

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
}
