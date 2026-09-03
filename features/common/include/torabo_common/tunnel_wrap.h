/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Common "apply -> save" WRITE wrapper for a torabo settings tunnel bridge.
 *
 * Every out-of-tree feature's tunnel_bridge.c wraps its own `*_apply_wire`
 * (which validates and publishes atomically, changing nothing on rejection)
 * with the same shape: on failure, LOG_WRN and bail; on success, persist to
 * NVS. This macro is the one place that shape lives, so the per-feature
 * copies (trackball, trackpad, encoder, led, timing) can't drift from each
 * other one field at a time. feature_id / GATT / wire are untouched — this
 * only replaces the hand-written WRITE glue each tunnel_bridge.c registers
 * with TORABO_TUNNEL_FEATURE().
 *
 * Requires the includer to already have <zephyr/logging/log.h> (for LOG_WRN)
 * and a LOG_MODULE_DECLARE/REGISTER in scope — every tunnel_bridge.c has one,
 * for the underlying config module it shares with gatt_service.c.
 */
#pragma once

#include <zephyr/types.h>

/**
 * @brief Define a tunnel WRITE callback that applies a wire blob then saves it.
 *
 * Expands to a `static int fn_name(const uint8_t *buf, uint16_t len)` matching
 * the `write` member of `struct torabo_tunnel_feature`. Use it directly as the
 * write_cb argument to TORABO_TUNNEL_FEATURE(). No trailing `;` after the
 * invocation — it expands to a complete function definition.
 *
 * @param fn_name Name of the generated static function.
 * @param apply_fn `int apply_fn(const uint8_t *buf, uint16_t len)` — validates
 *                 and publishes atomically; changes nothing on rejection.
 * @param save_fn `int save_fn(void)` — persists the now-live settings to NVS.
 *                Called only after a successful apply.
 * @param log_tag String literal naming the feature in the rejection log (e.g.
 *                "ztc", "tp") — concatenated onto the log format at compile
 *                time, so it must match each feature's existing log wording.
 */
#define TORABO_TUNNEL_APPLY_SAVE_WRITE(fn_name, apply_fn, save_fn, log_tag)                       \
    static int fn_name(const uint8_t *buf, uint16_t len) {                                        \
        int ret = apply_fn(buf, len);                                                             \
        if (ret != 0) {                                                                           \
            LOG_WRN(log_tag " tunnel write rejected (len=%u)", len);                              \
            return ret;                                                                           \
        }                                                                                          \
        (void)save_fn();                                                                          \
        return 0;                                                                                 \
    }
