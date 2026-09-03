/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the timing settings.
 * Same 96-byte wire blob as the GATT characteristic e1f4b001 (DESIGN-timing.md):
 *   READ  -> tmg_encode_wire(live snapshot)
 *   WRITE -> tmg_apply_wire() (validates + publishes atomically), then tmg_save()
 * feature_id 0x10 mirrors the low byte of the GATT service UUID (e1f4b000).
 *
 * gatt_service.c's reassembler has no counterpart here: RPC framing delivers a
 * whole message, so the blob handed to us is always complete. 96 bytes is also
 * far inside CONFIG_ZMK_STUDIO_TORABO_TUNNEL_BLOB_MAX_SIZE, so unlike the
 * trackpad wire this one can never outgrow the tunnel's budget.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_timing_config/config.h>
#include <torabo_common/tunnel_wrap.h>

LOG_MODULE_DECLARE(tmg_config, CONFIG_ZMK_TIMING_CONFIG_LOG_LEVEL);

#define TMG_TUNNEL_FEATURE_ID 0x10

/* tmg_apply_wire does ALL validation (version/shape/length/clamp) and
 * publishes atomically; it changes nothing on rejection. */
TORABO_TUNNEL_APPLY_SAVE_WRITE(tmg_tunnel_write, tmg_apply_wire, tmg_save, "tmg")

TORABO_TUNNEL_FEATURE(timing, TMG_TUNNEL_FEATURE_ID, tmg_encode_wire, tmg_tunnel_write);
