/*
 * live_feed — 16-byte envelope layout.
 *
 * PLAN §0.3: Torabo-Float drops every record whose proto_ver != 1, so the
 * envelope can never change shape. The firmware has BUILD_ASSERTs for sizeof;
 * this test additionally pins every FIELD OFFSET, which is what the app's
 * DataView reads actually depend on.
 */

#include <stddef.h>

#include <zmk_live_feed/live_feed.h>

#include "torabo_test.h"

void test_live_feed(void) {
    torabo_test_begin("live_feed 16B envelope");

    T_EQ_INT(sizeof(struct live_feed_evt), 16, "sizeof(live_feed_evt) == 16");
    T_EQ_INT(offsetof(struct live_feed_evt, proto_ver), 0, "evt +0  proto_ver");
    T_EQ_INT(offsetof(struct live_feed_evt, evt_type), 1, "evt +1  evt_type");
    T_EQ_INT(offsetof(struct live_feed_evt, position), 2, "evt +2  position u16");
    T_EQ_INT(offsetof(struct live_feed_evt, pressed), 4, "evt +4  pressed");
    T_EQ_INT(offsetof(struct live_feed_evt, source), 5, "evt +5  source");
    T_EQ_INT(offsetof(struct live_feed_evt, highest_layer), 6, "evt +6  highest_layer");
    T_EQ_INT(offsetof(struct live_feed_evt, active_layout), 7, "evt +7  active_layout");
    T_EQ_INT(offsetof(struct live_feed_evt, layer_mask), 8, "evt +8  layer_mask u32");
    T_EQ_INT(offsetof(struct live_feed_evt, keymap_crc), 12, "evt +12 keymap_crc u32");

    T_EQ_INT(sizeof(struct live_feed_diag), 16, "sizeof(live_feed_diag) == 16");
    T_EQ_INT(offsetof(struct live_feed_diag, proto_ver), 0, "diag +0  proto_ver");
    T_EQ_INT(offsetof(struct live_feed_diag, evt_type), 1, "diag +1  evt_type");
    T_EQ_INT(offsetof(struct live_feed_diag, device_id), 2, "diag +2  device_id");
    T_EQ_INT(offsetof(struct live_feed_diag, meta), 3, "diag +3  meta");
    T_EQ_INT(offsetof(struct live_feed_diag, status), 4, "diag +4  status");
    T_EQ_INT(offsetof(struct live_feed_diag, err_code), 5, "diag +5  err_code");
    T_EQ_INT(offsetof(struct live_feed_diag, event_count), 6, "diag +6  event_count u16");
    T_EQ_INT(offsetof(struct live_feed_diag, last_tick_ms), 8, "diag +8  last_tick_ms u32");
    T_EQ_INT(offsetof(struct live_feed_diag, detail), 12, "diag +12 detail u32");

    /* evt_type values are matched by the app; DIAG shares the envelope. */
    T_EQ_INT(LIVE_FEED_EVT_KEY, 1, "evt_type KEY = 1");
    T_EQ_INT(LIVE_FEED_EVT_LAYER, 2, "evt_type LAYER = 2");
    T_EQ_INT(LIVE_FEED_EVT_SNAPSHOT, 3, "evt_type SNAPSHOT = 3");
    T_EQ_INT(LIVE_FEED_EVT_DIAG, 4, "evt_type DIAG = 4");
    T_EQ_INT(LIVE_FEED_POSITION_NONE, 0xFFFF, "position sentinel 0xFFFF");

    /* The two envelopes must stay interchangeable on the wire: one stream, told
     * apart by evt_type only. */
    T_EQ_INT(sizeof(struct live_feed_evt), sizeof(struct live_feed_diag),
             "both envelopes are the same size");

    /* Serialising an evt through the packed struct must produce the documented
     * little-endian byte order. */
    struct live_feed_evt e = {
        .proto_ver = LIVE_FEED_PROTO_VER,
        .evt_type = LIVE_FEED_EVT_KEY,
        .position = 0x0123,
        .pressed = 1,
        .source = 0xFF,
        .highest_layer = 3,
        .active_layout = 1,
        .layer_mask = 0x89ABCDEFu,
        .keymap_crc = 0x11223344u,
    };
    static const uint8_t want[16] = {
        0x01, 0x01, 0x23, 0x01, 0x01, 0xFF, 0x03, 0x01,
        0xEF, 0xCD, 0xAB, 0x89, 0x44, 0x33, 0x22, 0x11,
    };
    T_EQ_MEM(&e, want, sizeof(want), "packed evt serialises little-endian");
}
