/* Host-test stub for <zephyr/device.h>. Only `name` is ever read by the codecs. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct device {
    const char *name;
    void *config;
    void *api;
    void *data;
};

static inline bool device_is_ready(const struct device *dev) { return dev != NULL; }
