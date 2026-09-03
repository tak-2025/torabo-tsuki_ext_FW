/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the trackpad settings (v2).
 * Same wire blob as the GATT characteristic e1f4ac01 (DESIGN-trackpad-v2.md §3):
 *   READ  -> tp_encode_wire(live snapshot)
 *   WRITE -> tp_apply_wire() (validates + publishes atomically), then tp_save()
 * feature_id 0x0C mirrors the low byte of the GATT service UUID (e1f4ac00).
 *
 * None of gatt_service.c's wire reassembly is needed here. That machinery exists
 * because a v2 wire can outgrow one ATT MTU and has to be rebuilt from chunks
 * that all arrive at offset 0; the RPC framing already delivers a whole message,
 * so the blob handed to us is always complete.
 *
 * The wire does outgrow the tunnel's blob budget in the extreme: a 4-device,
 * 20-layer build encodes 3054 bytes. tp_encode_wire refuses a too-small buffer
 * rather than truncating, so the tunnel answers ERROR; raise
 * CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE for such a build.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_trackpad_config/config.h>

LOG_MODULE_DECLARE(tp_config, CONFIG_ZMK_TRACKPAD_CONFIG_LOG_LEVEL);

#define TP_TUNNEL_FEATURE_ID 0x0C

static int tp_tunnel_write(const uint8_t *buf, uint16_t len) {
    /* tp_apply_wire does ALL validation (magic/version/length/clamp) and
     * publishes atomically; it changes nothing on rejection. */
    int ret = tp_apply_wire(buf, len);
    if (ret != 0) {
        LOG_WRN("tp tunnel write rejected (len=%u)", len);
        return ret;
    }

    (void)tp_save();
    return 0;
}

TORABO_TUNNEL_FEATURE(trackpad, TP_TUNNEL_FEATURE_ID, tp_encode_wire, tp_tunnel_write);
