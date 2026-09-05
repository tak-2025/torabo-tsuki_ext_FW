/*
 * BLE GATT window for the live feed — its own service so it cannot disturb the
 * READ/WRITE settings wires already in the field (Torabo-Float/PLAN.md §5, §6).
 *
 * One characteristic, unlike every other ext_FW service:
 *   NOTIFY -> a packed live_feed_evt is pushed on every key/layer change
 *             (built + coalesced in live_feed_central.c; sent by this file).
 *   READ   -> a fresh SNAPSHOT struct (current layer/mask/layout/crc).
 *   CCC    -> subscription state; encrypted, like the other services' perms.
 *
 * The in-tree CCC reference is app/src/studio/gatt_rpc_transport.c, but that is an
 * INDICATE transport (delivery-acked). We only borrow its CCC-attr layout and the
 * on-subscribe conn-latency bump; the push itself is plain NOTIFY (fire-and-forget,
 * recovery is the SNAPSHOT-on-resubscribe — PLAN §6-5).
 *
 * UUIDs (allocated after led e1f4ae00; must match Torabo-Float's live_feed.rs):
 *   service e1f4af00-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   feed    e1f4af01-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   diag    e1f4af02-1c2d-4b6e-9f3a-0a1b2c3d4e5f  (NOTIFY+READ+WRITE, §13 diagnostic mode)
 */

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_LIVE_FEED)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk/ble.h>

#include <zmk_live_feed/live_feed.h>

LOG_MODULE_DECLARE(live_feed, CONFIG_ZMK_LIVE_FEED_LOG_LEVEL);

#define LF_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4af00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define LF_BT_UUID_FEED BT_UUID_128_ENCODE(0xe1f4af01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define LF_BT_UUID_DIAG BT_UUID_128_ENCODE(0xe1f4af02, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 lf_svc_uuid = BT_UUID_INIT_128(LF_BT_UUID_SVC);
static struct bt_uuid_128 lf_feed_uuid = BT_UUID_INIT_128(LF_BT_UUID_FEED);
static struct bt_uuid_128 lf_diag_uuid = BT_UUID_INIT_128(LF_BT_UUID_DIAG);

/* READ returns the current state as a SNAPSHOT. GATT callbacks are serialised on
 * the BT RX thread, so a static scratch struct is safe and re-encoding per read
 * always reflects the live state. */
static ssize_t lf_read_feed(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    static struct live_feed_evt snap;
    live_feed_fill_snapshot(&snap);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &snap, sizeof(snap));
}

#if CONFIG_ZMK_LIVE_FEED_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY

/*
 * IS A STUDIO RPC SESSION SUBSCRIBED ON THIS CONNECTION?
 *
 * THE BUG THIS CLOSES (2026-09-05). The connection's latency has TWO
 * independent owners that both write it through bt_conn_le_param_update(), last
 * writer wins: this CCC callback, and ZMK's own Studio RPC transport
 * (app/src/studio/gatt_rpc_transport.c, which drops to
 * CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY when its CCC is subscribed and
 * restores CONFIG_BT_PERIPHERAL_PREF_LATENCY when it is not).
 *
 * Torabo-Key-App unsubscribes the live feed for the duration of a settings
 * sync, to keep feed notifications from competing with RPC for TX buffers. That
 * unsubscribe used to put the link straight into
 * CONFIG_BT_PERIPHERAL_PREF_LATENCY (30) WHILE a Studio RPC session was
 * mid-flight -- 15 ms x 30 = up to 450 ms of skipped connection events per
 * round trip -- so the getPhysicalLayouts / getKeymap calls that immediately
 * followed stretched past 4 s and hit the app's timeout. The app retries now,
 * but the firmware should not have created the stall in the first place: it was
 * undoing a request the RPC transport had deliberately made, on a connection it
 * does not exclusively own.
 *
 * So: while the client is subscribed to the Studio RPC characteristic, leave
 * the parameter alone. The RPC transport already asked for what it needs and
 * restores the default itself when its own CCC goes away, which is the correct
 * handoff.
 *
 * WHY THE CCC AND NOT THE STUDIO LOCK STATE. zmk_studio_core_get_lock_state()
 * (zmk/studio/core.h) looks like the obvious "is a session up" signal, but it is
 * useless here: the shipped torabo_tsuki_lp conf sets CONFIG_ZMK_STUDIO_LOCKING=n,
 * and core.c then hard-codes the state to UNLOCKED forever, so the test would
 * answer "session up" on an idle keyboard and the feed would never restore the
 * power-saving latency at all. The RPC transport's own subscription flag
 * (handling_rx in gatt_rpc_transport.c) is file-static and reaching it would
 * mean patching the zmk fork.
 *
 * The CCC itself is public, though: bt_gatt_foreach_attr_type() finds the RPC
 * characteristic by UUID in the local attribute table and bt_gatt_is_subscribed()
 * reports the client's subscription -- both plain Zephyr APIs, no ZMK internals.
 * The UUID is the one every Studio client already speaks (zmk
 * app/src/studio/uuid.h), so restating it here couples us to the wire protocol
 * rather than to a header. INDICATE, not NOTIFY: the RPC transport is
 * delivery-acked (BT_GATT_CHRC_INDICATE), unlike this feature's own feed.
 *
 * A build whose Studio has no BLE transport (USB-serial RPC only), or no Studio
 * at all, has no such characteristic to find, so this is false and the latency
 * behaviour is exactly what it has always been.
 */
#if IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_BLE)

/* ZMK_STUDIO_BT_RPC_CHRC_UUID, zmk app/src/studio/uuid.h. */
#define LF_STUDIO_RPC_CHRC_UUID BT_UUID_128_ENCODE(0x00000001, 0x0196, 0x6107, 0xc967, 0xc5cfb1c2482a)

static struct bt_uuid_128 lf_studio_rpc_uuid = BT_UUID_INIT_128(LF_STUDIO_RPC_CHRC_UUID);

static uint8_t lf_take_attr(const struct bt_gatt_attr *attr, uint16_t handle, void *user_data) {
    ARG_UNUSED(handle);
    *(const struct bt_gatt_attr **)user_data = attr;
    return BT_GATT_ITER_STOP;
}

static bool lf_studio_rpc_subscribed(struct bt_conn *conn) {
    const struct bt_gatt_attr *rpc = NULL;

    /* Matching on the characteristic UUID finds the VALUE attribute (the
     * declaration carries BT_UUID_GATT_CHRC instead), which is exactly what
     * bt_gatt_is_subscribed() wants. num_matches=1: there is only ever one. */
    bt_gatt_foreach_attr_type(0x0001, 0xffff, &lf_studio_rpc_uuid.uuid, NULL, 1, lf_take_attr,
                              &rpc);

    return rpc != NULL && bt_gatt_is_subscribed(conn, rpc, BT_GATT_CCC_INDICATE);
}

#else /* no BLE Studio transport in this build */

static inline bool lf_studio_rpc_subscribed(struct bt_conn *conn) {
    ARG_UNUSED(conn);
    return false;
}

#endif

static void lf_request_latency(struct bt_conn *conn, uint8_t latency) {
    int ret = bt_conn_le_param_update(
        conn, BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                               latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
    if (ret < 0) {
        LOG_WRN("live_feed: failed to request latency %u (%d)", latency, ret);
    }
}

#endif /* CONFIG_ZMK_LIVE_FEED_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY */

/* CCC subscribe/unsubscribe. On subscribe we (a) request a lower connection
 * latency so notifications are not batched into a laggy burst (mirrors
 * gatt_rpc_transport.c), and (b) tell the central to push a SNAPSHOT so a fresh
 * subscriber immediately knows the current layer/pressed state.
 *
 * On UNsubscribe we restore the power-saving default -- unless a Studio RPC
 * session is subscribed on this connection, in which case the parameter is not
 * ours to move; see lf_studio_rpc_subscribed() above. */
static void lf_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("live_feed notifications %s", notif_enabled ? "enabled" : "disabled");

#if CONFIG_ZMK_LIVE_FEED_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        if (notif_enabled) {
            lf_request_latency(conn, CONFIG_ZMK_LIVE_FEED_PREF_LATENCY);
        } else if (!lf_studio_rpc_subscribed(conn)) {
            lf_request_latency(conn, CONFIG_BT_PERIPHERAL_PREF_LATENCY);
        } else {
            LOG_DBG("live_feed: studio RPC is subscribed, leaving the connection latency alone");
        }
        bt_conn_unref(conn);
    }
#endif

    live_feed_on_subscribe(notif_enabled);
}

/* Diagnostic char (e1f4af02). READ = every device's current record concatenated
 * (initial sync). WRITE 1 byte = enable/disable the diag heartbeat (the app writes
 * 1 while its panel is open). NOTIFY = one record per push (on-change + heartbeat).
 * Separate from the hot feed so diagnostics never disturb the press visualisation. */
static ssize_t lf_read_diag(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    static uint8_t snap[LIVE_FEED_DIAG_MAX_DEVICES * sizeof(struct live_feed_diag)];
    uint16_t n = live_feed_diag_fill_all(snap, sizeof(snap));
    return bt_gatt_attr_read(conn, attr, buf, len, offset, snap, n);
}

static ssize_t lf_write_diag(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    live_feed_diag_set_stream(((const uint8_t *)buf)[0] != 0);
    return len;
}

static void lf_diag_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    LOG_INF("live_feed diag notifications %s",
            value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(live_feed_svc,
    BT_GATT_PRIMARY_SERVICE(&lf_svc_uuid),
    BT_GATT_CHARACTERISTIC(&lf_feed_uuid.uuid,
                           BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ_ENCRYPT,
                           lf_read_feed, NULL, NULL),
    BT_GATT_CCC(lf_ccc_cfg_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_CHARACTERISTIC(&lf_diag_uuid.uuid,
                           BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           lf_read_diag, lf_write_diag, NULL),
    BT_GATT_CCC(lf_diag_ccc_cfg_changed,
                BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
);
/* clang-format on */

/* Push one event to every subscriber. bt_gatt_notify no-ops (returns -ENOTCONN)
 * when nothing has enabled the CCC, so this costs nothing while unused. The value
 * attribute is attrs[1] (the characteristic declaration), matching the in-tree
 * usage in gatt_rpc_transport.c. Central-side callers drop the error silently. */
int live_feed_gatt_notify(const struct live_feed_evt *evt) {
    return bt_gatt_notify(NULL, &live_feed_svc.attrs[1], evt, sizeof(*evt));
}

/* Diag char value lives right after the feed char (decl) + its value + CCC:
 * attrs[0]=service, [1]=feed decl, [2]=feed value, [3]=feed CCC, [4]=diag decl.
 * Same attrs[decl] convention as the feed notify above. */
int live_feed_diag_notify(const struct live_feed_diag *d) {
    return bt_gatt_notify(NULL, &live_feed_svc.attrs[4], d, sizeof(*d));
}

#endif /* CONFIG_ZMK_LIVE_FEED */
