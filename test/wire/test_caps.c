/*
 * caps descriptor — pinned byte vector.
 *
 * PLAN §0.2: the descriptor is the first thing every client reads, and its
 * feature ids are append-only. This test pins the EXACT 48 bytes the current
 * 10-feature build emits, for the Kconfig set stated in torabo_test_config.h.
 *
 * If this fails, either a feature row moved / changed wire_ver (which must be a
 * deliberate, app-coordinated change) or the descriptor layout drifted.
 */

#include <errno.h>
#include <string.h>

#include <zmk_torabo_caps/caps.h>

#include "torabo_test.h"

/* hdr: magic_lo magic_hi desc_ver major minor patch feature_count rsv
 * row : id wire_ver caps_lo caps_hi */
static const uint8_t caps_golden[48] = {
    /* header */
    0x54, 0x43, /* magic 0x4354 "TC" LE */
    0x01,       /* desc_ver */
    0x00,       /* fw major */
    0x01,       /* fw minor */
    0x00,       /* fw patch */
    0x0A,       /* feature_count = 10 */
    0x00,       /* rsv */
    /* rows, in build_features() order */
    0x01, 0x03, 0x01, 0x00, /* trackball  wire v3, ZTC_COAST */
    0x02, 0x01, 0x00, 0x00, /* macros     wire v1 */
    0x03, 0x01, 0x00, 0x00, /* combos     wire v1 */
    0x04, 0x03, 0x10, 0x00, /* trackpad   wire v3, TP_COAST */
    0x05, 0x01, 0x00, 0x00, /* encoder    wire v1 */
    0x06, 0x01, 0x03, 0x00, /* led        wire v1, LEFT|RIGHT */
    0x07, 0x01, 0x04, 0x00, /* layers     wire v1, 4 reserved */
    0x08, 0x01, 0x01, 0x00, /* live_feed  wire v1, DIAG */
    0x09, 0x01, 0x01, 0x00, /* rpc_tunnel wire v1, NOTIFY */
    0x0A, 0x01, 0x01, 0x00, /* timing     wire v1, SPLIT_DEBOUNCE */
};

void test_caps(void) {
    torabo_test_begin("caps descriptor");

    uint8_t buf[TORABO_CAPS_WIRE_CAP];
    uint16_t len = 0;
    memset(buf, 0xAA, sizeof(buf));

    T_EQ_INT(torabo_caps_encode(buf, sizeof(buf), &len), 0, "encode succeeds");
    T_EQ_INT(len, sizeof(caps_golden), "encoded length 48B (single ATT read)");
    T_EQ_MEM(buf, caps_golden, sizeof(caps_golden), "48-byte descriptor is byte-identical");

    /* The descriptor must still fit the compile-time cap it declares. */
    T_EQ_INT(TORABO_CAPS_WIRE_CAP, 8 + 10 * 4, "wire cap = hdr + MAX_FEATURES rows");
    T_CHECK(len <= TORABO_CAPS_WIRE_CAP, "encoded length within declared cap");

    /* PLAN §0.2 landmine: build_features() has no bound check and the table is
     * exactly full today. Assert the fullness explicitly so an 11th feature is a
     * failing test rather than a stack smash on real hardware. */
    T_EQ_INT(buf[6], TORABO_CAPS_MAX_FEATURES,
             "feature_count == TORABO_CAPS_MAX_FEATURES (table is FULL: adding an "
             "11th feature overflows build_features()'s array - see PLAN phase 2 A-5)");

    /* Too-small buffer must be refused, not truncated. */
    uint8_t small[47];
    uint16_t slen = 0xFFFF;
    T_EQ_INT(torabo_caps_encode(small, sizeof(small), &slen), -ENOMEM,
             "encode into a 47B buffer is rejected");

    T_EQ_INT(torabo_caps_encode(NULL, 64, &slen), -ENOMEM, "NULL buffer is rejected");
}
