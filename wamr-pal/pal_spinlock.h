/*
 * pal_spinlock.h - Spinlock for bare-metal VMPL1
 *
 * In our bare-metal environment there is no pthread_mutex.
 * We use a simple test-and-set spinlock with PAUSE hint.
 *
 * For Phase 2 (single vCPU, single thread), the lock is never contended.
 * But we implement it correctly for future multi-vCPU support.
 *
 * Usage:
 *   pal_spinlock_t lock = PAL_SPINLOCK_INIT;
 *   pal_spin_lock(&lock);
 *   // critical section
 *   pal_spin_unlock(&lock);
 */
/*
 * pal_spinlock.h - 面向裸机 VMPL1 的自旋锁实现
 *
 * 在我们的裸机环境中不存在 pthread_mutex。
 * 因此我们使用基于 test-and-set 的简单自旋锁，并配合 PAUSE 指令提示。
 *
 * 在 Phase 2（单 vCPU、单线程）阶段，锁实际上不会发生竞争。
 * 但我们仍然按照正确方式实现，以便未来支持多 vCPU 场景。
 *
 * 使用方法：
 *   pal_spinlock_t lock = PAL_SPINLOCK_INIT;
 *   pal_spin_lock(&lock);
 *   // 临界区
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
