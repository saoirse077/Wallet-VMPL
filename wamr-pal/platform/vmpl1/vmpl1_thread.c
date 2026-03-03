/*
 * vmpl1_thread.c - VMPL1 裸机环境下 WAMR 线程/互斥/条件变量/读写锁 API 实现
 *
 * 多 vCPU 感知实现：
 *   - 带所有者跟踪的递归互斥锁（自旋锁 + TID + 计数器）
 *   - 基于 TLS 的每线程环境标记（非全局标志）
 *   - 线程创建委托给 wasmlet_platform（PAL → SVSM）
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "wasmlet_platform.h"

/* ================================================================
 * 互斥锁 —— 带所有者跟踪的递归自旋锁
 *
 * korp_mutex 包含 { pal_spinlock_t lock; owner; count }。
 * 所有互斥锁均支持递归：若当前线程已持有锁，仅递增计数器而不阻塞。
 * ================================================================ */

int
os_mutex_init(korp_mutex *mutex)
{
    if (!mutex)
        return BHT_ERROR;
    pal_spin_init(&mutex->lock);
    mutex->owner = 0;
    mutex->count = 0;
    return BHT_OK;
}

int
os_mutex_destroy(korp_mutex *mutex)
{
    (void)mutex;
    return BHT_OK;
}

int
os_mutex_lock(korp_mutex *mutex)
{
    if (!mutex)
        return BHT_ERROR;

    uint64_t self = wasmlet_thread_self();

    if (mutex->owner == self) {
        mutex->count++;
        return BHT_OK;
    }

    pal_spin_lock(&mutex->lock);
    mutex->owner = self;
    mutex->count = 1;
    return BHT_OK;
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    if (!mutex)
        return BHT_ERROR;

    if (--mutex->count == 0) {
        mutex->owner = 0;
        pal_spin_unlock(&mutex->lock);
    }
    return BHT_OK;
}

int
os_recursive_mutex_init(korp_mutex *mutex)
{
    return os_mutex_init(mutex);
}

/* ================================================================
 * 条件变量 —— 桩实现（WAMR 核心解释器不使用）
 *
 * WAMR 核心解释器不会等待条件变量。仅 thread-mgr 和 WASI threads
 * 需要，而这两者在本项目中均已禁用。
 * ================================================================ */

int
os_cond_init(korp_cond *cond)
{
    (void)cond;
    return BHT_OK;
}

int
os_cond_destroy(korp_cond *cond)
{
    (void)cond;
    return BHT_OK;
}

int
os_cond_wait(korp_cond *cond, korp_mutex *mutex)
{
    (void)cond;
    (void)mutex;
    /* 在当前使用场景下不应被调用 */
    return BHT_OK;
}

int
os_cond_reltimedwait(korp_cond *cond, korp_mutex *mutex, uint64 useconds)
{
    (void)cond;
    (void)mutex;
    (void)useconds;
    return BHT_ERROR;
}

int
os_cond_signal(korp_cond *cond)
{
    (void)cond;
    return BHT_OK;
}

int
os_cond_broadcast(korp_cond *cond)
{
    (void)cond;
    return BHT_OK;
}

/* ================================================================
 * 读写锁 —— 基于 pal_spinlock_t
 *
 * korp_rwlock 即 pal_spinlock_t 的 typedef。
 * 在当前场景下，读锁和写锁行为一致（均为独占）。
 * ================================================================ */

int
os_rwlock_init(korp_rwlock *lock)
{
    if (!lock)
        return BHT_ERROR;
    pal_spin_init(lock);
    return BHT_OK;
}

int
os_rwlock_rdlock(korp_rwlock *lock)
{
    if (!lock)
        return BHT_ERROR;
    pal_spin_lock(lock);
    return BHT_OK;
}

int
os_rwlock_wrlock(korp_rwlock *lock)
{
    if (!lock)
        return BHT_ERROR;
    pal_spin_lock(lock);
    return BHT_OK;
}

int
os_rwlock_unlock(korp_rwlock *lock)
{
    if (!lock)
        return BHT_ERROR;
    pal_spin_unlock(lock);
    return BHT_OK;
}

int
os_rwlock_destroy(korp_rwlock *lock)
{
    (void)lock;
    return BHT_OK;
}

/* ================================================================
 * 线程管理 —— 桩实现
 *
 * WAMR 平台 API 的线程创建接口仅做桩实现（返回错误）。
 * 真正的多线程通过 wasmlet 线程池 + PAL → SVSM 直接接口实现。
 * ================================================================ */

korp_tid
os_self_thread(void)
{
    return (korp_tid)wasmlet_thread_self();
}

uint8 *
os_thread_get_stack_boundary(void)
{
    /* 返回 NULL —— 通过 WASM_DISABLE_STACK_HW_BOUND_CHECK=1 禁用硬件栈边界检查 */
    return NULL;
}

void
os_thread_jit_write_protect_np(bool enabled)
{
    /* 解释器模式下无 JIT */
    (void)enabled;
}

/* 线程创建 —— 不支持（通过 wasmlet 线程池实现） */
int
os_thread_create_with_prio(korp_tid *tid, thread_start_routine_t start,
                           void *arg, unsigned int stack_size, int prio)
{
    (void)tid;
    (void)start;
    (void)arg;
    (void)stack_size;
    (void)prio;
    os_printf("[VMPL1] os_thread_create_with_prio: not supported\n");
    return BHT_ERROR;
}

int
os_thread_create(korp_tid *tid, thread_start_routine_t start, void *arg,
                 unsigned int stack_size)
{
    return os_thread_create_with_prio(tid, start, arg, stack_size,
                                      BH_THREAD_DEFAULT_PRIORITY);
}

int
os_thread_join(korp_tid thread, void **value_ptr)
{
    (void)thread;
    (void)value_ptr;
    return BHT_OK;
}

int
os_thread_detach(korp_tid thread)
{
    (void)thread;
    return BHT_OK;
}

void
os_thread_exit(void *retval)
{
    (void)retval;
    /* 裸机环境下直接返回 */
}

/* ================================================================
 * 线程环境 —— 基于 TLS 的每线程初始化标记
 *
 * 每个线程必须独立调用 wasm_runtime_init_thread_env()。
 * 若使用全局标志，worker 会在主线程设置后跳过自身初始化。
 * ================================================================ */

static wasmlet_tls_key_t tls_key_thread_env = -1;

static void
ensure_thread_env_tls_key(void)
{
    if (tls_key_thread_env < 0)
        wasmlet_tls_create(&tls_key_thread_env);
}

int
os_thread_env_init(void)
{
    ensure_thread_env_tls_key();
    wasmlet_tls_set(tls_key_thread_env, (void *)1);
    return BHT_OK;
}

void
os_thread_env_destroy(void)
{
    wasmlet_tls_set(tls_key_thread_env, (void *)0);
}

bool
os_thread_env_inited(void)
{
    if (tls_key_thread_env < 0)
        return false;
    return wasmlet_tls_get(tls_key_thread_env) != (void *)0;
}

/* ================================================================
 * 睡眠 —— 桩实现（裸机环境无睡眠机制）
 * ================================================================ */

int
os_usleep(uint32 usec)
{
    (void)usec;
    /* 忙等近似：无操作。在本项目的使用场景中不应被调用。 */
    return BHT_OK;
}

/* ================================================================
 * 信号量 —— 桩实现（线程池不使用）
 * ================================================================ */

korp_sem *
os_sem_open(const char *name, int oflags, int mode, int val)
{
    (void)name;
    (void)oflags;
    (void)mode;
    (void)val;
    return NULL;
}

int
os_sem_close(korp_sem *sem)
{
    (void)sem;
    return BHT_ERROR;
}

int
os_sem_wait(korp_sem *sem)
{
    (void)sem;
    return BHT_ERROR;
}

int
os_sem_trywait(korp_sem *sem)
{
    (void)sem;
    return BHT_ERROR;
}

int
os_sem_post(korp_sem *sem)
{
    (void)sem;
    return BHT_ERROR;
}

int
os_sem_getvalue(korp_sem *sem, int *sval)
{
    (void)sem;
    (void)sval;
    return BHT_ERROR;
}

int
os_sem_unlink(const char *name)
{
    (void)name;
    return BHT_ERROR;
}

/* ================================================================
 * 阻塞操作支持 —— 桩实现
 *
 * 已设置 WASM_DISABLE_WAKEUP_BLOCKING_OP=1，
 * 此处仅作为链接符号存在。
 * ================================================================ */

int
os_blocking_op_init(void)
{
    return BHT_OK;
}

void
os_begin_blocking_op(void)
{
}

void
os_end_blocking_op(void)
{
}

int
os_wakeup_blocking_op(korp_tid tid)
{
    (void)tid;
    return BHT_OK;
}
