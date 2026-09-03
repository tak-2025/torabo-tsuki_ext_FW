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
#include <torabo_common/gatt_simple.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#define ENC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ad00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ENC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4ad01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 enc_svc_uuid = BT_UUID_INIT_128(ENC_BT_UUID_SVC);
static struct bt_uuid_128 enc_cfg_uuid = BT_UUID_INIT_128(ENC_BT_UUID_CFG);

/* The READ/WRITE pair is the shape all five simple settings windows share
 * (torabo_common/gatt_simple.h). STATIC buffer: GATT callbacks are serialised on
 * the BT RX thread. */
TORABO_GATT_SIMPLE_HANDLERS(enc, TORABO_GATT_WIRE_STATIC, ENC_WIRE_CAP, enc_encode_wire,
                            enc_apply_wire, (void)enc_save(), "enc")

TORABO_GATT_SIMPLE_SERVICE_DEFINE(enc_svc, enc_svc_uuid, enc_cfg_uuid, enc_gatt_read,
                                  enc_gatt_write);

#endif /* CONFIG_ZMK_ENCODER_CONFIG_BLE */
