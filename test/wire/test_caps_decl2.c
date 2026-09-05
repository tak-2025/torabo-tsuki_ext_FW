/*
 * caps descriptor — PLAN-ext-fw-refactor.md phase 9 SECONDARY "declared" byte
 * vector: a double configuration (see stubs/torabo_test_config_decl2.h) —
 * ball on BOTH standard connectors, encoder on BOTH extension connectors,
 * central = left. Not a real fw-test/builder pattern (test_caps_decl.c's
 * fixture is); this one exists purely to exercise a slot value repeated on
 * both sides of each nibble pair, which a single-instance-per-value fixture
 * (like the primary one) cannot: a shift-by-one or a mask leaking into the
 * neighboring 4 bits would still decode to a "plausible" value there, but
 * shows up as a wrong hex digit here.
 *
 * Same 11-feature build as test_caps.c's caps_golden. Only two things differ
 * from the baseline:
 *   - header _rsv (offset 7): 0x04 -> 0x05 (central = left, plus the
 *     always-on WINDOW_READ bit2 that the baseline also carries)
 *   - the MODULES row's caps word (offset 50-51): 0x0000 -> 0x9191
 * Every other byte, trackball/encoder rows included, is identical to the
 * baseline vector -- same point as the primary decl fixture: placement bits
 * live only in the new row.
 */

#include <errno.h>
#include <string.h>

#include <zmk_torabo_caps/caps.h>

#include "torabo_test.h"

static const uint8_t caps_golden_decl2[52] = {
    /* header */
    0x54, 0x43, /* magic 0x4354 "TC" LE */
    0x01,       /* desc_ver */
    0x00,       /* fw major */
    0x01,       /* fw minor */
    0x01,       /* fw patch */
    0x0B,       /* feature_count = 11 */
    0x05,       /* _rsv: bit0-1 central side = 1 (left) -- PLAN phase 9
                 * | bit2 WINDOW_READ (2026-09-05) */
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
    0x0B, 0x01, 0x91, 0x91, /* modules    wire v1, LEFT_STD=ball LEFT_EXT=enc RIGHT_STD=ball RIGHT_EXT=enc */
};

void test_caps_decl2(void) {
    torabo_test_begin("caps descriptor (PLAN phase 9 declared fixture: double config)");

    uint8_t buf[TORABO_CAPS_WIRE_CAP];
    uint16_t len = 0;
    memset(buf, 0xAA, sizeof(buf));

    T_EQ_INT(torabo_caps_encode(buf, sizeof(buf), &len), 0, "encode succeeds");
    T_EQ_INT(len, sizeof(caps_golden_decl2), "encoded length still 52B (bits changed, no rows added)");
    T_EQ_MEM(buf, caps_golden_decl2, sizeof(caps_golden_decl2),
             "52-byte descriptor matches the double-config fixture vector");

    T_EQ_INT((buf[7] & TORABO_CAPS_HDR_CENTRAL_MASK) >> TORABO_CAPS_HDR_CENTRAL_SHIFT,
             TORABO_CAPS_SIDE_LEFT, "_rsv central side decodes to LEFT");
    T_CHECK((buf[7] & TORABO_CAPS_HDR_WINDOW_READ) != 0,
            "_rsv bit2 still declares WINDOW_READ alongside a declared central side");

    uint16_t modules_caps = (uint16_t)buf[50] | ((uint16_t)buf[51] << 8);
    T_EQ_INT(modules_caps, 0x9191, "MODULES caps word == 0x9191 (double config)");

    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_LEFT_STD_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_BALL, "left std slot decodes to BALL");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_LEFT_EXT_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_ENCODER, "left ext slot decodes to ENCODER");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_RIGHT_STD_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_BALL, "right std slot decodes to BALL");
    T_EQ_INT((modules_caps >> TORABO_CAPS_MOD_RIGHT_EXT_SHIFT) & TORABO_CAPS_MOD_SLOT_MASK,
             TORABO_CAPS_SLOT_ENCODER, "right ext slot decodes to ENCODER");

    T_EQ_INT(buf[10], TORABO_CAPS_ZTC_COAST, "trackball caps word carries only COAST, no placement bits");
    T_EQ_INT(buf[26], 0x00, "encoder caps word still 0, no placement bits");
}
