/*
 * combos (cb) wire v1 — READ returns all 16 slots (420 B), WRITE carries one
 * 26-byte slot row. The store keeps the wire rows verbatim, so the round-trip is
 * exact by construction; what actually needs pinning is the FIELD OFFSETS and
 * the structural validation, both of which the app codec mirrors.
 *
 * cb_fetch_pending() additionally resolves a row into an engine-ready combo; the
 * host build fakes the devicetree behavior nodes (see stubs/zephyr/devicetree.h),
 * so the target_type -> behavior mapping is checked by NAME, not by device
 * pointer.
 */

#include <errno.h>
#include <string.h>

#include <zmk_dynamic_keymap/dcombo.h>

#include "torabo_test.h"

#define CB_MAGIC_LO 0x63 /* 'c' */
#define CB_MAGIC_HI 0x62 /* 'b' */

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One valid 26-byte row with distinct values in every field. */
static void build_row(uint8_t row[CB_WIRE_SLOT], uint8_t pos_count, uint8_t tgt) {
    memset(row, 0, CB_WIRE_SLOT);
    row[CB_W_ENABLED] = 1;
    row[CB_W_POS_COUNT] = pos_count;
    for (uint8_t i = 0; i < pos_count; i++) {
        row[CB_W_POSITIONS + i] = (uint8_t)(10 + i);
    }
    put32(&row[CB_W_LAYER_MASK], 0x0000000Du);
    put16(&row[CB_W_TIMEOUT], 45);
    put16(&row[CB_W_PRIOR_IDLE], 130);
    row[CB_W_FLAGS] = CB_FLAG_SLOW_RELEASE;
    row[CB_W_TGT_TYPE] = tgt;
    put32(&row[CB_W_TGT_P1], 0xDEADBEEFu);
    put32(&row[CB_W_TGT_P2], 0x00C0FFEEu);
}

static uint16_t build_write(uint8_t buf[CB_WRITE_MAX], uint8_t slot, uint8_t pos_count,
                            uint8_t tgt) {
    buf[0] = CB_VERSION;
    buf[1] = slot;
    build_row(&buf[CB_WRITE_HDR], pos_count, tgt);
    return CB_WRITE_MAX;
}

void test_combos(void) {
    torabo_test_begin("combos cb wire v1");

    static uint8_t read_buf[CB_READ_WIRE_LEN];
    uint8_t wbuf[CB_WRITE_MAX];
    uint8_t bad[CB_WRITE_MAX];
    uint16_t out_len = 0;

    T_EQ_INT(cb_read_wire_len(), CB_READ_WIRE_LEN, "cb_read_wire_len() = 420");

    /* ---- an untouched store reads back all-disabled ----------------------- */
    memset(read_buf, 0xAA, sizeof(read_buf));
    T_EQ_INT(cb_encode_read_wire(read_buf, sizeof(read_buf), &out_len), 0, "READ encode succeeds");
    T_EQ_INT(out_len, CB_READ_WIRE_LEN, "READ length is 420");
    T_EQ_INT(read_buf[0], CB_MAGIC_LO, "header +0 magic lo 0x63");
    T_EQ_INT(read_buf[1], CB_MAGIC_HI, "header +1 magic hi 0x62");
    T_EQ_INT(read_buf[2], CB_VERSION, "header +2 version 1");
    T_EQ_INT(read_buf[3], CB_SLOTS, "header +3 slot count 16");
    {
        int all_disabled = 1;
        for (uint8_t s = 0; s < CB_SLOTS; s++) {
            if (read_buf[CB_READ_HDR + (uint32_t)s * CB_WIRE_SLOT + CB_W_ENABLED] != 0) {
                all_disabled = 0;
            }
        }
        T_CHECK(all_disabled, "every slot starts disabled (fail-safe: never captures a key)");
    }

    /* ---- write one slot, read it back verbatim ---------------------------- */
    const uint8_t slot = 5;
    build_write(wbuf, slot, 3, CB_TGT_KP);
    T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX), 0, "one-slot WRITE succeeds");

    memset(read_buf, 0xAA, sizeof(read_buf));
    cb_encode_read_wire(read_buf, sizeof(read_buf), &out_len);
    const uint8_t *row = &read_buf[CB_READ_HDR + (uint32_t)slot * CB_WIRE_SLOT];
    T_EQ_MEM(row, &wbuf[CB_WRITE_HDR], CB_WIRE_SLOT, "the 26-byte row round-trips byte-identically");
    T_EQ_INT(row[CB_W_POS_COUNT], 3, "pos_count at offset 1");
    T_EQ_INT(row[CB_W_POSITIONS + 2], 12, "positions at offset 2");
    T_EQ_INT(row[CB_W_LAYER_MASK], 0x0D, "layer_mask at offset 8 (LE)");
    T_EQ_INT(row[CB_W_TIMEOUT], 45, "timeout at offset 12 (LE)");
    T_EQ_INT(row[CB_W_PRIOR_IDLE], 130, "require_prior_idle at offset 14 (LE)");
    T_EQ_INT(row[CB_W_FLAGS], CB_FLAG_SLOW_RELEASE, "flags at offset 16");
    T_EQ_INT(row[CB_W_TGT_TYPE], CB_TGT_KP, "target type at offset 17");
    T_EQ_INT(row[CB_W_TGT_P1 + 3], 0xDE, "target param1 at offset 18 (LE)");
    T_EQ_INT(row[CB_W_TGT_P2 + 2], 0xC0, "target param2 at offset 22 (LE)");

    /* neighbours untouched */
    T_EQ_INT(read_buf[CB_READ_HDR + (uint32_t)(slot + 1) * CB_WIRE_SLOT + CB_W_ENABLED], 0,
             "the next slot is untouched");

    /* ---- the engine pull resolves rows and honours the fail-open rules ---- */
    struct cb_combo combos[CB_SLOTS];
    uint32_t seq = 0;
    T_CHECK(cb_fetch_pending(combos, &seq), "first fetch sees the staged config");
    T_CHECK(!cb_fetch_pending(combos, &seq), "a second fetch with no change returns false");

    T_CHECK(combos[slot].enabled, "the written slot resolves to an enabled combo");
    T_EQ_INT(combos[slot].key_position_len, 3, "key_position_len");
    T_EQ_INT(combos[slot].key_positions[2], 12, "key_positions");
    T_EQ_INT(combos[slot].timeout_ms, 45, "timeout_ms");
    T_EQ_INT(combos[slot].require_prior_idle_ms, 130, "require_prior_idle_ms");
    T_EQ_INT(combos[slot].layer_mask, 0x0000000Du, "layer_mask");
    T_CHECK(combos[slot].slow_release, "slow_release flag");
    T_EQ_INT(combos[slot].behavior.param1, 0xDEADBEEFu, "behavior param1");
    T_EQ_INT(combos[slot].behavior.param2, 0x00C0FFEEu, "behavior param2");
    T_CHECK(combos[slot].behavior.behavior_dev != NULL, "a key-press target resolves to a device");
    T_CHECK(!combos[0].enabled, "an unwritten slot stays disabled");

    /* every target type must map to a DISTINCT behavior device */
    {
        const char *devs[CB_TGT_MAX + 1] = {0};
        for (uint8_t t = 0; t <= CB_TGT_MAX; t++) {
            build_write(wbuf, (uint8_t)t, 2, t);
            T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX), 0, "stage one target type");
        }
        seq = 0;
        cb_fetch_pending(combos, &seq);
        int distinct = 1;
        for (uint8_t t = 0; t <= CB_TGT_MAX; t++) {
            devs[t] = combos[t].behavior.behavior_dev;
            for (uint8_t u = 0; u < t; u++) {
                if (devs[u] && devs[t] && strcmp(devs[u], devs[t]) == 0) {
                    distinct = 0;
                }
            }
        }
        T_CHECK(distinct, "each target type resolves to its own behavior device");
    }

    /* a row with pos_count 0 can never fire, even when marked enabled */
    build_write(wbuf, 12, 0, CB_TGT_MO);
    T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX), 0, "a 0-position row is stored");
    seq = 0;
    cb_fetch_pending(combos, &seq);
    T_CHECK(!combos[12].enabled, "a 0-position combo resolves to disabled (never captures a key)");

    /* ---- boundaries: structural validation -------------------------------- */
    build_write(wbuf, 2, 4, CB_TGT_TOG);
    T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX), 0, "restore a good write");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[0] = 2;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "WRITE version 2 is rejected");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[1] = CB_SLOTS;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "slot index 16 is rejected");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[CB_WRITE_HDR + CB_W_ENABLED] = 2;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "enabled > 1 is rejected");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[CB_WRITE_HDR + CB_W_POS_COUNT] = CB_MAX_POS + 1;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "pos_count 7 is rejected");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[CB_WRITE_HDR + CB_W_FLAGS] = 0x02;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "an undefined flag bit is rejected");

    memcpy(bad, wbuf, CB_WRITE_MAX);
    bad[CB_WRITE_HDR + CB_W_TGT_TYPE] = CB_TGT_MAX + 1;
    T_EQ_INT(cb_apply_write_wire(bad, CB_WRITE_MAX), -EINVAL, "an unknown target type is rejected");

    T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX - 1), -EINVAL, "a short WRITE is rejected");
    T_EQ_INT(cb_apply_write_wire(wbuf, CB_WRITE_MAX + 1), -EINVAL, "a long WRITE is rejected");
    T_EQ_INT(cb_apply_write_wire(NULL, CB_WRITE_MAX), -EINVAL, "NULL buffer is rejected");

    memset(read_buf, 0xAA, sizeof(read_buf));
    cb_encode_read_wire(read_buf, sizeof(read_buf), &out_len);
    T_EQ_MEM(&read_buf[CB_READ_HDR + 2 * CB_WIRE_SLOT], &wbuf[CB_WRITE_HDR], CB_WIRE_SLOT,
             "rejected writes left slot 2 unchanged");

    T_EQ_INT(cb_encode_read_wire(read_buf, CB_READ_WIRE_LEN - 1, &out_len), -ENOMEM,
             "READ into a too-small buffer is rejected");
    T_EQ_INT(cb_encode_read_wire(NULL, CB_READ_WIRE_LEN, &out_len), -ENOMEM,
             "READ into NULL is rejected");
}
