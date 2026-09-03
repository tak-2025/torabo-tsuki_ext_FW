/*
 * trackpad (tp) wire v1/v2/v3 — golden round-trip + boundaries.
 *
 * v3 = v2 with a 3-byte coast block appended to each DEVICE header; the layer
 * stride is unchanged. WRITE accepts v1 (upgraded to the v2 model), v2 and v3;
 * READ always emits v3 with gestures + coast (PLAN §0.4).
 *
 * The per-device `meta` byte is FW-authoritative: the app echoes it back but the
 * firmware re-derives it from Kconfig, so a round-trip must reproduce the
 * Kconfig value, not whatever the wire carried. That is asserted explicitly.
 */

#include <errno.h>
#include <string.h>

#include <zmk_trackpad_config/config.h>

#include "torabo_test.h"

#define TP_MAGIC_LO 0x70
#define TP_MAGIC_HI 0x74

static uint16_t tp_len_v3(uint8_t devs, uint8_t layers) {
    return (uint16_t)(TP_WIRE_HDR +
                      (uint32_t)devs * (TP_WIRE_DEV_HDR_V3 + (uint32_t)layers * TP_WIRE_LAYER_V2));
}

static uint16_t tp_len_v2(uint8_t devs, uint8_t layers) {
    return (uint16_t)(TP_WIRE_HDR +
                      (uint32_t)devs * (TP_WIRE_DEV_HDR + (uint32_t)layers * TP_WIRE_LAYER_V2));
}

static uint16_t tp_len_v1(uint8_t devs, uint8_t layers) {
    return (uint16_t)(TP_WIRE_HDR +
                      (uint32_t)devs * (TP_WIRE_DEV_HDR + (uint32_t)layers * TP_WIRE_LAYER_V1));
}

static void put_bind(uint8_t *p, uint8_t beh, uint8_t mods, uint16_t param) {
    p[0] = beh;
    p[1] = mods;
    p[2] = (uint8_t)param;
    p[3] = (uint8_t)(param >> 8);
}

/* Distinct, in-range dummy values everywhere: any byte that shifts shows up. */
static void put_axis(uint8_t *p, uint8_t seed) {
    p[0] = (uint8_t)(seed % 4);              /* role 0..3 (MOVE..ENCODER) */
    p[1] = (uint8_t)(seed & 1);              /* direction */
    p[2] = (uint8_t)(1 + (seed % 32));       /* step 1..32 */
    put_bind(&p[3], (uint8_t)(seed % 6), (uint8_t)(seed & 0x0F), (uint16_t)(0x100 + seed));
    put_bind(&p[7], (uint8_t)((seed + 3) % 6), 0, (uint16_t)(0x200 + seed));
}

static uint16_t build_v3(uint8_t *buf, uint16_t cap, uint8_t devs, uint8_t layers) {
    const uint16_t len = tp_len_v3(devs, layers);
    memset(buf, 0, cap);
    buf[0] = TP_MAGIC_LO;
    buf[1] = TP_MAGIC_HI;
    buf[2] = 3;
    buf[3] = devs;
    buf[4] = layers;
    buf[5] = TP_FLAG_GESTURES | TP_FLAG_COAST;

    uint32_t o = TP_WIRE_HDR;
    for (uint8_t d = 0; d < devs; d++) {
        buf[o + 0] = d;                       /* device_id */
        buf[o + 1] = 0xFF;                    /* meta: the app echo, must be IGNORED */
        buf[o + 2] = (uint8_t)(d == 0 ? 1 : 0); /* coast enable */
        buf[o + 3] = (uint8_t)(4 + d);        /* coast friction 1..32 */
        buf[o + 4] = (uint8_t)(30 + d);       /* coast threshold 1..255 */
        o += TP_WIRE_DEV_HDR_V3;
        for (uint8_t i = 0; i < layers; i++) {
            put_axis(&buf[o], (uint8_t)(d * 17 + i));
            put_axis(&buf[o + TP_WIRE_AXIS], (uint8_t)(d * 17 + i + 7));
            o += TP_WIRE_AXIS * 2u;
            put_bind(&buf[o + 0], 1, 0x02, (uint16_t)(0x300 + i));                 /* tap */
            put_bind(&buf[o + TP_WIRE_BIND], 3, 0, (uint16_t)(i % 4));             /* tap2 -> mo */
            put_bind(&buf[o + TP_WIRE_BIND * 2u], 2, 0, (uint16_t)(0xE9));         /* hold -> cp */
            put_bind(&buf[o + TP_WIRE_BIND * 3u], 5, 0, (uint16_t)((i + 1) % 4));  /* dtap -> tog */
            o += TP_WIRE_GEST;
        }
    }
    return len;
}

/* What the firmware will report back for device d's meta (from Kconfig). */
static uint8_t expected_meta(uint8_t d) {
    switch (d) {
    case 0:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV0_META;
    case 1:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV1_META;
    case 2:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV2_META;
    default:
        return (uint8_t)CONFIG_ZMK_TRACKPAD_CONFIG_DEV3_META;
    }
}

static void tp_blob_guard(void);

void test_trackpad(void) {
    torabo_test_begin("trackpad tp wire v3");

    static uint8_t wire[TP_WIRE_CAP];
    static uint8_t out[TP_WIRE_CAP];
    static uint8_t bad[TP_WIRE_CAP];
    uint16_t out_len = 0;

    const uint8_t devs = TP_DEFAULT_DEVICE_COUNT;
    const uint8_t layers = (uint8_t)TP_MAX_LAYERS;

    /* ---- length contract -------------------------------------------------- */
    T_EQ_INT(tp_wire_len_for(devs, layers), tp_len_v3(devs, layers),
             "tp_wire_len_for() = hdr + devs*(dev_hdr_v3 + layers*38)");
    T_CHECK(tp_wire_len_for(TP_MAX_DEVICES, layers) <= TP_WIRE_CAP,
            "TP_WIRE_CAP covers the widest v3 wire");

    /* ---- READ always emits v3 with both flags set ------------------------- */
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(tp_encode_wire(out, sizeof(out), &out_len), 0, "encode defaults succeeds");
    T_EQ_INT(out[0], TP_MAGIC_LO, "header +0 magic lo 0x70");
    T_EQ_INT(out[1], TP_MAGIC_HI, "header +1 magic hi 0x74");
    T_EQ_INT(out[2], 3, "header +2 version is always 3 on READ");
    T_EQ_INT(out[4], layers, "header +4 layer_count = TP_MAX_LAYERS");
    T_EQ_INT(out[5], TP_FLAG_GESTURES | TP_FLAG_COAST, "header +5 flags = GESTURES|COAST");
    T_EQ_INT(out_len, tp_len_v3(out[3], layers), "encoded length matches the v3 formula");

    /* ---- golden round-trip ------------------------------------------------ */
    const uint16_t len = build_v3(wire, sizeof(wire), devs, layers);
    T_EQ_INT(tp_apply_wire(wire, len), 0, "apply synthetic v3 wire succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(tp_encode_wire(out, sizeof(out), &out_len), 0, "re-encode succeeds");
    T_EQ_INT(out_len, len, "re-encoded length matches");

    /* meta is FW-authoritative, so patch the expectation before comparing and
     * then assert the substitution actually happened. */
    for (uint8_t d = 0; d < devs; d++) {
        uint32_t off = TP_WIRE_HDR + (uint32_t)d * (TP_WIRE_DEV_HDR_V3 +
                                                    (uint32_t)layers * TP_WIRE_LAYER_V2);
        T_EQ_INT(out[off + 1], expected_meta(d),
                 "device meta comes from Kconfig, not from the wire");
        wire[off + 1] = expected_meta(d);
    }
    T_EQ_MEM(out, wire, len, "v3 round-trip is byte-identical (meta normalised)");

    /* ---- v2 accepted, coast lands disabled, v2 body unchanged ------------- */
    static uint8_t v2[TP_WIRE_CAP];
    memset(v2, 0, sizeof(v2));
    memcpy(v2, wire, TP_WIRE_HDR);
    v2[2] = 2;
    v2[5] = TP_FLAG_GESTURES;
    {
        uint32_t so = TP_WIRE_HDR, do_ = TP_WIRE_HDR;
        for (uint8_t d = 0; d < devs; d++) {
            v2[do_ + 0] = wire[so + 0];
            v2[do_ + 1] = wire[so + 1];
            memcpy(&v2[do_ + TP_WIRE_DEV_HDR], &wire[so + TP_WIRE_DEV_HDR_V3],
                   (size_t)layers * TP_WIRE_LAYER_V2);
            so += TP_WIRE_DEV_HDR_V3 + (uint32_t)layers * TP_WIRE_LAYER_V2;
            do_ += TP_WIRE_DEV_HDR + (uint32_t)layers * TP_WIRE_LAYER_V2;
        }
    }
    T_EQ_INT(tp_apply_wire(v2, tp_len_v2(devs, layers)), 0, "apply a v2 wire succeeds");
    memset(out, 0xAA, sizeof(out));
    tp_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[2], 3, "a v2 write still reads back as v3");
    for (uint8_t d = 0; d < devs; d++) {
        uint32_t off = TP_WIRE_HDR + (uint32_t)d * (TP_WIRE_DEV_HDR_V3 +
                                                    (uint32_t)layers * TP_WIRE_LAYER_V2);
        T_EQ_INT(out[off + 2], 0, "v2 write leaves this device's coast DISABLED");
        T_EQ_INT(out[off + 3], TP_COAST_FRICTION_DEFAULT, "v2 write: default friction");
        T_EQ_INT(out[off + 4], TP_COAST_THRESHOLD_DEFAULT, "v2 write: default threshold");
        T_EQ_MEM(&out[off + TP_WIRE_DEV_HDR_V3],
                 &wire[off + TP_WIRE_DEV_HDR_V3], (size_t)layers * TP_WIRE_LAYER_V2,
                 "v2 layer body survives the v3 device-header growth");
    }

    /* ---- v1 is still accepted (discrete roles upgraded to ENCODER) --------- */
    static uint8_t v1[TP_WIRE_CAP];
    memset(v1, 0, sizeof(v1));
    v1[0] = TP_MAGIC_LO;
    v1[1] = TP_MAGIC_HI;
    v1[2] = 1;
    v1[3] = devs;
    v1[4] = layers;
    v1[5] = 0;
    {
        uint32_t o = TP_WIRE_HDR;
        for (uint8_t d = 0; d < devs; d++) {
            v1[o] = d;
            o += TP_WIRE_DEV_HDR;
            for (uint8_t i = 0; i < layers; i++) {
                v1[o + 0] = 3; /* v1 "Volume" discrete role */
                v1[o + 1] = 0;
                v1[o + 2] = 4;
                v1[o + 3] = 1; /* y: SCROLL */
                v1[o + 4] = 1;
                v1[o + 5] = 8;
                o += TP_WIRE_LAYER_V1;
            }
        }
    }
    T_EQ_INT(tp_apply_wire(v1, tp_len_v1(devs, layers)), 0, "apply a v1 wire succeeds");
    memset(out, 0xAA, sizeof(out));
    tp_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out_len, tp_len_v3(devs, layers), "a v1 write reads back at the v3 length");
    {
        uint32_t a = TP_WIRE_HDR + TP_WIRE_DEV_HDR_V3; /* device 0, layer 0, axis x */
        T_EQ_INT(out[a + 0], TP_ROLE_ENCODER, "v1 discrete role 3 upgrades to ENCODER");
        T_EQ_INT(out[a + 3], TP_BEH_CP, "v1 Volume preset: pos binding is a consumer key");
        T_EQ_INT((uint16_t)(out[a + 5] | (out[a + 6] << 8)), 0xE9, "v1 Volume preset: VOL_UP");
        T_EQ_INT(out[a + 7], TP_BEH_CP, "v1 Volume preset: neg binding is a consumer key");
        T_EQ_INT((uint16_t)(out[a + 9] | (out[a + 10] << 8)), 0xEA, "v1 Volume preset: VOL_DN");
        T_EQ_INT(out[a + TP_WIRE_AXIS], TP_ROLE_SCROLL, "v1 continuous role passes through");
    }

    /* ---- boundaries ------------------------------------------------------- */
    build_v3(wire, sizeof(wire), devs, layers);
    T_EQ_INT(tp_apply_wire(wire, len), 0, "restore a known-good wire");

    memcpy(bad, wire, len);
    bad[0] ^= 0xFF;
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL, "bad magic is rejected");

    memcpy(bad, wire, len);
    bad[2] = 0;
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL, "version 0 is rejected");
    bad[2] = 4;
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL, "version 4 is rejected");

    memcpy(bad, wire, len);
    bad[3] = TP_MAX_DEVICES + 1;
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL, "device_count above TP_MAX_DEVICES is rejected");

    memcpy(bad, wire, len);
    bad[4] = (uint8_t)(TP_MAX_LAYERS + 1);
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL, "layer_count above TP_MAX_LAYERS is rejected");

    T_EQ_INT(tp_apply_wire(wire, (uint16_t)(len - 1)), -EINVAL, "short length is rejected");
    T_EQ_INT(tp_apply_wire(wire, TP_WIRE_HDR - 1), -EINVAL, "sub-header length is rejected");
    T_EQ_INT(tp_apply_wire(NULL, len), -EINVAL, "NULL buffer is rejected");

    /* a v3 header whose flags claim no gestures changes the expected LENGTH,
     * so the same buffer must now be refused rather than silently misparsed */
    memcpy(bad, wire, len);
    bad[5] = TP_FLAG_COAST;
    T_EQ_INT(tp_apply_wire(bad, len), -EINVAL,
             "clearing the GESTURES flag without shortening the wire is rejected");

    memset(out, 0xAA, sizeof(out));
    tp_encode_wire(out, sizeof(out), &out_len);
    for (uint8_t d = 0; d < devs; d++) {
        uint32_t off = TP_WIRE_HDR + (uint32_t)d * (TP_WIRE_DEV_HDR_V3 +
                                                    (uint32_t)layers * TP_WIRE_LAYER_V2);
        wire[off + 1] = expected_meta(d);
    }
    T_EQ_MEM(out, wire, len, "rejected writes left the live snapshot unchanged");

    /* ---- clamping --------------------------------------------------------- */
    memcpy(bad, wire, len);
    {
        uint32_t a = TP_WIRE_HDR + TP_WIRE_DEV_HDR_V3;
        bad[a + 0] = 200; /* role out of range => MOVE */
        bad[a + 2] = 0;   /* step 0 => TP_STEP_MIN */
        bad[a + 3] = 99;  /* binding behavior out of range => NONE */
        bad[TP_WIRE_HDR + 3] = 250; /* coast friction above max */
    }
    T_EQ_INT(tp_apply_wire(bad, len), 0, "an in-shape wire with wild values is ACCEPTED");
    memset(out, 0xAA, sizeof(out));
    tp_encode_wire(out, sizeof(out), &out_len);
    {
        uint32_t a = TP_WIRE_HDR + TP_WIRE_DEV_HDR_V3;
        T_EQ_INT(out[a + 0], TP_ROLE_MOVE, "unknown role clamps to MOVE (fail-open)");
        T_EQ_INT(out[a + 2], TP_STEP_MIN, "step 0 clamps to TP_STEP_MIN");
        T_EQ_INT(out[a + 3], TP_BEH_NONE, "unknown behavior clamps to NONE");
        T_EQ_INT(out[TP_WIRE_HDR + 3], TP_COAST_FRICTION_MAX, "coast friction clamps to MAX");
    }

    tp_blob_guard();
}

/* ---------------------------------------------------------------------------
 * docs/BACKLOG.md B-1: the WRITE guard on the tunnel blob budget.
 *
 * tp_apply_wire() refuses a device_count whose READ wire (always full v3 at
 * TP_MAX_LAYERS) would outgrow CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE.
 * Without it, a 3- or 4-device write is accepted and PERSISTED, after which the
 * tunnel's READ errors forever (it refuses rather than truncating).
 *
 * Two levels are checked:
 *  - tp_read_fits(), the predicate the guard is built on — exercised exactly at
 *    the boundary, which the real Kconfig numbers cannot reach (with any device
 *    count and any of the swept layer counts, tp_wire_len_for never lands on
 *    2048 precisely), so the "== cap still fits" edge is tested by feeding
 *    tp_read_fits the exact encoded length as the cap.
 *  - tp_apply_wire() itself, for EVERY device count, against the real 2048 the
 *    field firmware runs: accepted iff the READ length fits. At 20 layers that
 *    makes 1 and 2 devices pass (771 / 1536 B) and 3 and 4 fail (2301 / 3066 B);
 *    at 10 and 4 layers nothing exceeds the cap and everything passes, which is
 *    exactly why the field firmware has never tripped over this.
 * ------------------------------------------------------------------------- */
static void tp_blob_guard(void) {
    torabo_test_begin("trackpad tunnel-blob WRITE guard (BACKLOG B-1)");

    static uint8_t wire[TP_WIRE_CAP];
    static uint8_t before[TP_WIRE_CAP];
    static uint8_t after[TP_WIRE_CAP];
    uint16_t before_len = 0, after_len = 0;

    const uint8_t layers = (uint8_t)TP_MAX_LAYERS;

    /* ---- the predicate, exactly at the boundary --------------------------- */
    for (uint8_t d = 1; d <= TP_MAX_DEVICES; d++) {
        const uint16_t exact = tp_wire_len_for(d, layers);
        T_CHECK(tp_read_fits(d, exact), "a cap EQUAL to the READ length still fits");
        T_CHECK(!tp_read_fits(d, (uint16_t)(exact - 1)), "one byte under the READ length does not");
        T_CHECK(tp_read_fits(d, (uint16_t)(exact + 1)), "one byte over the READ length fits");
    }

    /* ---- the guard, on the cap the field firmware actually runs ----------- */
    const uint16_t cap = (uint16_t)CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE;

    /* Start from a known-good config and remember exactly what READ returns. */
    uint16_t len = build_v3(wire, sizeof(wire), TP_DEFAULT_DEVICE_COUNT, layers);
    T_EQ_INT(tp_apply_wire(wire, len), 0, "baseline write accepted");
    T_EQ_INT(tp_encode_wire(before, sizeof(before), &before_len), 0, "baseline READ encodes");

    for (uint8_t d = 1; d <= TP_MAX_DEVICES; d++) {
        const uint16_t read_len = tp_wire_len_for(d, layers);
        const bool fits = read_len <= cap;
        char what[128];

        len = build_v3(wire, sizeof(wire), d, layers);
        snprintf(what, sizeof(what), "device_count=%u (READ %u B, cap %u) -> %s", d, read_len, cap,
                 fits ? "accepted" : "REJECTED");
        T_EQ_INT(tp_apply_wire(wire, len), fits ? 0 : -EINVAL, what);

        if (!fits) {
            /* A rejected write must leave the live snapshot byte-identical: the
             * whole point is that the device keeps working. */
            T_EQ_INT(tp_encode_wire(after, sizeof(after), &after_len), 0, "READ still encodes");
            T_EQ_INT(after_len, before_len, "a rejected write did not change the READ length");
            T_EQ_MEM(after, before, before_len, "a rejected write left the live config untouched");
        } else {
            /* Accepted: re-baseline so the next iteration compares against the
             * config that is actually live. */
            T_EQ_INT(tp_encode_wire(before, sizeof(before), &before_len), 0, "re-baseline READ");
        }
    }

    /* The v1 form is short (6 + d*(2 + layers*6)) yet READs back as full v3, so
     * the guard has to fire on it too — this is the shape most likely to sneak a
     * big device_count past a length-only check. */
    if (!tp_read_fits(TP_MAX_DEVICES, cap)) {
        const uint16_t v1_len = tp_len_v1(TP_MAX_DEVICES, layers);
        memset(wire, 0, sizeof(wire));
        wire[0] = TP_MAGIC_LO;
        wire[1] = TP_MAGIC_HI;
        wire[2] = 1;
        wire[3] = TP_MAX_DEVICES;
        wire[4] = layers;
        wire[5] = 0;
        T_EQ_INT(tp_apply_wire(wire, v1_len), -EINVAL,
                 "a SHORT v1 write is rejected too when its v3 READ would not fit");
    }

    /* Restore the default config for any test that runs after this one. */
    len = build_v3(wire, sizeof(wire), TP_DEFAULT_DEVICE_COUNT, layers);
    T_EQ_INT(tp_apply_wire(wire, len), 0, "restore the default device count");
}
