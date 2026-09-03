/*
 * timing (tmg) wire v1 — fixed 96 bytes.
 *
 * The only fixed-length wire in the module: the header must describe exactly
 * this build's shape or the blob is refused outright (fail closed), which is why
 * the header/shape boundary cases matter more here than anywhere else.
 *
 * Also covers the two zmk-fork override seams the store implements, since they
 * are what actually consumes the decoded values.
 */

#include <errno.h>
#include <string.h>

#include <zmk/debounce.h>
#include <zmk/torabo_timing.h>
#include <zmk_timing_config/config.h>

#include "torabo_test.h"

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Synthetic 96B wire: both ht blocks distinct, positional slots partly used. */
static void build_wire(uint8_t buf[TMG_WIRE_LEN]) {
    memset(buf, 0, TMG_WIRE_LEN);
    buf[0] = TMG_WIRE_VERSION;
    buf[1] = TMG_HT_NODES;
    buf[2] = TMG_HT_POS_SLOTS;
    buf[3] = 7;  /* debounce_press_ms  (1..100) */
    buf[4] = 12; /* debounce_release_ms */

    uint8_t *mt = &buf[TMG_WIRE_HDR];
    put16(&mt[TMG_HT_OFF_TAPPING_TERM], 180);
    put16(&mt[TMG_HT_OFF_QUICK_TAP], 150);
    put16(&mt[TMG_HT_OFF_PRIOR_IDLE], TMG_U16_DISABLED);
    mt[TMG_HT_OFF_FLAVOR] = 1; /* balanced */
    mt[TMG_HT_OFF_FLAGS] = ZMK_TORABO_HT_FLAG_RETRO_TAP;
    mt[TMG_HT_OFF_POS_COUNT] = 3;
    mt[TMG_HT_OFF_POSITIONS + 0] = 11;
    mt[TMG_HT_OFF_POSITIONS + 1] = 22;
    mt[TMG_HT_OFF_POSITIONS + 2] = 33;

    uint8_t *lt = &buf[TMG_WIRE_HDR + TMG_HT_BLOCK];
    put16(&lt[TMG_HT_OFF_TAPPING_TERM], 220);
    put16(&lt[TMG_HT_OFF_QUICK_TAP], TMG_U16_DISABLED);
    put16(&lt[TMG_HT_OFF_PRIOR_IDLE], 75);
    lt[TMG_HT_OFF_FLAVOR] = 2; /* tap-preferred */
    lt[TMG_HT_OFF_FLAGS] =
        ZMK_TORABO_HT_FLAG_HOLD_TRIGGER_ON_RELEASE | ZMK_TORABO_HT_FLAG_HOLD_WHILE_UNDECIDED;
    lt[TMG_HT_OFF_POS_COUNT] = (uint8_t)TMG_HT_POS_SLOTS; /* all slots used */
    for (uint8_t i = 0; i < TMG_HT_POS_SLOTS; i++) {
        lt[TMG_HT_OFF_POSITIONS + i] = (uint8_t)(40 + i);
    }
}

void test_timing(void) {
    torabo_test_begin("timing tmg wire v1 (96B)");

    uint8_t wire[TMG_WIRE_LEN];
    uint8_t out[TMG_WIRE_CAP];
    uint8_t bad[TMG_WIRE_LEN];
    uint16_t out_len = 0;

    T_EQ_INT(TMG_WIRE_LEN, 96, "wire length is exactly 96 bytes");
    T_EQ_INT(TMG_WIRE_CAP, TMG_WIRE_LEN, "cap == len (fixed-size wire)");

    /* ---- header framing helper ------------------------------------------- */
    build_wire(wire);
    T_EQ_INT(tmg_expected_len(wire), TMG_WIRE_LEN, "expected_len() accepts this build's header");
    memcpy(bad, wire, TMG_WIRE_LEN);
    bad[0] = 2;
    T_EQ_INT(tmg_expected_len(bad), 0, "expected_len() refuses version 2");
    bad[0] = TMG_WIRE_VERSION;
    bad[1] = 3;
    T_EQ_INT(tmg_expected_len(bad), 0, "expected_len() refuses a different ht node count");
    bad[1] = TMG_HT_NODES;
    bad[2] = 16;
    T_EQ_INT(tmg_expected_len(bad), 0, "expected_len() refuses a different positional slot count");
    T_EQ_INT(tmg_expected_len(NULL), 0, "expected_len(NULL) = 0");

    /* ---- golden round-trip ------------------------------------------------ */
    T_EQ_INT(tmg_apply_wire(wire, TMG_WIRE_LEN), 0, "apply synthetic 96B wire succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(tmg_encode_wire(out, sizeof(out), &out_len), 0, "re-encode succeeds");
    T_EQ_INT(out_len, TMG_WIRE_LEN, "re-encoded length is 96");
    T_EQ_MEM(out, wire, TMG_WIRE_LEN, "96B round-trip is byte-identical");

    T_EQ_INT(out[0], TMG_WIRE_VERSION, "header +0 version 1");
    T_EQ_INT(out[1], TMG_HT_NODES, "header +1 ht node count 2");
    T_EQ_INT(out[2], TMG_HT_POS_SLOTS, "header +2 positional slots 32");
    T_EQ_INT(out[5], 0, "header +5 reserved stays zero");
    T_EQ_INT(out[6], 0, "header +6 reserved stays zero");
    T_EQ_INT(out[7], 0, "header +7 reserved stays zero");

    /* the 0xFFFF "disabled" sentinel must survive decode(-1) -> encode */
    T_EQ_INT((uint16_t)(out[TMG_WIRE_HDR + TMG_HT_OFF_PRIOR_IDLE] |
                        (out[TMG_WIRE_HDR + TMG_HT_OFF_PRIOR_IDLE + 1] << 8)),
             TMG_U16_DISABLED, "mt require_prior_idle 'disabled' sentinel round-trips");
    T_EQ_INT((uint16_t)(out[TMG_WIRE_HDR + TMG_HT_BLOCK + TMG_HT_OFF_QUICK_TAP] |
                        (out[TMG_WIRE_HDR + TMG_HT_BLOCK + TMG_HT_OFF_QUICK_TAP + 1] << 8)),
             TMG_U16_DISABLED, "lt quick_tap 'disabled' sentinel round-trips");

    /* ---- the store now feeds the fork's override seams -------------------- */
    struct zmk_torabo_ht_params p;
    struct device mt_dev = {.name = TMG_NODE_NAME_MT};
    struct device lt_dev = {.name = TMG_NODE_NAME_LT};
    struct device other = {.name = "some_other_hold_tap"};

    memset(&p, 0, sizeof(p));
    T_CHECK(zmk_torabo_ht_override(&mt_dev, &p), "override answers for \"mod_tap\"");
    T_EQ_INT(p.tapping_term_ms, 180, "mt tapping_term reaches the behavior");
    T_EQ_INT(p.quick_tap_ms, 150, "mt quick_tap reaches the behavior");
    T_EQ_INT(p.require_prior_idle_ms, -1, "mt disabled sentinel decodes to -1");
    T_EQ_INT(p.flavor, 1, "mt flavor reaches the behavior");
    T_EQ_INT(p.pos_count, 3, "mt positional count reaches the behavior");
    T_EQ_INT(p.positions[2], 33, "mt positional list reaches the behavior");

    memset(&p, 0, sizeof(p));
    T_CHECK(zmk_torabo_ht_override(&lt_dev, &p), "override answers for \"layer_tap\"");
    T_EQ_INT(p.tapping_term_ms, 220, "lt tapping_term reaches the behavior");
    T_EQ_INT(p.pos_count, TMG_HT_POS_SLOTS, "lt uses all 32 positional slots");

    T_CHECK(!zmk_torabo_ht_override(&other, &p),
            "any other hold-tap node keeps its devicetree config");
    T_CHECK(!zmk_torabo_ht_override(NULL, &p), "NULL device declines");

    uint8_t press = 0, release = 0;
    T_CHECK(zmk_torabo_debounce_split_values(&press, &release),
            "split debounce values are available after a write");
    T_EQ_INT(press, 7, "split debounce press_ms");
    T_EQ_INT(release, 12, "split debounce release_ms");

    struct zmk_debounce_config dt = {.debounce_press_ms = 99, .debounce_release_ms = 99};
    const struct zmk_debounce_config *eff = zmk_torabo_debounce_effective(&dt);
    T_CHECK(eff != &dt, "kscan gets the live config, not the devicetree one");
    T_EQ_INT(eff->debounce_press_ms, 7, "effective debounce press_ms");
    T_EQ_INT(eff->debounce_release_ms, 12, "effective debounce release_ms");

    /* ---- boundaries: fail closed ----------------------------------------- */
    T_EQ_INT(tmg_apply_wire(wire, TMG_WIRE_LEN - 1), -EINVAL, "95 bytes is rejected");
    T_EQ_INT(tmg_apply_wire(wire, TMG_WIRE_LEN + 1), -EINVAL, "97 bytes is rejected");
    T_EQ_INT(tmg_apply_wire(NULL, TMG_WIRE_LEN), -EINVAL, "NULL buffer is rejected");

    memcpy(bad, wire, TMG_WIRE_LEN);
    bad[0] = 2;
    T_EQ_INT(tmg_apply_wire(bad, TMG_WIRE_LEN), -EINVAL, "version 2 is rejected");
    memcpy(bad, wire, TMG_WIRE_LEN);
    bad[1] = 1;
    T_EQ_INT(tmg_apply_wire(bad, TMG_WIRE_LEN), -EINVAL, "ht node count 1 is rejected");
    memcpy(bad, wire, TMG_WIRE_LEN);
    bad[2] = 8;
    T_EQ_INT(tmg_apply_wire(bad, TMG_WIRE_LEN), -EINVAL, "positional slot count 8 is rejected");

    memset(out, 0xAA, sizeof(out));
    tmg_encode_wire(out, sizeof(out), &out_len);
    T_EQ_MEM(out, wire, TMG_WIRE_LEN, "rejected writes left the live snapshot unchanged");

    T_EQ_INT(tmg_encode_wire(out, TMG_WIRE_LEN - 1, &out_len), -ENOMEM,
             "encode into a 95B buffer is rejected");

    /* ---- clamping --------------------------------------------------------- */
    memcpy(bad, wire, TMG_WIRE_LEN);
    bad[3] = 0;   /* debounce below min */
    bad[4] = 200; /* debounce above max */
    put16(&bad[TMG_WIRE_HDR + TMG_HT_OFF_TAPPING_TERM], 5);    /* below min */
    bad[TMG_WIRE_HDR + TMG_HT_OFF_FLAVOR] = 9;                 /* unknown flavor */
    bad[TMG_WIRE_HDR + TMG_HT_OFF_FLAGS] = 0xFF;               /* undefined flag bits */
    bad[TMG_WIRE_HDR + TMG_HT_OFF_POS_COUNT] = 200;            /* beyond the slot count */
    put16(&bad[TMG_WIRE_HDR + TMG_HT_BLOCK + TMG_HT_OFF_TAPPING_TERM], 9000); /* above max */
    T_EQ_INT(tmg_apply_wire(bad, TMG_WIRE_LEN), 0, "an in-shape wire with wild values is ACCEPTED");
    memset(out, 0xAA, sizeof(out));
    tmg_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[3], TMG_DEBOUNCE_MIN, "debounce press clamps to MIN");
    T_EQ_INT(out[4], TMG_DEBOUNCE_MAX, "debounce release clamps to MAX");
    T_EQ_INT((uint16_t)(out[TMG_WIRE_HDR + TMG_HT_OFF_TAPPING_TERM] |
                        (out[TMG_WIRE_HDR + TMG_HT_OFF_TAPPING_TERM + 1] << 8)),
             TMG_TAPPING_TERM_MIN, "tapping_term clamps to MIN");
    T_EQ_INT((uint16_t)(out[TMG_WIRE_HDR + TMG_HT_BLOCK + TMG_HT_OFF_TAPPING_TERM] |
                        (out[TMG_WIRE_HDR + TMG_HT_BLOCK + TMG_HT_OFF_TAPPING_TERM + 1] << 8)),
             TMG_TAPPING_TERM_MAX, "tapping_term clamps to MAX");
    T_EQ_INT(out[TMG_WIRE_HDR + TMG_HT_OFF_FLAVOR], 0, "unknown flavor falls back to 0");
    T_EQ_INT(out[TMG_WIRE_HDR + TMG_HT_OFF_FLAGS], TMG_FLAGS_MASK,
             "undefined flag bits are masked off");
    T_EQ_INT(out[TMG_WIRE_HDR + TMG_HT_OFF_POS_COUNT], TMG_HT_POS_SLOTS,
             "pos_count clamps to the slot count");
}
