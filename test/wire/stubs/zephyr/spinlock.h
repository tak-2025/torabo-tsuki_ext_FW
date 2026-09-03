/* Host-test stub for <zephyr/spinlock.h>. Single-threaded: locking is a no-op. */
#pragma once

struct k_spinlock {
    int _unused;
};

typedef int k_spinlock_key_t;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {
    (void)l;
    return 0;
}

static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key) {
    (void)l;
    (void)key;
}
