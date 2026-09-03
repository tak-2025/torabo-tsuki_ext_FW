/*
 * Host-test stub for <zmk/torabo_timing.h>.
 *
 * This header lives in the ZMK FORK, not in this repository, so the host build
 * needs a copy. struct zmk_torabo_ht_params and the flag/slot constants are
 * reproduced VERBATIM from the fork
 * (zmk/app/include/zmk/torabo_timing.h) — if the fork ever changes them, the
 * timing wire test is the thing that should start failing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

#define ZMK_TORABO_HT_MAX_POSITIONS 32

#define ZMK_TORABO_HT_FLAG_RETRO_TAP 0x01
#define ZMK_TORABO_HT_FLAG_HOLD_TRIGGER_ON_RELEASE 0x02
#define ZMK_TORABO_HT_FLAG_HOLD_WHILE_UNDECIDED 0x04

struct zmk_torabo_ht_params {
    uint16_t tapping_term_ms;
    int32_t quick_tap_ms;
    int32_t require_prior_idle_ms;
    uint8_t flavor;
    uint8_t flags;
    uint8_t pos_count;
    uint8_t positions[ZMK_TORABO_HT_MAX_POSITIONS];
};

bool zmk_torabo_ht_override(const struct device *dev, struct zmk_torabo_ht_params *out);
void zmk_torabo_ht_report_dt(const char *dev_name, const struct zmk_torabo_ht_params *dt);

struct zmk_debounce_config;

const struct zmk_debounce_config *
zmk_torabo_debounce_effective(const struct zmk_debounce_config *dt);

bool zmk_torabo_debounce_split_values(uint8_t *press_ms, uint8_t *release_ms);
void zmk_torabo_debounce_split_push(void);
void zmk_torabo_debounce_split_apply(uint8_t press_ms, uint8_t release_ms);
