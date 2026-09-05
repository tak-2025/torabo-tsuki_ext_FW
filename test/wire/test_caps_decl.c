/*
 * caps descriptor — PLAN-ext-fw-refactor.md phase 9 "declared" byte vector,
 * re-redesigned 2026-09-03 into one MODULES row of four 4-bit slot values
 * (see stubs/torabo_test_config_decl.h for the fixture: the REAL right-central
 * hardware pattern -- right = central + ball on its standard connector, left =
 * encoder on its standard connector, both halves carry an extension pad).
 *
 * This is the PRIMARY declared fixture, shared with torabo-studio's own caps
 * decoder golden test: MODULES caps word 0x2129 (wire bytes 0x29, 0x21
 * little-endian). A second, additional "double" configuration is fixtured
 * separately in test_caps_decl2.c, purely to stress a repeated-slot-value
 * case this one doesn't exercise.
 *
 * Same 11-feature build as test_caps.c's caps_golden, compiled against
 * stubs/torabo_test_config_decl.h (CONFIG_TORABO_CENTRAL_SIDE=2,
 * SLOT_LEFT_STD=9, SLOT_LEFT_EXT=2, SLOT_RIGHT_STD=1, SLOT_RIGHT_EXT=2)
 * instead of the all-zero baseline. Only two things differ from
 * test_caps.c's caps_golden:
 *   - header _rsv (offset 7): 0x00 -> 0x02 (central = right)
 *   - the MODULES row's caps word (offset 50-51): 0x0000 -> 0x2129
 * Every other byte -- including the trackball and encoder rows -- is
 * identical to the baseline vector, which is the point: placement no longer
 * touches those two rows at all (unlike this phase's two earlier, scrapped
 * designs).
 *
 * The macros row's wire_ver (offset 13) is 0x02 here too, unrelated to phase 9
 * -- it is PLAN phase 8's dm v2 bump, carried over from test_caps.c's baseline
 * so this fixture only asserts what phase 9 itself changed.
 */

#include <errno.h>
#include <string.h>

#include <zmk_torabo_caps/caps.h>

#include "torabo_test.h"

static const uint8_t caps_golden_decl[52] = {
    /* header */
    0x54, 0x43, /* magic 0x4354 "TC" LE */
    0x01,       /* desc_ver */
    0x00,       /* fw major */
    0x01,       /* fw minor */
    0x01,       /* fw patch */
    0x0B,       /* feature_count = 11 */
    0x02,       /* _rsv: bit0-1 central side = 2 (right) -- PLAN phase 9 */
    /* rows, in build_features() order */
    0x01, 0x03, 0x01, 0x00, /* trackball  wire v3, ZTC_COAST (placement not carried here) */
    0x02, 0x02, 0x00, 0x00, /* macros     wire v2 (PLAN phase 8: dm name block) */
    0x03, 0x01, 0x00, 0x00, /* combos     wire v1 */
    0x04, 0x03, 0x10, 0x00, /* trackpad   wire v3, TP_COAST */
    0x05, 0x01, 0x00, 0x00, /* encoder    wire v1 (placement not carried here) */
    0x06, 0x01, 0x03, 0x00, /* led        wire v1, LEFT|RIGHT */
    0x07, 0x01, 0x04, 0x00, /* layers     wire v1, 4 reserved */
    0x08, 0x01, 0x01, 0x00, /* live_feed  wire v1, DIAG */
    0x09, 0x01, 0x01, 0x00, /* rpc_tunnel wire v1, NOTIFY */
    0x0A, 0x01, 0x01, 0x00, /* timing     wire v1, SPLIT_DEBOUNCE */
    0x0B, 0x01, 0x29, 0x21, /* modules    wire v1, LEFT_STD=enc LEFT_EXT=pad RIGHT_STD=ball RIGHT_EXT=pad */
};

void test_caps_decl(void) {
    torabo_test_begin("caps descriptor (PLAN phase 9 declared fixture: real hw pattern)");

    uint8_t buf[TORABO_CAPS_WIRE_CAP];
    uint16_t len = 0;
    memset(buf, 0xAA, sizeof(buf));

    T_EQ_INT(torabo_caps_encode(buf, sizeof(buf), &len), 0, "encode succeeds");
    T_EQ_INT(len, sizeof(caps_golden_decl), "encoded length still 52B (bits changed, no rows added)");
    T_EQ_MEM(buf, caps_golden_decl, sizeof(caps_golden_decl),
             "52-byte descriptor matches the declared-fixture vector");

    /* Pin the exact bit math so a future shift/mask edit fails loudly here
     * rather than only in the opaque byte diff above. */
    T_EQ_INT((buf[7] & TORABO_CAPS_HDR_CENTRAL_MASK) >> TORABO_CAPS_HDR_CENTRAL_SHIFT,
             TORABO_CAPS_SIDE_RIGHT, "_rsv central side decodes to RIGHT");

    /* MODULES row starts at offset 48 (8 header + 10*4 prior rows); its caps
     * word is offset 50-51. 0x2129 little-endian = lo 0x29, hi 0x21. */
    uint16_t modules_caps = (uint16_t)buf[50] | ((uint16_t)buf[51] << 8);
    T_EQ_INT(modules_caps, 0x2129, "MODULES caps word == 0x2129 (shared with torabo-studio's golden)");

    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_LEFT_STD_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_ENCODER, "left std slot decodes to ENCODER");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_LEFT_EXT_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_PAD, "left ext slot decodes to PAD");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_RIGHT_STD_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_BALL, "right std slot decodes to BALL");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_RIGHT_EXT_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_PAD, "right ext slot decodes to PAD");

    /* The trackball/encoder caps words carry NO placement bits any more --
     * this phase's two earlier (scrapped) designs put them here; confirm they
     * are gone by checking these rows equal the undeclared baseline exactly. */
    T_EQ_INT(buf[10], TORABO_CAPS_ZTC_COAST, "trackball caps word carries only COAST, no placement bits");
    T_EQ_INT(buf[11], 0x00, "trackball caps word high byte still 0");
    T_EQ_INT(buf[26], 0x00, "encoder caps word still 0, no placement bits");
    T_EQ_INT(buf[27], 0x00, "encoder caps word high byte still 0");
}
