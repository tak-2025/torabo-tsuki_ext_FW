/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Peripheral half of the debounce settings: take the two windows the central
 * pushes down the split link and hand them to this half's kscan.
 * docs/DESIGN-timing.md §"デバウンスの split 伝搬".
 *
 * WHY THIS IS SO SMALL
 * Everything that makes the numbers — the wire, the validation, the NVS store,
 * the GATT/RPC windows — belongs to the central, which is the only half the app
 * talks to. All that has to exist over here is somewhere to put two bytes and
 * the seam that lets the scan loop read them. Nothing is persisted: the central
 * re-sends on every connect, which is what keeps one source of truth instead of
 * two stores that can disagree after a reflash.
 *
 * WHO CALLS WHAT, AND ON WHICH THREAD
 *   zmk_torabo_debounce_split_apply   the split service work queue, per push
 *   zmk_torabo_debounce_effective     the kscan thread, every scan
 * The reader takes no lock: it reads one atomic index and then a buffer the
 * writer is not touching. There is exactly one writer.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/debounce.h>
#include <zmk_timing_config/config.h>

LOG_MODULE_REGISTER(tmg_split, CONFIG_ZMK_TIMING_SPLIT_PERIPHERAL_LOG_LEVEL);

/* Double-buffered for the same reason as the central store: the kscan thread
 * dereferences the pointer we hand back long after we returned it, so the bytes
 * behind it must never be edited in place. */
static struct zmk_debounce_config dbn[2];
static atomic_t dbn_idx = ATOMIC_INIT(0);

/* Until the first push lands — the first seconds after a boot, and any stretch
 * with no central — this half runs on its devicetree values, exactly as a build
 * without this file would. */
static atomic_t have_values = ATOMIC_INIT(0);

void zmk_torabo_debounce_split_apply(uint8_t press_ms, uint8_t release_ms) {
    /* Clamped again rather than trusted: the central clamps on write, but a
     * zero here would make zmk_debounce_update latch on the first scan. */
    uint32_t press = CLAMP(press_ms, TMG_DEBOUNCE_MIN, TMG_DEBOUNCE_MAX);
    uint32_t release = CLAMP(release_ms, TMG_DEBOUNCE_MIN, TMG_DEBOUNCE_MAX);

    int next = 1 - (int)atomic_get(&dbn_idx);
    dbn[next].debounce_press_ms = press;
    dbn[next].debounce_release_ms = release;
    atomic_set(&dbn_idx, next); /* the writes above complete before the swap */
    atomic_set(&have_values, 1);

    LOG_INF("tmg split debounce: %u/%u ms", press, release);
}

const struct zmk_debounce_config *
zmk_torabo_debounce_effective(const struct zmk_debounce_config *dt) {
    if (!atomic_get(&have_values)) {
        return dt; /* nothing pushed yet: the driver's own devicetree config */
    }
    return &dbn[atomic_get(&dbn_idx)];
}
