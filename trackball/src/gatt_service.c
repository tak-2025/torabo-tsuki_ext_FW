/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * BLE GATT window for the trackball settings (v2). docs/DESIGN_v2.md §11.E.
 * One encrypted characteristic carrying the packed versioned wire (§4).
 *   READ  -> ztc_encode_wire(live snapshot)
 *   WRITE -> ztc_apply_wire(): validates magic+version+length, clamps every
 *            field into a shadow, then publishes via a single atomic swap; only
 *            then persists. Invalid writes are rejected and change nothing.
 * The write path NEVER touches the live store directly, so the lockless input
 * reader can never observe a half-applied or out-of-range config.
 *
 * UUIDs (unchanged from v1 so the app keeps working):
 *   service e1f4a900-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 *   config  e1f4a901-1c2d-4b6e-9f3a-0a1b2c3d4e5f
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk_trackball_config/config.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_TRACKBALL_CONFIG_BLE)

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#define ZTC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4a900, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ZTC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4a901, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 ztc_svc_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_SVC);
static struct bt_uuid_128 ztc_cfg_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_CFG);

/* compile-time upper bound on the wire size for stack buffers */
#define ZTC_WIRE_CAP (8u + (uint32_t)ZTC_MAX_LAYERS * 12u)

static ssize_t ztc_read_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                            uint16_t len, uint16_t offset) {
    uint8_t wire[ZTC_WIRE_CAP];
    uint16_t wlen = 0;
    if (ztc_encode_wire(wire, sizeof(wire), &wlen) != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, wire, wlen);
}

static ssize_t ztc_write_cfg(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    /* ztc_apply_wire does ALL validation (len/magic/version/range clamp) and
     * publishes atomically; it changes nothing on rejection. */
    if (ztc_apply_wire((const uint8_t *)buf, len) != 0) {
        LOG_WRN("ztc GATT write rejected (len=%u)", len);
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    (void)ztc_save();
    return len;
}

/* clang-format off */
BT_GATT_SERVICE_DEFINE(ztc_svc,
    BT_GATT_PRIMARY_SERVICE(&ztc_svc_uuid),
    BT_GATT_CHARACTERISTIC(&ztc_cfg_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                           ztc_read_cfg, ztc_write_cfg, NULL),
);
/* clang-format on */

#endif /* CONFIG_ZMK_TRACKBALL_CONFIG_BLE */
