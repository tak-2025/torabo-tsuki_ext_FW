/* Host-test stub for <zephyr/kernel.h>. */
#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

/* The wire codecs only ever use k_uptime_get_32() for diagnostics timestamps. */
static inline uint32_t k_uptime_get_32(void) { return 0; }
static inline int64_t k_uptime_get(void) { return 0; }
