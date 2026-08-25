/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the dynamic combos.
 * Same wire blobs as the GATT characteristic e1f4ab01:
 *   READ  -> cb_encode_read_wire(): ALL slots at once (420 B).
 *   WRITE -> cb_apply_write_wire(): ONE slot, the slot number in the blob header.
 *            It validates, stages and persists that slot itself.
 * feature_id 0x0B mirrors the low byte of the GATT service UUID (e1f4ab00).
 */

#include <zephyr/kernel.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_dynamic_keymap/dcombo.h>

#define CB_TUNNEL_FEATURE_ID 0x0B

TORABO_TUNNEL_FEATURE(combos, CB_TUNNEL_FEATURE_ID, cb_encode_read_wire, cb_apply_write_wire);
