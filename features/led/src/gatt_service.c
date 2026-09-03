/*
 * BLE settings window for the LED rule table — its own service, so it cannot
 * disturb the trackpad/encoder wires already in the field.
 *
 * 72 B, well inside one ATT MTU: reject offset != 0 and skip Write Long.
 *
 * UUIDs must match zmk-studio's transport table. Allocated so far:
 * trackball e1f4a900, macros e1f4aa00, combos e1f4ab00, trackpad e1f4ac00,
 * encoder e1f4ad00, led e1f4ae00 (this one).
 */

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_ZMK_LED_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include <zmk_led_config/config.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#define LED_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ae00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define LED_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ae01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 led_svc_uuid = BT_UUID_INIT_128(LED_BT_UUID_SVC);
static struct bt_uuid_128 led_cfg_uuid = BT_UUID_INIT_128(LED_BT_UUID_CFG);

static ssize_t led_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    static uint8_t wire[LED_WIRE_CAP]; /* static: GATT cbs are serialised on BT RX */
    uint16_t wlen = 0;
    if (led_encode_wire(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

static ssize_t led_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (led_apply_wire((const uint8_t *)buf, len) != 0) {
        LOG_WRN("led GATT write rejected (len=%u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    (void)led_save();
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(led_svc,
    BT_GATT_PRIMARY_SERVICE(&led_svc_uuid),
    BT_GATT_CHARACTERISTIC(&led_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           led_read_cfg, led_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_LED_CONFIG_BLE */
