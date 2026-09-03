/*
 * Copyright (c) 2026 tak-2025
 * SPDX-License-Identifier: MIT
 *
 * Torabo settings tunnel window for the LED rule table.
 * Same wire blob as the GATT characteristic e1f4ae01 (72 B):
 *   READ  -> led_encode_wire(live snapshot)
 *   WRITE -> led_apply_wire() (validates + publishes), then led_save()
 * feature_id 0x0E mirrors the low byte of the GATT service UUID (e1f4ae00).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/studio/torabo_tunnel.h>
#include <zmk_led_config/config.h>

LOG_MODULE_DECLARE(led_config, CONFIG_ZMK_LED_CONFIG_LOG_LEVEL);

#define LED_TUNNEL_FEATURE_ID 0x0E

static int led_tunnel_write(const uint8_t *buf, uint16_t len) {
    int ret = led_apply_wire(buf, len);
    if (ret != 0) {
        LOG_WRN("led tunnel write rejected (len=%u)", len);
        return ret;
    }

    (void)led_save();
    return 0;
}

TORABO_TUNNEL_FEATURE(led, LED_TUNNEL_FEATURE_ID, led_encode_wire, led_tunnel_write);
