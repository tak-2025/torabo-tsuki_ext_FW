/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the live feed — the one feature that PUSHES,
 * so it is also the one that uses the tunnel's SUBSCRIBE ops.
 *
 * Same 16-byte records as the GATT service e1f4af00, unchanged, so the app's
 * decoder is shared between transports:
 *   READ        -> a fresh SNAPSHOT followed by every device's diagnostic record
 *   WRITE       -> one byte, the diagnostic heartbeat switch (what af02's WRITE takes)
 *   SUBSCRIBE   -> start pushing; a SNAPSHOT is sent straight away, exactly like
 *                  enabling the af01 CCC
 *   UNSUBSCRIBE -> stop
 * feature_id 0x0F mirrors the low byte of the GATT service UUID (e1f4af00).
 *
 * ONE feature id covers what GATT splits over two characteristics. Over GATT the
 * split is necessary: a burst of diagnostics on the hot feed would disturb the
 * press visualisation. Over RPC every message is serialised through one transport
 * anyway, so the split would buy nothing and cost an id.
 *
 * That means both windows concatenate the two record types and let evt_type
 * separate them, which is what the app already does with the GATT records:
 *   - READ returns af01's SNAPSHOT first, then af02's whole table. A caller
 *     wanting the feed takes the first non-DIAG record; one wanting diagnostics
 *     takes every DIAG record.
 *   - the notification stream carries one record per notification, from both
 *     sources, tagged the same way.
 * Deliberately NOT a selector byte in the request: the wire blobs then stop being
 * byte-identical to the GATT ones, which is the whole point of the tunnel.
 *
 * Pushes here are droppable, which is the tunnel's contract and already how the
 * BLE side behaves (§6-5: fire-and-forget, no retry). A client that misses a
 * record recovers the same way it starts up — the READ above, or the SNAPSHOT it
 * gets on subscribing — because every record carries the full layer state, not a
 * delta. Only a key press/release pair can genuinely be lost, and the next event
 * of any kind corrects the display.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_live_feed/live_feed.h>

#define LF_TUNNEL_FEATURE_ID 0x0F

static int lf_tunnel_read(uint8_t *buf, uint16_t cap, uint16_t *out_len) {
    struct live_feed_evt snap;

    /* The SNAPSHOT is the part a plain Float needs, so it must always fit; the
     * diagnostic table that follows is best-effort and fill_all stops early on a
     * short buffer. 16 + 8*16 = 144 B at most. */
    if (cap < sizeof(snap)) {
        return -ENOMEM;
    }

    live_feed_fill_snapshot(&snap);
    memcpy(buf, &snap, sizeof(snap));
    uint16_t len = (uint16_t)sizeof(snap);

    len += live_feed_diag_fill_all(buf + len, (uint16_t)(cap - len));

    if (out_len) {
        *out_len = len;
    }
    return 0;
}

/* Byte-identical to the af02 WRITE handler: one byte, non-zero starts the
 * diagnostic heartbeat. Kept in step with lf_write_diag in gatt_service.c. */
static int lf_tunnel_write(const uint8_t *buf, uint16_t len) {
    if (len < 1) {
        return -EINVAL;
    }

    live_feed_diag_set_stream(buf[0] != 0);
    return 0;
}

static int lf_tunnel_subscribe(bool enabled) {
    /* Same handler the af01 CCC calls: schedules a SNAPSHOT on subscribe, and
     * nothing on unsubscribe (the push side checks the subscription itself). The
     * connection-latency bump next to that CCC is deliberately not mirrored — it
     * is a BLE knob, and it stays owned by the BLE path. */
    live_feed_on_subscribe(enabled);
    return 0;
}

int live_feed_tunnel_notify(const void *rec) {
    return torabo_tunnel_notify(LF_TUNNEL_FEATURE_ID, (const uint8_t *)rec,
                                (uint16_t)LIVE_FEED_RECORD_SIZE);
}

TORABO_TUNNEL_FEATURE_SUB(live_feed, LF_TUNNEL_FEATURE_ID, lf_tunnel_read, lf_tunnel_write,
                          lf_tunnel_subscribe);
