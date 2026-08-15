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

#define CB_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4ab00, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define CB_BT_UUID_COMBO BT_UUID_128_ENCODE(0xe1f4ab01, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 cb_svc_uuid = BT_UUID_INIT_128(CB_BT_UUID_SVC);
static struct bt_uuid_128 cb_combo_uuid = BT_UUID_INIT_128(CB_BT_UUID_COMBO);

/* Materialized once; GATT reads are serialized on the BT RX thread, so a single
 * static buffer avoids a >400 B stack allocation in the callback. */
static uint8_t cb_rdbuf[CB_READ_WIRE_LEN];

static ssize_t cb_read_combos(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                              uint16_t len, uint16_t offset) {
    uint16_t wlen = 0;
    if (cb_encode_read_wire(cb_rdbuf, sizeof(cb_rdbuf), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, cb_rdbuf, wlen);
}

static ssize_t cb_write_combo(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                              uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    /* cb_apply_write_wire does ALL validation and stages atomically; it changes
     * nothing on rejection. */
    if (cb_apply_write_wire((const uint8_t *)buf, len) != 0) {
        LOG_WRN("combo GATT write rejected (len=%u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(cb_svc,
    BT_GATT_PRIMARY_SERVICE(&cb_svc_uuid),
    BT_GATT_CHARACTERISTIC(&cb_combo_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           cb_read_combos, cb_write_combo, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_DYNAMIC_COMBOS_BLE */
