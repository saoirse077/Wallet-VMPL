/*
 * pal_spinlock.h - Spinlock for bare-metal VMPL1
 *
 * In our bare-metal environment there is no pthread_mutex.
 * We use a simple test-and-set spinlock with PAUSE hint.
 *
 * Usage:
 *   pal_spinlock_t lock = PAL_SPINLOCK_INIT;
 *   pal_spin_lock(&lock);
 *   // critical section
 *   pal_spin_unlock(&lock);
 */
#ifndef PAL_SPINLOCK_H
#define PAL_SPINLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile int locked;
} pal_spinlock_t;

#define PAL_SPINLOCK_INIT { 0 }

/*
 * Acquire the spinlock.
 * Uses __atomic_test_and_set (GCC built-in) which compiles to
 * XCHG or LOCK BTS on x86_64.
 * PAUSE instruction hints the CPU to save power during spin-wait
 * and avoids memory-order pipeline flushes.
 */
static inline void pal_spin_lock(pal_spinlock_t *l)
{
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) {
        /* Spin with PAUSE hint — reduces power and improves performance
         * on hyperthreaded / multi-core CPUs */
        __asm__ volatile("pause" ::: "memory");
    }
}

/*
 * Release the spinlock.
 * Uses __atomic_clear (GCC built-in) which compiles to a
 * store with release semantics.
 */
static inline void pal_spin_unlock(pal_spinlock_t *l)
{
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
}

/*
 * Initialize a spinlock (alternative to PAL_SPINLOCK_INIT for runtime init).
 */
static inline void pal_spin_init(pal_spinlock_t *l)
{
    l->locked = 0;
}

/*
 * Try to acquire the spinlock without blocking.
 * Returns: 0 if lock acquired, non-zero if already held.
 */
static inline int pal_spin_trylock(pal_spinlock_t *l)
{
    return __atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE);
}

#ifdef __cplusplus
}
#endif

#endif /* PAL_SPINLOCK_H */
