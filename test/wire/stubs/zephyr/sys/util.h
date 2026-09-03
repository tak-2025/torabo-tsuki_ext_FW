/* Host-test stub for <zephyr/sys/util.h>. Only what the wire codecs use. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* IS_ENABLED: the upstream Zephyr trick, so `#if IS_ENABLED(CONFIG_X)` is 0 for
 * an undefined CONFIG_X and 1 only when it is defined to exactly 1. */
#define Z_IS_ENABLED_XXXX1 Z_IS_ENABLED_YYYY,
#define Z_IS_ENABLED3(ignore_this, val, ...) val
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(Z_IS_ENABLED_XXXX##config_macro)
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)

#ifndef BUILD_ASSERT
#define BUILD_ASSERT(EXPR, ...) _Static_assert(EXPR, "" __VA_ARGS__)
#endif

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef CLAMP
#define CLAMP(val, low, high) (((val) <= (low)) ? (low) : MIN(val, high))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
#ifndef BIT_MASK
#define BIT_MASK(n) (BIT(n) - 1UL)
#endif

#ifndef ARG_UNUSED
#define ARG_UNUSED(x) ((void)(x))
#endif

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __weak
#define __weak __attribute__((__weak__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
