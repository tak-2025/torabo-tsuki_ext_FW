/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for dynamic combos.
 *   READ  -> cb_encode_read_wire(): ALL combo slots (Read Blob handles >MTU).
 *   WRITE -> cb_apply_write_wire(): ONE slot (<= CB_WRITE_MAX bytes => fits a
 *            single ATT write, so offset!=0 / long-write is rejected).
 *
 * UUIDs (distinct from trackball e1f4a9xx and macros e1f4aaxx):
 *   service e1f4ab00-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   combos  e1f4ab01-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_dynamic_keymap/dcombo.h>

LOG_MODULE_DECLARE(dcombo_config, CONFIG_ZMK_DYNAMIC_COMBOS_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DYNAMIC_COMBOS_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <torabo_common/gatt_simple.h>

#define CB_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ab00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define CB_BT_UUID_COMBO BT_UUID_128_ENCODE(0xe1f4ab01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 cb_svc_uuid = BT_UUID_INIT_128(CB_BT_UUID_SVC);
static struct bt_uuid_128 cb_combo_uuid = BT_UUID_INIT_128(CB_BT_UUID_COMBO);

/* The READ/WRITE pair is the shape the simple settings windows share
 * (torabo_common/gatt_simple.h). The READ buffer is materialized once and STATIC
 * — GATT reads are serialized on the BT RX thread, so a single shared buffer
 * avoids a >400 B stack allocation in the callback. cb_apply_write_wire does ALL
 * validation, stages atomically AND persists, so there is no save call: it
 * changes nothing on rejection. */
TORABO_GATT_SIMPLE_HANDLERS(cb, TORABO_GATT_WIRE_STATIC, CB_READ_WIRE_LEN, cb_encode_read_wire,
                            cb_apply_write_wire, TORABO_GATT_NO_SAVE, "combo")

TORABO_GATT_SIMPLE_SERVICE_DEFINE(cb_svc, cb_svc_uuid, cb_combo_uuid, cb_gatt_read, cb_gatt_write);

#endif /* CONFIG_ZMK_DYNAMIC_COMBOS_BLE */
