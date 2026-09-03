/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the trackball settings (v2).
 * Same wire blob as the GATT characteristic e1f4a901 (docs/DESIGN_v2.md §4), so
 * the host side codec is shared; only the transport differs.
 *   READ  -> ztc_encode_wire(live snapshot)
 *   WRITE -> ztc_apply_wire() (validates + publishes atomically), then ztc_save()
 * feature_id 0x09 mirrors the low byte of the GATT service UUID (e1f4a900).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_trackball_config/config.h>
#include <torabo_common/tunnel_wrap.h>

LOG_MODULE_DECLARE(ztc_config, CONFIG_ZMK_TRACKBALL_CONFIG_LOG_LEVEL);

#define ZTC_TUNNEL_FEATURE_ID 0x09

/* ztc_apply_wire does ALL validation (len/magic/version/range clamp) and
 * publishes atomically; it changes nothing on rejection. */
TORABO_TUNNEL_APPLY_SAVE_WRITE(ztc_tunnel_write, ztc_apply_wire, ztc_save, "ztc")

TORABO_TUNNEL_FEATURE(trackball, ZTC_TUNNEL_FEATURE_ID, ztc_encode_wire, ztc_tunnel_write);
