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

#include <torabo_common/gatt_simple.h>

#define ZTC_BT_UUID_SVC BT_UUID_128_ENCODE(0xe1f4a900, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)
#define ZTC_BT_UUID_CFG BT_UUID_128_ENCODE(0xe1f4a901, 0x1c2d, 0x4b6e, 0x9f3a, 0x0a1b2c3d4e5f)

static struct bt_uuid_128 ztc_svc_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_SVC);
static struct bt_uuid_128 ztc_cfg_uuid = BT_UUID_INIT_128(ZTC_BT_UUID_CFG);

/* The READ/WRITE pair is the shape all five simple settings windows share
 * (torabo_common/gatt_simple.h). ZTC_WIRE_CAP (the v3 length: hdr + layers +
 * coast trailer) comes from config.h so the buffer can never drift behind a wire
 * bump, and the trackball wire is small enough to keep on the RX stack (AUTO),
 * as it always has been. ztc_apply_wire does ALL validation (len/magic/version/
 * range clamp) and publishes atomically; it changes nothing on rejection. */
TORABO_GATT_SIMPLE_HANDLERS(ztc, TORABO_GATT_WIRE_AUTO, ZTC_WIRE_CAP, ztc_encode_wire,
                            ztc_apply_wire, (void)ztc_save(), "ztc")

TORABO_GATT_SIMPLE_SERVICE_DEFINE(ztc_svc, ztc_svc_uuid, ztc_cfg_uuid, ztc_gatt_read,
                                  ztc_gatt_write);

#endif /* CONFIG_ZMK_TRACKBALL_CONFIG_BLE */
