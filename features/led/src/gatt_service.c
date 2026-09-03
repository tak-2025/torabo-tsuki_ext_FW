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
#include <torabo_common/gatt_simple.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#define LED_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ae00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define LED_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ae01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 led_svc_uuid = BT_UUID_INIT_128(LED_BT_UUID_SVC);
static struct bt_uuid_128 led_cfg_uuid = BT_UUID_INIT_128(LED_BT_UUID_CFG);

/* The READ/WRITE pair is the shape all five simple settings windows share
 * (torabo_common/gatt_simple.h). STATIC buffer: GATT cbs are serialised on BT RX. */
TORABO_GATT_SIMPLE_HANDLERS(led, TORABO_GATT_WIRE_STATIC, LED_WIRE_CAP, led_encode_wire,
                            led_apply_wire, (void)led_save(), "led")

TORABO_GATT_SIMPLE_SERVICE_DEFINE(led_svc, led_svc_uuid, led_cfg_uuid, led_gatt_read,
                                  led_gatt_write);

#endif /* CONFIG_ZMK_LED_CONFIG_BLE */
