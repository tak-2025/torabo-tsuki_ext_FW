/*
 * led (le) wire v1 — 72 bytes, fixed shape.
 *
 * The caps byte and rule_max are FW-authoritative: the app echoes them back and
 * the firmware must ignore what it is told, which is what the "app lies about
 * caps" case below checks.
 */

#include <errno.h>
#include <string.h>

#include <zmk_led_config/config.h>

#include "torabo_test.h"

#define LED_MAGIC_LO 0x6C /* 'l' */
#define LED_MAGIC_HI 0x65 /* 'e' */

/* The caps byte this build must report, per torabo_test_config.h. */
#define EXPECT_LED_CAPS (LED_CAP_LEFT_PRESENT | LED_CAP_RIGHT_PRESENT)

static void put_rule(uint8_t *p, uint8_t uc, uint8_t colour, uint8_t pattern, uint8_t param) {
    p[0] = uc;
    p[1] = colour;
    p[2] = pattern;
    p[3] = param;
}

static void build_wire(uint8_t buf[LED_WIRE_CAP]) {
    memset(buf, 0, LED_WIRE_CAP);
    buf[0] = LED_MAGIC_LO;
    buf[1] = LED_MAGIC_HI;
    buf[2] = LED_WIRE_VERSION;
    buf[3] = 0xFF; /* caps: the app echo, must be IGNORED */
    buf[4] = 0xFF; /* rule_max: likewise */
    buf[5] = 0;

    uint32_t o = LED_WIRE_HDR;
    /* left side: 2 rules */
    buf[o++] = 2;
    put_rule(&buf[o + 0 * LED_WIRE_RULE], LED_UC_LINK_LOST, LED_CH_RED, LED_PAT_BLINK_FAST, 0);
    put_rule(&buf[o + 1 * LED_WIRE_RULE], LED_UC_BATTERY_LOW, LED_CH_YG, LED_PAT_DOUBLE, 20);
    o += LED_MAX_RULES * LED_WIRE_RULE;
    /* right side: all 8 slots used */
    buf[o++] = LED_MAX_RULES;
    put_rule(&buf[o + 0 * LED_WIRE_RULE], LED_UC_LINK_LOST, LED_CH_RED, LED_PAT_SOLID, 0);
    put_rule(&buf[o + 1 * LED_WIRE_RULE], LED_UC_BATTERY_LOW, LED_CH_RED, LED_PAT_BLINK_SLOW, 15);
    put_rule(&buf[o + 2 * LED_WIRE_RULE], LED_UC_PROFILE_CHANGED, LED_COLOUR_AUTO,
             LED_PAT_FLASH_LONG, 0);
    put_rule(&buf[o + 3 * LED_WIRE_RULE], LED_UC_LAYER_CHANGED, LED_COLOUR_AUTO, LED_PAT_FLASH, 0);
    put_rule(&buf[o + 4 * LED_WIRE_RULE], LED_UC_ENDPOINT_CHANGED, LED_CH_GRN, LED_PAT_FLASH, 0);
    put_rule(&buf[o + 5 * LED_WIRE_RULE], LED_UC_CAPS_LOCK, LED_CH_YG, LED_PAT_SOLID, 0);
    put_rule(&buf[o + 6 * LED_WIRE_RULE], LED_UC_MODIFIER, LED_CH_RED | LED_CH_GRN,
             LED_PAT_SOLID, LED_MOD_SFT);
    put_rule(&buf[o + 7 * LED_WIRE_RULE], LED_UC_MODIFIER, LED_CH_RED | LED_CH_YG | LED_CH_GRN,
             LED_PAT_BLINK_FAST, LED_MOD_GUI);
}

void test_led(void) {
    torabo_test_begin("led le wire v1 (72B)");

    uint8_t wire[LED_WIRE_CAP];
    uint8_t out[LED_WIRE_CAP];
    uint8_t bad[LED_WIRE_CAP];
    uint16_t out_len = 0;

    T_EQ_INT(LED_WIRE_CAP, 72, "wire is 72 bytes (one ATT write, no Write Long)");

    /* ---- header is FW-authoritative -------------------------------------- */
    build_wire(wire);
    T_EQ_INT(led_apply_wire(wire, LED_WIRE_CAP), 0, "apply synthetic wire succeeds");
    memset(out, 0xAA, sizeof(out));
    T_EQ_INT(led_encode_wire(out, sizeof(out), &out_len), 0, "encode succeeds");
    T_EQ_INT(out_len, LED_WIRE_CAP, "encoded length is 72");
    T_EQ_INT(out[0], LED_MAGIC_LO, "header +0 magic lo 0x6C");
    T_EQ_INT(out[1], LED_MAGIC_HI, "header +1 magic hi 0x65");
    T_EQ_INT(out[2], LED_WIRE_VERSION, "header +2 version 1");
    T_EQ_INT(out[3], EXPECT_LED_CAPS, "header +3 caps comes from Kconfig, not from the wire");
    T_EQ_INT(out[4], LED_MAX_RULES, "header +4 rule_max comes from the firmware");
    T_EQ_INT(out[5], 0, "header +5 reserved stays zero");

    /* ---- golden round-trip (header normalised, body byte-identical) ------- */
    wire[3] = EXPECT_LED_CAPS;
    wire[4] = LED_MAX_RULES;
    T_EQ_MEM(out, wire, LED_WIRE_CAP, "72B round-trip is byte-identical");

    T_EQ_INT(out[LED_WIRE_HDR], 2, "left rule_count round-trips");
    T_EQ_INT(out[LED_WIRE_HDR + 1 + LED_MAX_RULES * LED_WIRE_RULE], LED_MAX_RULES,
             "right rule_count round-trips (all 8 slots)");

    /* ---- boundaries ------------------------------------------------------ */
    memcpy(bad, wire, LED_WIRE_CAP);
    bad[0] ^= 0xFF;
    T_EQ_INT(led_apply_wire(bad, LED_WIRE_CAP), -EINVAL, "bad magic is rejected");

    memcpy(bad, wire, LED_WIRE_CAP);
    bad[2] = 2;
    T_EQ_INT(led_apply_wire(bad, LED_WIRE_CAP), -EINVAL, "version 2 is rejected");

    T_EQ_INT(led_apply_wire(wire, LED_WIRE_CAP - 1), -EINVAL, "71 bytes is rejected");
    T_EQ_INT(led_apply_wire(wire, LED_WIRE_HDR - 1), -EINVAL, "sub-header length is rejected");
    T_EQ_INT(led_apply_wire(NULL, LED_WIRE_CAP), -EINVAL, "NULL buffer is rejected");
    T_EQ_INT(led_encode_wire(out, LED_WIRE_CAP - 1, &out_len), -ENOMEM,
             "encode into a 71B buffer is rejected");

    memset(out, 0xAA, sizeof(out));
    led_encode_wire(out, sizeof(out), &out_len);
    T_EQ_MEM(out, wire, LED_WIRE_CAP, "rejected writes left the live snapshot unchanged");

    /* ---- an uninterpretable rule is dropped, not acted on ----------------- */
    memcpy(bad, wire, LED_WIRE_CAP);
    put_rule(&bad[LED_WIRE_HDR + 1], 200, LED_CH_RED, LED_PAT_SOLID, 0); /* unknown usecase */
    put_rule(&bad[LED_WIRE_HDR + 1 + LED_WIRE_RULE], LED_UC_CAPS_LOCK, LED_CH_RED, 200, 0);
    bad[LED_WIRE_HDR + 1 + 2 * LED_WIRE_RULE + 1] = 0xF8; /* colour bits outside LED_CH_MASK */
    bad[LED_WIRE_HDR] = 3;                                /* three rules on the left now */
    T_EQ_INT(led_apply_wire(bad, LED_WIRE_CAP), 0, "an in-shape wire with wild rules is ACCEPTED");
    memset(out, 0xAA, sizeof(out));
    led_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[LED_WIRE_HDR + 1 + 0], LED_UC_NONE, "unknown usecase drops the rule to all-zero");
    T_EQ_INT(out[LED_WIRE_HDR + 1 + LED_WIRE_RULE + 0], LED_UC_NONE,
             "unknown pattern drops the rule to all-zero");
    T_EQ_INT(out[LED_WIRE_HDR + 1 + 2 * LED_WIRE_RULE + 1] & ~LED_CH_MASK, 0,
             "colour bits outside LED_CH_MASK are masked off");

    /* ---- rule_count above the slot count is clamped, not rejected --------- */
    memcpy(bad, wire, LED_WIRE_CAP);
    bad[LED_WIRE_HDR] = 200;
    T_EQ_INT(led_apply_wire(bad, LED_WIRE_CAP), 0, "an oversized rule_count is accepted");
    memset(out, 0xAA, sizeof(out));
    led_encode_wire(out, sizeof(out), &out_len);
    T_EQ_INT(out[LED_WIRE_HDR], LED_MAX_RULES, "rule_count clamps to LED_MAX_RULES");

    /* ---- the split render command packing (used across the split link) ---- */
    uint8_t colour = 0, pattern = 0;
    led_render_decode(led_render_encode(LED_CH_RED | LED_CH_GRN, LED_PAT_DOUBLE), &colour,
                      &pattern);
    T_EQ_INT(colour, LED_CH_RED | LED_CH_GRN, "render encode/decode preserves the colour mask");
    T_EQ_INT(pattern, LED_PAT_DOUBLE, "render encode/decode preserves the pattern");
    T_EQ_INT(led_render_encode(LED_CH_MASK, LED_PAT_FLASH_LONG), 0x0507u,
             "render command packing is (pattern << 8) | colour");
}
