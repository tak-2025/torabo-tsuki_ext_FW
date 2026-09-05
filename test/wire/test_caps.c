/*
 * caps descriptor — pinned byte vector.
 *
 * PLAN §0.2: the descriptor is the first thing every client reads, and its
 * feature ids are append-only. This test pins the EXACT 52 bytes the current
 * 11-feature build emits, for the Kconfig set stated in torabo_test_config.h.
 *
 * If this fails, either a feature row moved / changed wire_ver (which must be a
 * deliberate, app-coordinated change) or the descriptor layout drifted.
 *
 * PLAN phase 9 (re-redesigned 2026-09-03, the SECOND redesign of this phase)
 * adds one new row: TORABO_FEAT_MODULES (id 11), always appended last whenever
 * CONFIG_TORABO_CAPS is on. Its caps word packs four 4-bit slot values (left
 * std/ext, right std/ext -- see caps.h) from CONFIG_TORABO_SLOT_LEFT_STD /
 * _LEFT_EXT / _RIGHT_STD / _RIGHT_EXT, plus the header `_rsv` byte from
 * CONFIG_TORABO_CENTRAL_SIDE (unchanged since the ORIGINAL phase 9 commit).
 * torabo_test_config.h defines all five as 0 (their real Kconfig default), so
 * this vector's _rsv is 0x00 and the new MODULES row's caps word is 0x0000 --
 * every one of the OTHER 10 rows is BYTE-IDENTICAL to the pre-phase-9 48B
 * vector (this phase's two earlier designs put placement bits inside the
 * trackball/encoder rows themselves; that is gone -- placement now lives
 * ONLY in the new row, so those two rows never change with it again). The
 * declared (non-zero) case lives in the separate test_caps_decl.c /
 * test_caps_decl2.c binaries (see run-tests.sh), never in this one.
 *
 * PLAN phase 8 (dm v2 macro names) bumped the macros row's wire_ver 1->2
 * (offset 13, the row's 2nd byte). This is the ONLY byte the ORIGINAL 48B of
 * this vector changes for phase 8: the row's caps word (offset 14-15) stays
 * 0, feature_count and every other row of the original 10 are untouched --
 * the name block only extends the macros READ wire itself, which this
 * descriptor never carries.
 */

#include <errno.h>
#include <string.h>

#include <zmk_torabo_caps/caps.h>

#include "torabo_test.h"

/* hdr: magic_lo magic_hi desc_ver major minor patch feature_count rsv
 * row : id wire_ver caps_lo caps_hi */
static const uint8_t caps_golden[52] = {
    /* header */
    0x54, 0x43, /* magic 0x4354 "TC" LE */
    0x01,       /* desc_ver */
    0x00,       /* fw major */
    0x01,       /* fw minor */
    0x01,       /* fw patch */
    0x0B,       /* feature_count = 11 */
    0x00,       /* rsv */
    /* rows, in build_features() order */
    0x01, 0x03, 0x01, 0x00, /* trackball  wire v3, ZTC_COAST */
    0x02, 0x02, 0x00, 0x00, /* macros     wire v2 (PLAN phase 8: dm name block) */
    0x03, 0x01, 0x00, 0x00, /* combos     wire v1 */
    0x04, 0x03, 0x10, 0x00, /* trackpad   wire v3, TP_COAST */
    0x05, 0x01, 0x00, 0x00, /* encoder    wire v1 */
    0x06, 0x01, 0x03, 0x00, /* led        wire v1, LEFT|RIGHT */
    0x07, 0x01, 0x04, 0x00, /* layers     wire v1, 4 reserved */
    0x08, 0x01, 0x01, 0x00, /* live_feed  wire v1, DIAG */
    0x09, 0x01, 0x01, 0x00, /* rpc_tunnel wire v1, NOTIFY */
    0x0A, 0x01, 0x01, 0x00, /* timing     wire v1, SPLIT_DEBOUNCE */
    0x0B, 0x01, 0x00, 0x00, /* modules    wire v1, all 4 slots undeclared (PLAN phase 9) */
};

void test_caps(void) {
    torabo_test_begin("caps descriptor");

    uint8_t buf[TORABO_CAPS_WIRE_CAP];
    uint16_t len = 0;
    memset(buf, 0xAA, sizeof(buf));

    T_EQ_INT(torabo_caps_encode(buf, sizeof(buf), &len), 0, "encode succeeds");
    T_EQ_INT(len, sizeof(caps_golden), "encoded length 52B (single ATT read)");
    T_EQ_MEM(buf, caps_golden, sizeof(caps_golden), "52-byte descriptor is byte-identical");

    /* The 10 rows that existed before PLAN phase 9's MODULES row was appended
     * must not have moved or changed by so much as one byte -- the new row is
     * purely additive. Checked explicitly (not just implied by the full-buffer
     * compare above) so a future edit that shifts these rows fails loudly here. */
    static const uint8_t pre_phase9_rows[46] = {
        0x54, 0x43, 0x01, 0x00, 0x01, 0x01, /* magic, desc_ver, fw major/minor/patch
                                                (feature_count and _rsv excluded: both
                                                changed by this phase) */
        0x01, 0x03, 0x01, 0x00, /* trackball */
        0x02, 0x02, 0x00, 0x00, /* macros */
        0x03, 0x01, 0x00, 0x00, /* combos */
        0x04, 0x03, 0x10, 0x00, /* trackpad */
        0x05, 0x01, 0x00, 0x00, /* encoder */
        0x06, 0x01, 0x03, 0x00, /* led */
        0x07, 0x01, 0x04, 0x00, /* layers */
        0x08, 0x01, 0x01, 0x00, /* live_feed */
        0x09, 0x01, 0x01, 0x00, /* rpc_tunnel */
        0x0A, 0x01, 0x01, 0x00, /* timing */
    };
    T_EQ_MEM(buf, pre_phase9_rows, 6, "header magic/desc_ver/fw-version untouched by MODULES row");
    T_EQ_MEM(&buf[8], &pre_phase9_rows[6], sizeof(pre_phase9_rows) - 6,
              "all 10 pre-phase-9 feature rows are byte-identical to before the MODULES row");

    /* The descriptor must still fit the compile-time cap it declares. PLAN
     * phase 6 (B-4) raised the cap 10->16; phase 9 raises it again 16->32 for
     * headroom (a bigger static buffer only -- desc_ver stays 1, and the wire
     * itself is count-driven so it is unaffected either way: still 52B for
     * this 11-feature build). */
    T_EQ_INT(TORABO_CAPS_MAX_FEATURES, 32, "MAX_FEATURES raised to 32 (PLAN phase 9)");
    T_EQ_INT(TORABO_CAPS_WIRE_CAP, 8 + 32 * 4, "wire cap = hdr + MAX_FEATURES rows");
    T_CHECK(len <= TORABO_CAPS_WIRE_CAP, "encoded length within declared cap");

    /* PLAN §0.2 landmine (closed in phase 2 A-5): build_features() guards
     * against overrunning this array (drops + LOG_ERR past the limit). The
     * table sits at 11/32 today (raised from 10/16 by PLAN phase 9's new
     * MODULES row plus its own cap bump), with 21 slots free for future
     * features. Assert the count explicitly so a future feature is a visible,
     * deliberate choice rather than a silently dropped feature row. */
    T_EQ_INT(buf[6], 11,
             "feature_count == 11 (11 of 32 TORABO_CAPS_MAX_FEATURES slots used - see "
             "PLAN phase 9; overflow guard itself is PLAN phase 2 A-5)");

    /* Too-small buffer must be refused, not truncated. */
    uint8_t small[51];
    uint16_t slen = 0xFFFF;
    T_EQ_INT(torabo_caps_encode(small, sizeof(small), &slen), -ENOMEM,
             "encode into a 51B buffer is rejected");

    T_EQ_INT(torabo_caps_encode(NULL, 64, &slen), -ENOMEM, "NULL buffer is rejected");
}
