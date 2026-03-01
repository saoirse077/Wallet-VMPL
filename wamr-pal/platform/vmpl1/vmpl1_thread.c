/*
 * vmpl1_thread.c - WAMR thread/mutex/cond/rwlock APIs for bare-metal VMPL1
 *
 * In our bare-metal environment:
 *   - No pthreads — all mutex ops use pal_spinlock
 *   - No condition variables — stubs return BHT_OK
 *   - No thread creation — stubs return BHT_ERROR
 *   - Single vCPU, single thread in Phase 2
 *
 * For future multi-vCPU support, thread creation will be handled
 * directly via PAL → SVSM interface, bypassing WAMR's os_thread_create.
 */
 /*
 * vmpl1_thread.c - 面向裸机 VMPL1 的 WAMR 线程/互斥锁/条件变量/读写锁接口实现
 *
 * 在我们的裸机环境中：
 *   - 不支持 pthread —— 所有互斥锁操作都使用 pal_spinlock 实现
 *   - 不支持条件变量 —— 相关桩函数直接返回 BHT_OK
 *   - 不支持线程创建 —— 相关桩函数直接返回 BHT_ERROR
 *   - 第二阶段（Phase 2）仅有单个 vCPU、单线程运行
 *
 * 未来若支持多 vCPU，线程创建将通过
 * PAL → SVSM 接口直接实现，
 * 而不是通过 WAMR 的 os_thread_create。
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "wasmlet_platform.h"

/* ================================================================
 * Mutex — backed by pal_spinlock_t
 *
 * korp_mutex is typedef'd to pal_spinlock_t in platform_internal.h.
 * In single-thread Phase 2, these are never contended.
 * ================================================================ */

int
os_mutex_init(korp_mutex *mutex)
{
    if (!mutex)
        return BHT_ERROR;
    pal_spin_init(mutex);
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
    pal_spin_lock(mutex);
    return BHT_OK;
}

int
os_mutex_unlock(korp_mutex *mutex)
{
    if (!mutex)
        return BHT_ERROR;
    pal_spin_unlock(mutex);
    return BHT_OK;
}

/* Recursive mutex — in single-thread mode, same as regular mutex.
 * For true recursive support, we'd need to track owner thread ID
 * and recursion count. Not needed in Phase 2. */
int
os_recursive_mutex_init(korp_mutex *mutex)
{
    return os_mutex_init(mutex);
}

/* ================================================================
 * Condition variables — stubs (not used in single-thread mode)
 *
 * WAMR's core interpreter doesn't actually wait on condvars.
 * These are only needed for thread-mgr and WASI threads, both
 * of which we have disabled.
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
    /* Should never be called in single-thread mode */
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
 * Read-write lock — backed by pal_spinlock_t
 *
 * korp_rwlock is typedef'd to pal_spinlock_t.
 * In single-thread mode, read and write locks are identical.
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
 * Thread management — stubs
 *
 * In Phase 2, we have a single vCPU running a single thread.
 * Thread creation is not supported through WAMR's platform API.
 * Future multi-vCPU support will use PAL → SVSM direct interface.
 * ================================================================ */

korp_tid
os_self_thread(void)
{
    return (korp_tid)wasmlet_thread_self();
}

uint8 *
os_thread_get_stack_boundary(void)
{
    /* Return NULL — we disable hardware stack boundary check
     * via WASM_DISABLE_STACK_HW_BOUND_CHECK=1 */
    return NULL;
}

void
os_thread_jit_write_protect_np(bool enabled)
{
    /* No JIT in interpreter mode */
    (void)enabled;
}

/* Thread creation — not supported */
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
    /* In bare-metal, just halt */
}

/* ================================================================
 * Thread environment — used by wasm_runtime_init_thread_env()
 *
 * In single-thread mode, we just track a boolean flag.
 * ================================================================ */

static bool thread_env_initialized = false;

int
os_thread_env_init(void)
{
    thread_env_initialized = true;
    return BHT_OK;
}

void
os_thread_env_destroy(void)
{
    thread_env_initialized = false;
}

bool
os_thread_env_inited(void)
{
    return thread_env_initialized;
}

/* ================================================================
 * Sleep — stub (bare-metal has no sleep mechanism)
 * ================================================================ */

int
os_usleep(uint32 usec)
{
    (void)usec;
    /* Busy-wait approximation: do nothing.
     * In practice, this should never be called in our use case. */
    return BHT_OK;
}

/* ================================================================
 * Semaphore — stubs (not used by our thread pool)
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
 * Blocking operation support — stubs
 *
 * WASM_DISABLE_WAKEUP_BLOCKING_OP=1, so these are only needed
 * as link symbols.
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
