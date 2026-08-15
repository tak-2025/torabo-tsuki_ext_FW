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

/* CCC subscribe/unsubscribe. On subscribe we (a) request a lower connection
 * latency so notifications are not batched into a laggy burst (mirrors
 * gatt_rpc_transport.c), and (b) tell the central to push a SNAPSHOT so a fresh
 * subscriber immediately knows the current layer/pressed state. */
static void lf_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("live_feed notifications %s", notif_enabled ? "enabled" : "disabled");

#if CONFIG_ZMK_LIVE_FEED_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = notif_enabled ? CONFIG_ZMK_LIVE_FEED_PREF_LATENCY
                                        : CONFIG_BT_PERIPHERAL_PREF_LATENCY;
        int ret = bt_conn_le_param_update(
            conn,
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                             latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
        if (ret < 0) {
            LOG_WRN("live_feed: failed to request lower latency (%d)", ret);
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
