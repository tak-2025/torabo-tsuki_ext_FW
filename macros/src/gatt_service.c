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

#define DM_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4aa00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define DM_BT_UUID_MACRO BT_UUID_128_ENCODE(0xe1f4aa01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 dm_svc_uuid = BT_UUID_INIT_128(DM_BT_UUID_SVC);
static struct bt_uuid_128 dm_macro_uuid = BT_UUID_INIT_128(DM_BT_UUID_MACRO);

/* Materialized once; GATT reads are serialized on the BT RX thread, so a single
 * static buffer avoids a >1.5 KB stack allocation in the callback. */
static uint8_t dm_rdbuf[DM_READ_WIRE_LEN];

static ssize_t dm_read_macros(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    uint16_t wlen = 0;
    if (dm_encode_read_wire(dm_rdbuf, sizeof(dm_rdbuf), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, dm_rdbuf, wlen);
}

static ssize_t dm_write_macro(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                              uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    /* dm_apply_write_wire does ALL validation and publishes atomically; it
     * changes nothing on rejection. */
    if (dm_apply_write_wire((const uint8_t *)buf, len) != 0) {
        LOG_WRN("dmac GATT write rejected (len=%u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(dm_svc,
    BT_GATT_PRIMARY_SERVICE(&dm_svc_uuid),
    BT_GATT_CHARACTERISTIC(&dm_macro_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           dm_read_macros, dm_write_macro, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_DYNAMIC_KEYMAP_BLE */
