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
#include <torabo_common/tunnel_wrap.h>

LOG_MODULE_DECLARE(enc_config, CONFIG_ZMK_ENCODER_CONFIG_LOG_LEVEL);

#define ENC_TUNNEL_FEATURE_ID 0x0D

TORABO_TUNNEL_APPLY_SAVE_WRITE(enc_tunnel_write, enc_apply_wire, enc_save, "enc")

TORABO_TUNNEL_FEATURE(encoder, ENC_TUNNEL_FEATURE_ID, enc_encode_wire, enc_tunnel_write);
