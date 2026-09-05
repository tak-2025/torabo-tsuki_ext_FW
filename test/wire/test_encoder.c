/*
 * encoder (en) wire v1 — hdr(4) + layer_count * { cw ccw btn }, 4B per binding.
 */

#include <errno.h>
#include <string.h>

#include <zmk_encoder_config/config.h>

#include "torabo_test.h"

#define ENC_MAGIC_LO 0x65 /* 'e' */
#define ENC_MAGIC_HI 0x6E /* 'n' */

static void put_bind(uint8_t *p, uint8_t beh, uint8_t mods, uint16_t param) {
    p[0] = beh;
    p[1] = mods;
    p[2] = (uint8_t)param;
    p[3] = (uint8_t)(param >> 8);
}

static uint16_t build_wire(uint8_t *buf, uint16_t cap, uint8_t layers) {
    const uint16_t len = (uint16_t)(ENC_WIRE_HDR + (uint32_t)layers * ENC_WIRE_LAYER);
    memset(buf, 0, cap);
    buf[0] = ENC_MAGIC_LO;
    buf[1] = ENC_MAGIC_HI;
    buf[2] = ENC_WIRE_VERSION;
    buf[3] = layers;
    for (uint8_t i = 0; i < layers; i++) {
        uint8_t *lp = &buf[ENC_WIRE_HDR + (uint32_t)i * ENC_WIRE_LAYER];
        put_bind(&lp[0], (uint8_t)(1 + (i % 5)), (uint8_t)(i & 0x0F), (uint16_t)(0x1000 + i));
        put_bind(&lp[ENC_WIRE_BIND], (uint8_t)(1 + ((i + 2) % 5)), 0, (uint16_t)(0x2000 + i));
        put_bind(&lp[ENC_WIRE_BIND * 2], (uint8_t)(1 + ((i + 4) % 5)), ENC_MOD_LCTL,
                 (uint16_t)(0x3000 + i));
    }
    return len;
}

void test_encoder(void) {
    torabo_test_begin("encoder en wire v1");

    uint8_t wire[ENC_WIRE_CAP];
    uint8_t out[ENC_WIRE_CAP];
    uint8_t bad[ENC_WIRE_CAP];
    uint16_t out_len = 0;
    const uint8_t layers = (uint8_t)ENC_MAX_LAYERS;

    T_EQ_INT(enc_wire_len_for(layers), ENC_WIRE_HDR + layers * ENC_WIRE_LAYER,
             "enc_wire_len_for() = hdr + N*12");
    T_EQ_INT(ENC_WIRE_CAP, enc_wire_len_for(layers), "ENC_WIRE_CAP == full-layer wire length");

    /* ---- enc_expected_len(): the one place a header becomes a byte length ---
     * enc_apply_wire()'s length check and the GATT chunk assembler
     * (torabo_common/wire_asm.h) both call it, so a divergence here is a wire
     * that a client can write but the firmware cannot frame, or vice versa.
     * Unlike the trackball's, this length IS sized by the declared layer_count. */
    {
        uint8_t hdr[ENC_WIRE_HDR];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = ENC_MAGIC_LO;
        hdr[1] = ENC_MAGIC_HI;
        hdr[2] = ENC_WIRE_VERSION;

        /* the layer counts this build could ever be compiled at (run-tests.sh
         * sweeps ZMK_KEYMAP_LAYERS_LEN over 4, 10 and 20) plus the extremes */
        static const uint8_t counts[] = {1, 3, 4, 10, 20};
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            const uint8_t lc = counts[i];
            hdr[3] = lc;
            if (lc <= ENC_MAX_LAYERS) {
                char what[80];
                snprintf(what, sizeof(what), "expected_len(layer_count=%u) = 4 + %u*12 = %u", lc,
                         lc, (unsigned)(ENC_WIRE_HDR + lc * ENC_WIRE_LAYER));
                T_EQ_INT(enc_expected_len(hdr), ENC_WIRE_HDR + lc * ENC_WIRE_LAYER, what);
            } else {
                T_EQ_INT(enc_expected_len(hdr), 0,
                         "expected_len refuses a layer_count above the build's max");
            }
        }

        hdr[3] = layers;
        T_EQ_INT(enc_expected_len(hdr), enc_wire_len_for(layers),
                 "expected_len(ENC_MAX_LAYERS) == enc_wire_len_for()");
        T_EQ_INT(enc_expected_len(hdr), ENC_WIRE_CAP, "...which is ENC_WIRE_CAP");

        hdr[3] = 0;
        T_EQ_INT(enc_expected_len(hdr), 0, "expected_len refuses layer_count 0");
        hdr[3] = (uint8_t)(ENC_MAX_LAYERS + 1);
        T_EQ_INT(enc_expected_len(hdr), 0, "expected_len refuses layer_count max+1");
        hdr[3] = layers;

        hdr[2] = 2;
        T_EQ_INT(enc_expected_len(hdr), 0, "expected_len refuses version 2");
        hdr[2] = ENC_WIRE_VERSION;

        hdr[0] ^= 0xFF;
        T_EQ_INT(enc_expected_len(hdr), 0, "expected_len refuses a bad magic");
        hdr[0] = ENC_MAGIC_LO;

        T_EQ_INT(enc_expected_len(NULL), 0, "expected_len(NULL) = 0");

        /* Why this feature moved onto the chunk assembler too: at 20 layers the
         * wire is exactly 244 B, the largest payload a single ATT Write can
         * carry on a 247-byte MTU — no margin left at all. */
        if (ENC_MAX_LAYERS == 20) {
            T_EQ_INT(enc_wire_len_for(20), 244, "20 layers => 244 B, exactly the single-write limit");
        }
    }

    /* ---- golden round-trip ------------------------------------------------ */
    const uint16_t len = build_wire(wire, sizeof(wire), layers);
    T_EQ_INT(enc_apply_wire(wire, len), 0, "apply synthetic wire succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(enc_encode_wire(out, sizeof(out), &out_len), 0, "encode succeeds");
    T_EQ_INT(out_len, len, "encoded length matches");
    T_EQ_INT(out[0], ENC_MAGIC_LO, "header +0 magic lo 0x65");
    T_EQ_INT(out[1], ENC_MAGIC_HI, "header +1 magic hi 0x6E");
    T_EQ_INT(out[2], ENC_WIRE_VERSION, "header +2 version 1");
    T_EQ_INT(out[3], layers, "header +3 layer_count round-trips");
    T_EQ_MEM(out, wire, len, "round-trip is byte-identical");

    /* ---- a shorter wire is accepted and read back at its own length ------- */
    if (layers >= 3) {
        const uint16_t slen = build_wire(bad, sizeof(bad), 3);
        T_EQ_INT(enc_apply_wire(bad, slen), 0, "a 3-layer wire is accepted");
        memset(out, 0xAA, sizeof(out));
        enc_encode_wire(out, sizeof(out), &out_len);
        T_EQ_INT(out_len, slen, "READ mirrors the stored layer_count");
        T_EQ_MEM(out, bad, slen, "3-layer round-trip is byte-identical");
    }

    /* ---- boundaries ------------------------------------------------------- */
    build_wire(wire, sizeof(wire), layers);
    T_EQ_INT(enc_apply_wire(wire, len), 0, "restore a known-good wire");

    memcpy(bad, wire, len);
    bad[0] ^= 0xFF;
    T_EQ_INT(enc_apply_wire(bad, len), -EINVAL, "bad magic is rejected");

    memcpy(bad, wire, len);
    bad[2] = 2;
    T_EQ_INT(enc_apply_wire(bad, len), -EINVAL, "version 2 is rejected");

    memcpy(bad, wire, len);
    bad[3] = 0;
    T_EQ_INT(enc_apply_wire(bad, len), -EINVAL, "layer_count 0 is rejected");
    bad[3] = (uint8_t)(ENC_MAX_LAYERS + 1);
    T_EQ_INT(enc_apply_wire(bad, len), -EINVAL, "layer_count above the build's max is rejected");

    T_EQ_INT(enc_apply_wire(wire, (uint16_t)(len - 1)), -EINVAL,
             "a wire shorter than its layer_count claims is rejected");
    T_EQ_INT(enc_apply_wire(wire, ENC_WIRE_HDR - 1), -EINVAL, "sub-header length is rejected");
    T_EQ_INT(enc_apply_wire(NULL, len), -EINVAL, "NULL buffer is rejected");
    T_EQ_INT(enc_encode_wire(out, (uint16_t)(len - 1), &out_len), -ENOMEM,
             "encode into a too-small buffer is rejected");

    memset(out, 0xAA, sizeof(out));
    enc_encode_wire(out, sizeof(out), &out_len);
    T_EQ_MEM(out, wire, len, "rejected writes left the live snapshot unchanged");

    /* ---- an unknown behavior is dropped to NONE, never fired blindly ------ */
    memcpy(bad, wire, len);
    bad[ENC_WIRE_HDR + 0] = 99;
    bad[ENC_WIRE_HDR + 1] = 0x0F;
    bad[ENC_WIRE_HDR + 2] = 0xAB;
    T_EQ_INT(enc_apply_wire(bad, len), 0, "an in-shape wire with an unknown behavior is ACCEPTED");
    memset(out, 0xAA, sizeof(out));
    enc_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[ENC_WIRE_HDR + 0], ENC_BEH_NONE, "unknown behavior drops to NONE");
    T_EQ_INT(out[ENC_WIRE_HDR + 1], 0, "...and its mods are cleared too");
    T_EQ_INT(out[ENC_WIRE_HDR + 2], 0, "...and its param is cleared too");

    /* ---- the fail-open accessor ------------------------------------------ */
    const struct enc_snapshot *s = enc_live();
    struct enc_binding b = enc_binding_for(s, (uint8_t)(ENC_MAX_LAYERS + 5), ENC_CW);
    T_EQ_INT(b.behavior, ENC_BEH_NONE, "out-of-range layer yields an unassigned binding");
    T_CHECK(!enc_binding_active(&b), "an unassigned binding is not active");
    b = enc_binding_for(s, 1, 99);
    T_EQ_INT(b.behavior, ENC_BEH_NONE, "an unknown 'which' yields an unassigned binding");
}
