/*
 * Torabo-Float live feed — the wire that lets a desktop overlay show which key
 * is being pressed and which layer is active, in real time.
 *
 * WHY THIS EXISTS
 * Neither Studio RPC nor any existing ext_FW settings service can PUSH: RPC has
 * only lock/unsaved notifications, and every e1f4a*00 service is READ/WRITE. So
 * a live visualiser needs a keyboard-originated push — a single NOTIFY
 * characteristic. Only the CENTRAL knows the layer state and the global key
 * position, so the whole feature lives central-side (Torabo-Float/PLAN.md §3, §5).
 *
 * WIRE (Torabo-Float/PLAN.md §5): a small packed, little-endian, versioned event.
 * 16 bytes, well inside one ATT MTU, so a notification never fragments.
 *   proto_ver = 1. The app ignores unknown proto_ver / evt_type for forward compat.
 */

#pragma once

#include <zephyr/types.h>

/* Bumped only on an incompatible wire change (new fields => new proto_ver). */
#define LIVE_FEED_PROTO_VER 1

/* evt_type values. */
#define LIVE_FEED_EVT_KEY 1      /* a key changed state (press/release) */
#define LIVE_FEED_EVT_LAYER 2    /* the active layer set changed */
#define LIVE_FEED_EVT_SNAPSHOT 3 /* full current state, sent on (re)subscribe / READ */

/* position sentinel for non-KEY events. */
#define LIVE_FEED_POSITION_NONE 0xFFFFu

/*
 * The packed event pushed by the NOTIFY characteristic (and returned by READ as a
 * fresh SNAPSHOT). Little-endian, 16 bytes. See PLAN §5 for the field contract:
 *   - highest_layer / layer_mask are id-keyed (NOT index): the app caches by
 *     Layer.id, and reordering makes id != index real here.
 *   - source: 0xFF = central-local (ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL),
 *     otherwise the peripheral slot number.
 *   - position is already in the GLOBAL keymap space (each half globalises it
 *     before the event is raised), so this is independent of which side is central.
 */
struct live_feed_evt {
    uint8_t proto_ver;     /* = LIVE_FEED_PROTO_VER */
    uint8_t evt_type;      /* LIVE_FEED_EVT_* */
    uint16_t position;     /* KEY: global keymap position; else LIVE_FEED_POSITION_NONE */
    uint8_t pressed;       /* KEY: 1=press 0=release; else 0 */
    uint8_t source;        /* KEY: 0xFF=central-local, 0..=peripheral slot; else 0 */
    uint8_t highest_layer; /* layer id of the highest active layer */
    uint8_t active_layout; /* selected physical layout index */
    uint32_t layer_mask;   /* id-keyed active-layer bitmask */
    uint32_t keymap_crc;   /* CRC32 over all (layer_id, binding); cache-staleness check */
} __packed;

BUILD_ASSERT(sizeof(struct live_feed_evt) == 16, "live_feed wire must stay 16 bytes");

/*
 * Fill *out with a fresh SNAPSHOT of the current state (layer id / mask / layout /
 * crc; position=NONE, pressed=0, source=0). Implemented in live_feed_central.c;
 * used by the GATT READ handler and by the on-subscribe snapshot send.
 */
void live_feed_fill_snapshot(struct live_feed_evt *out);

/*
 * Called by the GATT layer when the CCC subscription state changes. On subscribe
 * (enabled=true) the central requests a lower connection latency and schedules a
 * SNAPSHOT push; on unsubscribe it simply stops (bt_gatt_notify no-ops when the
 * CCC is clear). Implemented in live_feed_central.c.
 */
void live_feed_on_subscribe(bool enabled);

/*
 * Push one event to every subscriber (NOTIFY). Implemented in gatt_service.c,
 * called from the central's coalescing work item. Returns the bt_gatt_notify
 * result; the caller drops errors silently (recovery = SNAPSHOT on resubscribe).
 */
int live_feed_gatt_notify(const struct live_feed_evt *evt);

/* ==========================================================================
 * Diagnostic mode (Torabo-Float 診断モード, PLAN-torabo-float.md §13)
 *
 * A SECOND characteristic (e1f4af02) carries per-device liveness so the overlay
 * can act as a live wiring checker: which pointing device init'd, whether events
 * are flowing, and the encoder's cw/ccw/btn counters. It is deliberately a
 * separate char from the hot key/layer feed (af01) so a diagnostic burst never
 * disturbs the press visualisation, and it only streams when a client opts in
 * (WRITE 1 to af02) — a normal Float user pays nothing.
 *
 * The record reuses the same 16-byte envelope; evt_type = DIAG marks it. Old
 * apps ignore an unknown evt_type (§5 forward-compat).
 * ========================================================================== */

#define LIVE_FEED_EVT_DIAG 4 /* a per-device diagnostic record (sent on af02) */

/* status bits (live_feed_diag.status) */
#define LIVE_FEED_DIAG_PRESENT 0x01     /* the device node exists in this build */
#define LIVE_FEED_DIAG_INIT_OK 0x02     /* device_is_ready(): driver init succeeded */
#define LIVE_FEED_DIAG_POWERED 0x04     /* a power-gpios rail is asserted (if any) */
#define LIVE_FEED_DIAG_EVENT_SEEN 0x08  /* >=1 input/sensor event since boot */
#define LIVE_FEED_DIAG_ERR 0x10         /* err_code is latched */
#define LIVE_FEED_DIAG_PERIPHERAL 0x20  /* health is INFERRED (peripheral: central can't probe init) */

/* meta byte: reuse the trackpad device-meta layout (encoder-extender §3-0) so the
 * app can label dynamically. 0 = unknown -> app falls back to "device N".
 *   bits0-1 side (1=left 2=right) | bits2-3 conn (1=std FFC 2=ext FPC) | bits4-5 kind */
#define LIVE_FEED_META_KIND_PAD (1u << 4)  /* trackpad */
#define LIVE_FEED_META_KIND_BALL (2u << 4) /* trackball */
#define LIVE_FEED_META_KIND_ENC (3u << 4)  /* rotary encoder (rot+btn folded into one record) */

/*
 * One device's diagnostic snapshot. LE, 16 bytes (same envelope as live_feed_evt).
 *   - device_id: stable enumerated slot; the app labels from `meta`.
 *   - detail is device-dependent:
 *       encoder rows:        cw | (ccw<<8) | (btn<<16), each a low-byte counter
 *                            that just needs to be seen incrementing (wraps at 256);
 *       split receiver rows  (status has PERIPHERAL set): byte0 = the peripheral
 *                            slot number (DT reg of the zmk,input-split receiver).
 *                            meta is ALWAYS 0 on these rows — never the encoder
 *                            kind, or the app would decode `detail` as cw/ccw/btn
 *                            counters — so the app labels them 相手側デバイス
 *                            （スロットN） from detail byte0;
 *       everything else:     0.
 */
struct live_feed_diag {
    uint8_t proto_ver;     /* = LIVE_FEED_PROTO_VER */
    uint8_t evt_type;      /* = LIVE_FEED_EVT_DIAG */
    uint8_t device_id;     /* stable enumerated slot (0..) */
    uint8_t meta;          /* side/conn/kind; 0 = unknown */
    uint8_t status;        /* LIVE_FEED_DIAG_* */
    uint8_t err_code;      /* |errno| clamped to 255; 0 = none */
    uint16_t event_count;  /* input/sensor events since boot (wraps) */
    uint32_t last_tick_ms; /* k_uptime_get_32() at last event / status change */
    uint32_t detail;       /* encoder: cw|(ccw<<8)|(btn<<16); split rows: slot in byte0; else 0 */
} __packed;

BUILD_ASSERT(sizeof(struct live_feed_diag) == 16, "live_feed diag wire must stay 16 bytes");

/* How many device records the READ (af02) returns at most. Bounds the read buffer. */
#define LIVE_FEED_DIAG_MAX_DEVICES 8

/*
 * Push one diagnostic record to diag subscribers (char e1f4af02). Implemented in
 * gatt_service.c; no-ops when nobody subscribed. Central-side callers drop errors.
 */
int live_feed_diag_notify(const struct live_feed_diag *d);

/*
 * Fill *buf with every known device's current record, concatenated (used by the
 * af02 READ for initial sync). Returns bytes written (<= cap). Implemented in
 * live_feed_central.c.
 */
uint16_t live_feed_diag_fill_all(uint8_t *buf, uint16_t cap);

/*
 * Turn the periodic diag heartbeat on/off. Called by the af02 WRITE handler: the
 * app writes 1 while its diagnostic panel is open, 0 when closed. On-change records
 * are always sent regardless; the heartbeat only adds periodic refreshes so the
 * app can show "N ms ago" freshness and encoder counter deltas. Impl in
 * live_feed_central.c.
 */
void live_feed_diag_set_stream(bool on);
