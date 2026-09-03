/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the dynamic macros.
 * Same wire blobs as the GATT characteristic e1f4aa01:
 *   READ  -> dm_encode_read_wire(): ALL slots at once (1624 B).
 *   WRITE -> dm_apply_write_wire(): ONE slot, the slot number in the blob header.
 *            It validates, publishes and persists that slot itself.
 * The asymmetry exists because a GATT write had to fit one ATT packet. It is
 * kept here so the app's codec is shared between transports, even though the RPC
 * framing would happily carry the whole table in one message.
 * feature_id 0x0A mirrors the low byte of the GATT service UUID (e1f4aa00).
 */

#include <zephyr/kernel.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_dynamic_keymap/dmac.h>

#define DM_TUNNEL_FEATURE_ID 0x0A

TORABO_TUNNEL_FEATURE(macros, DM_TUNNEL_FEATURE_ID, dm_encode_read_wire, dm_apply_write_wire);
