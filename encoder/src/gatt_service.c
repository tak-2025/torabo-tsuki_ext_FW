/*
 * BLE settings window for the encoder store — its own service, deliberately not
 * sharing the trackpad's, so a bug here cannot disturb a wire that is already in
 * the field.
 *
 * The whole config is ~124 B, well inside one ATT MTU, so we reject offset != 0
 * and skip Write Long entirely (same as trackball/macros/combos).
 *
 * UUIDs must match zmk-studio's transport table (src-tauri .../transport/).
 * Allocated so far: trackball e1f4a900, macros e1f4aa00, combos e1f4ab00,
 * trackpad e1f4ac00, encoder e1f4ad00 (this one).
 */

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_ENCODER_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk_encoder_config/config.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#define ENC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ad00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ENC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ad01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 enc_svc_uuid = BT_UUID_INIT_128(ENC_BT_UUID_SVC);
static struct bt_uuid_128 enc_cfg_uuid = BT_UUID_INIT_128(ENC_BT_UUID_CFG);

static ssize_t enc_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    /* static: GATT callbacks are serialised on the BT RX thread */
    static uint8_t wire[ENC_WIRE_CAP];
    uint16_t wlen = 0;
    if (enc_encode_wire(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    /* handles Read Blob / offset for us */
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

static ssize_t enc_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        /* the config fits in one MTU; a fragmented write means something is wrong */
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (enc_apply_wire((const uint8_t *)buf, len) != 0) {
        LOG_WRN("enc GATT write rejected (len=%u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    (void)enc_save();
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(enc_svc,
    BT_GATT_PRIMARY_SERVICE(&enc_svc_uuid),
    BT_GATT_CHARACTERISTIC(&enc_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           enc_read_cfg, enc_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_ENCODER_CONFIG_BLE */
