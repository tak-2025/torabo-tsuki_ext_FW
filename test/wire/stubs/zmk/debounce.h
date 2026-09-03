/*
 * Host-test stub for <zmk/debounce.h>.
 * struct zmk_debounce_config copied verbatim from the kscan driver module so the
 * timing store's published debounce buffer has the real layout.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>

#define DEBOUNCE_COUNTER_BITS 14
#define DEBOUNCE_COUNTER_MAX BIT_MASK(DEBOUNCE_COUNTER_BITS)

struct zmk_debounce_state {
    bool pressed : 1;
    bool changed : 1;
    uint16_t counter : DEBOUNCE_COUNTER_BITS;
};

struct zmk_debounce_config {
    uint32_t debounce_press_ms;
    uint32_t debounce_release_ms;
};
