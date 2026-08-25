/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the rotary encoder settings.
 * Same wire blob as the GATT characteristic e1f4ad01:
 *   READ  -> enc_encode_wire(live snapshot)
 *   WRITE -> enc_apply_wire() (validates + publishes), then enc_save()
 * feature_id 0x0D mirrors the low byte of the GATT service UUID (e1f4ad00).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_encoder_config/config.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#define ENC_TUNNEL_FEATURE_ID 0x0D

static int enc_tunnel_write(const uint8_t *buf, uint16_t len) {
    int ret = enc_apply_wire(buf, len);
    if (ret != 0) {
        LOG_WRN("enc tunnel write rejected (len=%u)", len);
        return ret;
    }

    (void)enc_save();
    return 0;
}

TORABO_TUNNEL_FEATURE(encoder, ENC_TUNNEL_FEATURE_ID, enc_encode_wire, enc_tunnel_write);
