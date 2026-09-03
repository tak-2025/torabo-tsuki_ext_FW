/*
 * Host-test stub for <zephyr/sys/atomic.h>.
 *
 * The tests are single-threaded, so plain loads/stores reproduce the firmware's
 * semantics exactly: the double-buffer publish pattern the stores use is about
 * READER tearing, and there are no concurrent readers here.
 */
#pragma once

#include <stdbool.h>

typedef long atomic_t;

#define ATOMIC_INIT(i) (i)

static inline atomic_t atomic_get(const atomic_t *target) { return *target; }

static inline atomic_t atomic_set(atomic_t *target, atomic_t value) {
    atomic_t old = *target;
    *target = value;
    return old;
}

static inline bool atomic_cas(atomic_t *target, atomic_t old_value, atomic_t new_value) {
    if (*target == old_value) {
        *target = new_value;
        return true;
    }
    return false;
}

static inline atomic_t atomic_inc(atomic_t *target) { return (*target)++; }
static inline atomic_t atomic_dec(atomic_t *target) { return (*target)--; }
