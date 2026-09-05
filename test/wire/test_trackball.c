/*
 * trackball (ztc) wire v2/v3 — golden round-trip + boundaries.
 *
 * Fixture is synthetic: a hand-built v3 blob whose every field is a distinct,
 * in-range dummy value, so a byte that moves shows up as a mismatch rather than
 * being masked by a zero.
 *
 * v3 = v2 layout + a 4-byte coast TRAILER, so every v2 offset must survive.
 * WRITE accepts v2 and v3; READ always emits v3 (PLAN §0.4).
 */

#include <errno.h>
#include <string.h>

#include <zmk_trackball_config/config.h>

#include "torabo_test.h"

#define ZTC_MAGIC_LO 0x74
#define ZTC_MAGIC_HI 0x7A

static uint16_t ztc_len_v2(void) {
    return (uint16_t)(ZTC_WIRE_HDR + (uint32_t)ZTC_MAX_LAYERS * ZTC_WIRE_LAYER);
}

/* Build a fully-populated v3 wire with per-layer distinct values. */
static uint16_t build_v3(uint8_t *buf, uint16_t cap) {
    const uint16_t len = ztc_len_v2() + ZTC_WIRE_COAST;
    memset(buf, 0, cap);
    buf[0] = ZTC_MAGIC_LO;
    buf[1] = ZTC_MAGIC_HI;
    buf[2] = 3;                          /* version */
    buf[3] = (uint8_t)ZTC_MAX_LAYERS;    /* layer_count */
    buf[4] = 1;                          /* temp_target */
    buf[5] = 0;                          /* _rsv */
    buf[6] = 0xE8;                       /* temp_timeout_ms = 1000 LE */
    buf[7] = 0x03;
    for (uint8_t i = 0; i < ZTC_MAX_LAYERS; i++) {
        uint8_t *lp = &buf[ZTC_WIRE_HDR + (uint32_t)i * ZTC_WIRE_LAYER];
        lp[0] = (uint8_t)(i % 3);              /* x.role  cycles MOVE/SCROLL/OFF */
        lp[1] = (uint8_t)(i & 1);              /* x.direction */
        lp[2] = (uint8_t)(1 + (i % 32));       /* x.speed_div, in range */
        lp[4] = (uint8_t)((i + 1) % 3);        /* y.role */
        lp[5] = (uint8_t)((i + 1) & 1);        /* y.direction */
        lp[6] = (uint8_t)(1 + ((i + 5) % 32)); /* y.speed_div */
        lp[8] = (uint8_t)(i < 2 ? 1 : 0);      /* temp_enable */
    }
    uint8_t *cp = &buf[ztc_len_v2()];
    cp[0] = 1;  /* coast enable */
    cp[1] = 12; /* friction, in 1..32 */
    cp[2] = 90; /* threshold, in 1..255 */
    return len;
}

void test_trackball(void) {
    torabo_test_begin("trackball ztc wire v3");

    uint8_t wire[ZTC_WIRE_CAP];
    uint8_t out[ZTC_WIRE_CAP];
    uint16_t out_len = 0;

    /* ---- length contract ------------------------------------------------- */
    T_EQ_INT(ztc_wire_len(), ZTC_WIRE_HDR + ZTC_MAX_LAYERS * ZTC_WIRE_LAYER + ZTC_WIRE_COAST,
             "ztc_wire_len() = hdr + N*layer + coast");
    T_EQ_INT(ZTC_WIRE_CAP, ztc_wire_len(), "ZTC_WIRE_CAP == v3 length");

    /* ---- ztc_expected_len(): the one place a header becomes a byte length ---
     * ztc_apply_wire()'s exact-length check and the GATT chunk assembler
     * (torabo_common/wire_asm.h) both call it, so a divergence here is a wire
     * that a client can write but the firmware cannot frame, or vice versa. */
    {
        uint8_t hdr[ZTC_WIRE_HDR];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = ZTC_MAGIC_LO;
        hdr[1] = ZTC_MAGIC_HI;

        hdr[2] = 3;
        T_EQ_INT(ztc_expected_len(hdr), ZTC_WIRE_HDR + ZTC_MAX_LAYERS * ZTC_WIRE_LAYER +
                                            ZTC_WIRE_COAST,
                 "expected_len(v3) = hdr + N*layer + coast, for THIS build's layer count");
        T_EQ_INT(ztc_expected_len(hdr), ztc_wire_len(), "expected_len(v3) == ztc_wire_len()");

        hdr[2] = 2;
        T_EQ_INT(ztc_expected_len(hdr), ZTC_WIRE_HDR + ZTC_MAX_LAYERS * ZTC_WIRE_LAYER,
                 "expected_len(v2) = hdr + N*layer, no coast trailer");

        hdr[2] = 1;
        T_EQ_INT(ztc_expected_len(hdr), 0, "expected_len refuses version 1");
        hdr[2] = 4;
        T_EQ_INT(ztc_expected_len(hdr), 0, "expected_len refuses version 4");

        hdr[2] = 3;
        hdr[0] ^= 0xFF;
        T_EQ_INT(ztc_expected_len(hdr), 0, "expected_len refuses a bad magic");
        hdr[0] = ZTC_MAGIC_LO;

        T_EQ_INT(ztc_expected_len(NULL), 0, "expected_len(NULL) = 0");

        /* The declared layer_count does NOT move the end of a trackball wire —
         * every wire carries ZTC_MAX_LAYERS slots. That is deliberate: it keeps
         * a wild count out of the framing decision so the blob is assembled in
         * full and then rejected by apply, with a log line naming the value. */
        for (unsigned lc = 0; lc <= 255u; lc += 51u) {
            hdr[3] = (uint8_t)lc;
            if (ztc_expected_len(hdr) != ztc_wire_len()) {
                T_CHECK(0, "expected_len ignores the declared layer_count");
                break;
            }
        }
        T_CHECK(1, "expected_len ignores the declared layer_count (0..255)");
        hdr[3] = (uint8_t)ZTC_MAX_LAYERS;

        /* Why this feature had to move onto the chunk assembler: on a 247-byte
         * ATT MTU a single Write carries at most 244 B. This is a statement
         * about the build, not an assertion that 20 layers is the only shape. */
        if (ZTC_MAX_LAYERS == 20) {
            T_EQ_INT(ztc_wire_len(), 252, "20 layers => 252 B, past the 244 B single-write limit");
        }
    }

    /* ---- READ always emits v3, with the frozen header ---------------------- */
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(ztc_encode_wire(out, sizeof(out), &out_len), 0, "encode defaults succeeds");
    T_EQ_INT(out_len, ztc_wire_len(), "encoded length is the v3 length");
    T_EQ_INT(out[0], ZTC_MAGIC_LO, "header +0 magic lo 0x74");
    T_EQ_INT(out[1], ZTC_MAGIC_HI, "header +1 magic hi 0x7A");
    T_EQ_INT(out[2], 3, "header +2 version is always 3 on READ");
    T_EQ_INT(out[3], ZTC_MAX_LAYERS, "header +3 layer_count = ZTC_MAX_LAYERS");

    /* ---- golden round-trip: synthetic v3 -> apply -> encode -> same bytes -- */
    const uint16_t len = build_v3(wire, sizeof(wire));
    T_EQ_INT(ztc_apply_wire(wire, len), 0, "apply synthetic v3 wire succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(ztc_encode_wire(out, sizeof(out), &out_len), 0, "re-encode succeeds");
    T_EQ_INT(out_len, len, "re-encoded length matches");
    T_EQ_MEM(out, wire, len, "v3 round-trip is byte-identical");

    /* ---- v2 is still accepted, and lands with coasting OFF ---------------- */
    uint8_t v2[ZTC_WIRE_CAP];
    memcpy(v2, wire, ztc_len_v2());
    v2[2] = 2; /* version */
    T_EQ_INT(ztc_apply_wire(v2, ztc_len_v2()), 0, "apply a v2 wire (no trailer) succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(ztc_encode_wire(out, sizeof(out), &out_len), 0, "encode after v2 apply");
    T_EQ_INT(out[2], 3, "a v2 write still reads back as v3");
    T_EQ_INT(out[ztc_len_v2() + 0], 0, "v2 write leaves coast DISABLED (pre-v3 behaviour)");
    T_EQ_INT(out[ztc_len_v2() + 1], ZTC_COAST_FRICTION_DEFAULT, "v2 write: default friction");
    T_EQ_INT(out[ztc_len_v2() + 2], ZTC_COAST_THRESHOLD_DEFAULT, "v2 write: default threshold");
    T_EQ_MEM(out, wire, ztc_len_v2(), "the v2 prefix survives untouched under v3");

    /* ---- boundaries: a rejected wire must leave the store unchanged -------- */
    build_v3(wire, sizeof(wire));
    T_EQ_INT(ztc_apply_wire(wire, len), 0, "restore a known-good wire");

    uint8_t bad[ZTC_WIRE_CAP];
    memcpy(bad, wire, len);
    bad[0] ^= 0xFF;
    T_EQ_INT(ztc_apply_wire(bad, len), -EINVAL, "bad magic is rejected");

    memcpy(bad, wire, len);
    bad[2] = 1;
    T_EQ_INT(ztc_apply_wire(bad, len), -EINVAL, "version 1 is rejected");
    bad[2] = 4;
    T_EQ_INT(ztc_apply_wire(bad, len), -EINVAL, "version 4 is rejected");

    memcpy(bad, wire, len);
    T_EQ_INT(ztc_apply_wire(bad, (uint16_t)(len - 1)), -EINVAL, "short v3 length is rejected");
    T_EQ_INT(ztc_apply_wire(bad, ztc_len_v2()), -EINVAL,
             "v3 header with a v2 length is rejected");
    T_EQ_INT(ztc_apply_wire(bad, ZTC_WIRE_HDR - 1), -EINVAL, "sub-header length is rejected");
    T_EQ_INT(ztc_apply_wire(NULL, len), -EINVAL, "NULL buffer is rejected");

    memcpy(bad, wire, len);
    bad[3] = (uint8_t)(ZTC_MAX_LAYERS + 1);
    T_EQ_INT(ztc_apply_wire(bad, len), -EINVAL, "layer_count above the build's max is rejected");

    /* the store still holds the last GOOD wire */
    memset(out, 0xAA, sizeof(out));
    ztc_encode_wire(out, sizeof(out), &out_len);
    T_EQ_MEM(out, wire, len, "rejected writes left the live snapshot unchanged");

    /* ---- clamping: out-of-range fields are pulled in, not rejected --------- */
    memcpy(bad, wire, len);
    bad[ZTC_WIRE_HDR + 0] = 99; /* layer0 x.role: unknown => MOVE */
    bad[ZTC_WIRE_HDR + 2] = 0;  /* layer0 x.speed_div: 0 => SPEED_MIN */
    bad[6] = 0;                 /* temp_timeout 0 => TIMEOUT_MIN */
    bad[7] = 0;
    bad[4] = (uint8_t)ZTC_MAX_LAYERS; /* temp_target out of range */
    bad[ztc_len_v2() + 1] = 200;      /* friction above max */
    T_EQ_INT(ztc_apply_wire(bad, len), 0, "an in-shape wire with wild values is ACCEPTED");
    memset(out, 0xAA, sizeof(out));
    ztc_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[ZTC_WIRE_HDR + 0], 0, "unknown role clamps to MOVE (fail-open)");
    T_EQ_INT(out[ZTC_WIRE_HDR + 2], ZTC_SPEED_MIN, "speed_div 0 clamps to SPEED_MIN");
    T_EQ_INT((uint16_t)(out[6] | (out[7] << 8)), ZTC_TIMEOUT_MIN, "timeout 0 clamps to MIN");
    T_CHECK(out[4] < ZTC_MAX_LAYERS, "out-of-range temp_target falls back in range");
    T_EQ_INT(out[ztc_len_v2() + 1], ZTC_COAST_FRICTION_MAX, "friction clamps to MAX");
}
