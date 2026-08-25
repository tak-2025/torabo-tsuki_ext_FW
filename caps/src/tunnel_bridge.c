/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the capability descriptor.
 * Same wire blob as the GATT characteristic e1f4a001, and read-only for the same
 * reason: it describes the build, so there is nothing for the app to write.
 * feature_id 0x00 mirrors the low byte of the GATT service UUID (e1f4a000).
 *
 * This is the entry point of the whole USB story: a USB-connected app reads the
 * descriptor through the tunnel, sees the RpcTunnel feature, and knows every
 * other tab can be served over the same transport.
 */

#include <zephyr/kernel.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_torabo_caps/caps.h>

#define CAPS_TUNNEL_FEATURE_ID 0x00

TORABO_TUNNEL_FEATURE(caps, CAPS_TUNNEL_FEATURE_ID, torabo_caps_encode, NULL);
