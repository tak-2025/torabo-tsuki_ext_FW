/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for dynamic macros.
 *   READ  -> dm_encode_read_wire(): ALL slots (Read Blob handles >MTU).
 *   WRITE -> dm_apply_write_wire(): ONE slot (<= DM_WRITE_MAX bytes => fits a
 *            single ATT write, so offset!=0 / long-write is rejected).
 *
 * UUIDs (distinct from the trackball service e1f4a9xx):
 *   service e1f4aa00-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   macros  e1f4aa01-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_dynamic_keymap/dmac.h>

LOG_MODULE_DECLARE(dmac_config, CONFIG_ZMK_DYNAMIC_KEYMAP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DYNAMIC_KEYMAP_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <torabo_common/gatt_simple.h>

#define DM_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4aa00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define DM_BT_UUID_MACRO BT_UUID_128_ENCODE(0xe1f4aa01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 dm_svc_uuid = BT_UUID_INIT_128(DM_BT_UUID_SVC);
static struct bt_uuid_128 dm_macro_uuid = BT_UUID_INIT_128(DM_BT_UUID_MACRO);

/* The READ/WRITE pair is the shape all five simple settings windows share
 * (torabo_common/gatt_simple.h). The READ buffer is materialized once and STATIC
 * — GATT reads are serialized on the BT RX thread, so a single shared buffer
 * avoids a >1.5 KB stack allocation in the callback. dm_apply_write_wire does
 * ALL validation, publishes atomically AND persists, so there is no save call:
 * it changes nothing on rejection. */
TORABO_GATT_SIMPLE_HANDLERS(dm, TORABO_GATT_WIRE_STATIC, DM_READ_WIRE_LEN, dm_encode_read_wire,
                            dm_apply_write_wire, TORABO_GATT_NO_SAVE, "dmac")

TORABO_GATT_SIMPLE_SERVICE_DEFINE(dm_svc, dm_svc_uuid, dm_macro_uuid, dm_gatt_read, dm_gatt_write);

#endif /* CONFIG_ZMK_DYNAMIC_KEYMAP_BLE */
