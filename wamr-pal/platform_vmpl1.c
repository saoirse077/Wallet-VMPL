/**
 * @file platform_vmpl1.c
 * @brief VMPL1（裸机）环境下 wasmlet 平台抽象层的实现
 *
 * TLS:    通过 GS.base 访问每线程 TCB
 * Mutex:  基于 pal_spinlock_t 的自旋锁
 * Thread: 通过 SVSM monitor call 实现 thread_create/join/exit
 * MPK:    通过 SVSM monitor call 实现六接口
 * Time:   基于 RDTSC 指令
 * Log:    通过 pal_svsm_debug_print 输出到串口
 */

#include "wasmlet_platform.h"
#include "pal_monitor_call.h"
#include "pal_spinlock.h"
#include "pal_string.h"

/* ============ TCB（线程控制块，通过 GS.base 指向）============ */

struct thread_tcb {
    struct thread_tcb *self;
    uint64_t           thread_id;
    void              *tls_slots[WASMLET_TLS_MAX_KEYS];
};

static struct thread_tcb main_tcb;
static int               tls_next_key = 0;

static void set_gs_base(struct thread_tcb *tcb) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFFA;
    data.rbx = (uint64_t)tcb;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
}

static inline struct thread_tcb *get_tcb(void) {
    struct thread_tcb *p;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(p));
    return p;
}

static int gs_initialized = 0;

static void ensure_main_tcb(void) {
    if (gs_initialized)
        return;
    memset(&main_tcb, 0, sizeof(main_tcb));
    main_tcb.self = &main_tcb;
    set_gs_base(&main_tcb);
    gs_initialized = 1;
}

/* ============ TLS（每线程，通过 GS.base TCB 实现）============ */

int wasmlet_tls_create(wasmlet_tls_key_t *key) {
    ensure_main_tcb();
    if (!key || tls_next_key >= WASMLET_TLS_MAX_KEYS)
        return -1;
    *key = tls_next_key++;
    return 0;
}

void wasmlet_tls_delete(wasmlet_tls_key_t key) {
    (void)key;
}

void *wasmlet_tls_get(wasmlet_tls_key_t key) {
    if (key < 0 || key >= WASMLET_TLS_MAX_KEYS)
        return (void *)0;
    struct thread_tcb *tcb = get_tcb();
    return tcb->tls_slots[key];
}

int wasmlet_tls_set(wasmlet_tls_key_t key, void *value) {
    if (key < 0 || key >= WASMLET_TLS_MAX_KEYS)
        return -1;
    struct thread_tcb *tcb = get_tcb();
    tcb->tls_slots[key] = value;
    return 0;
}

/* ============ 互斥锁（自旋锁封装）============ */

static inline pal_spinlock_t *spin_ptr(wasmlet_mutex_t *m) {
    return (pal_spinlock_t *)m->_opaque;
}

int wasmlet_mutex_init(wasmlet_mutex_t *m) {
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    pal_spin_init(spin_ptr(m));
    return 0;
}

int wasmlet_mutex_lock(wasmlet_mutex_t *m) {
    pal_spin_lock(spin_ptr(m));
    return 0;
}

int wasmlet_mutex_unlock(wasmlet_mutex_t *m) {
    pal_spin_unlock(spin_ptr(m));
    return 0;
}

void wasmlet_mutex_destroy(wasmlet_mutex_t *m) {
    (void)m;
}

/* ============ 线程管理（SVSM thread_create/join/exit）============ */

#define THREAD_STACK_DEFAULT_SIZE  (64 * 1024)
#define THREAD_STACK_REGION_START  0x70000000000ULL   /* 7TB 起始，专用于线程栈 */

struct thread_start_info {
    void *(*start_fn)(void *);
    void  *user_arg;
};

static volatile uint64_t next_stack_addr = THREAD_STACK_REGION_START;

/*
 * 线程入口点。SVSM 设置 RDI=arg（指向 thread_start_info），
 * RSP=stack_top, GS.base=TCB 地址。
 */
static void __attribute__((noinline, used))
thread_entry(struct thread_start_info *info) {
    void *ret = info->start_fn(info->user_arg);
    pal_svsm_thread_exit((uint64_t)(uintptr_t)ret);
    __builtin_unreachable();
}

int wasmlet_thread_create(wasmlet_thread_t *t,
                          void *(*start)(void *), void *arg,
                          uint32_t stack_size) {
    ensure_main_tcb();
    if (!t || !start)
        return -1;

    if (stack_size == 0)
        stack_size = THREAD_STACK_DEFAULT_SIZE;
    uint64_t aligned = (stack_size + 0xFFF) & ~0xFFFULL;
    /* 底部额外预留一页用于 TCB + start_info */
    uint64_t total = aligned + 0x1000;

    uint64_t base_val = __atomic_fetch_add(&next_stack_addr, total, __ATOMIC_SEQ_CST);
    void *base = (void *)base_val;

    int ret = pal_svsm_virt_alloc(base, total, 0x3 /* RW */);
    if (ret != 0)
        return -1;
    memset(base, 0, total);

    /* TCB 位于区域最低地址 */
    struct thread_tcb *tcb = (struct thread_tcb *)base;
    tcb->self = tcb;

    /* start_info 紧随 TCB 之后 */
    struct thread_start_info *info =
        (struct thread_start_info *)((uint8_t *)tcb + sizeof(*tcb));
    info->start_fn = start;
    info->user_arg = arg;

    /*
     * x86-64 ABI 要求函数入口处 RSP % 16 == 8（如同 CALL 压入返回地址后）。
     * 减去 8 使硬件设置的 RSP 满足此要求；否则 GCC -O2 可能生成 movaps
     * 操作导致栈未对齐，在裸机上触发 #GP → triple-fault。
     */
    uint64_t stack_top = (uint64_t)base + total - 8;

    uint64_t tid = pal_svsm_thread_create(
        (uint64_t)thread_entry,
        stack_top,
        (uint64_t)tcb,
        (uint64_t)info
    );

    if (tid == UINT64_MAX)
        return -1;

    tcb->thread_id = tid;
    *t = tid;
    return 0;
}

int wasmlet_thread_join(wasmlet_thread_t t, void **retval) {
    uint64_t code = pal_svsm_thread_join(t);
    if (retval)
        *retval = (void *)(uintptr_t)code;
    return 0;
}

uint64_t wasmlet_thread_self(void) {
    if (!gs_initialized)
        return 0;
    /*
     * 使用 TCB 指针作为线程标识。
     * 不能使用 tcb->thread_id，因为：
     *   - 主线程的 thread_id 为 0（从未设置），与 korp_mutex.owner
     *     的"无持有者"哨兵值 (0) 冲突。
     *   - Worker 的 thread_id 在 pal_svsm_thread_create 返回后才设置，
     *     worker 可能竞态读到 0。
     * TCB 地址对每线程唯一（各自独立的 GS.base），且始终非零，
     * 消除了以上两个问题。
     */
    return (uint64_t)get_tcb();
}

void wasmlet_thread_exit(void *retval) {
    pal_svsm_thread_exit((uint64_t)(uintptr_t)retval);
    __builtin_unreachable();
}

/* ============ MPK 原语（SVSM monitor call 实现）============ */

int wasmlet_pkey_alloc(void) {
    return pal_svsm_mpk_pkey_alloc();
}

void *wasmlet_mem_map(size_t size, int pkey) {
    /* 页对齐 */
    size = (size + 0xFFF) & ~0xFFFULL;

    /*
     * pkey > 0: 使用 SVSM mpk_alloc 分配带 pkey 标记的内存。
     * pkey == 0: 使用 pal_svsm_virt_alloc（通用内存分配）。
     *
     * 原子 bump 分配器 —— 对多 worker 线程（各在不同 vCPU）的并发调用安全。
     */
    static volatile uint64_t next_addr = 0x60000000000ULL;  /* 6TB */

    uint64_t addr_val = __atomic_fetch_add(&next_addr, size, __ATOMIC_SEQ_CST);
    void *addr = (void *)addr_val;

    int ret;
    if (pkey > 0) {
        ret = pal_svsm_mpk_alloc(addr, (uint64_t)size, (uint32_t)pkey);
    } else {
        ret = pal_svsm_virt_alloc(addr, (uint64_t)size, 0x3 /* RW */);
    }

    if (ret != 0) {
        return (void *)0;
    }

    /*
     * 仅对 pkey==0 的内存执行零填充（无需切换 PKRU 即可访问）。
     * pkey>0 的页面由 SVSM 在分配时已清零；调用者必须先进入域
     *（更新 PKRU）才能访问。
     */
    if (pkey == 0) {
        memset(addr, 0, size);
    }
    return addr;
}

int wasmlet_pkru_set(int pkey) {
    return pal_svsm_mpk_enter_domain((uint32_t)pkey);
}

void wasmlet_pkru_reset(int pkey) {
    pal_svsm_mpk_exit_domain((uint32_t)pkey);
}

void wasmlet_mem_unmap(void *addr, size_t size) {
    /*
     * pkey=0 区域通过 pal_svsm_virt_alloc 分配，不在 SVSM 的 MpkMemoryManager
     * 中跟踪。没有 pal_svsm_virt_free，故此处跳过释放；SVSM 在 Trustlet
     * 退出时自动回收所有资源。
     *
     * pkey>0 区域通过 wasmlet_pkey_free（mpk_free_pkey）释放，
     * 不经过本函数，故此处无需操作。
     */
    (void)addr;
    (void)size;
}

void wasmlet_pkey_free(int pkey, void *addr, size_t size) {
    pal_svsm_mpk_free_pkey((uint32_t)pkey, addr, (uint64_t)size);
}

void wasmlet_mem_reset(void *addr, size_t size) {
    if (addr && size > 0)
        memset(addr, 0, size);
}

int wasmlet_mem_set_pkey(void *addr, size_t size, int pkey) {
    /*
     * SVSM 不支持对已有页面重新标记 pkey（同一 VA 上 free+realloc 会导致
     * 页分配器 panic）。exec heap 保持 pkey=0，无论 PKRU 状态如何都可访问。
     * 待 SVSM 支持 pkey_mprotect 后，可重新实现按域隔离 exec heap。
     */
    (void)addr;
    (void)size;
    (void)pkey;
    return 0;
}

/* ============ 时间（基于 RDTSC）============ */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 假设 TSC ~2 GHz; >> 11 ≈ / 2048 ≈ / 2000，近似微秒 */
#define TSC_TO_US_SHIFT 11

uint64_t wasmlet_time_us(void) {
    return rdtsc() >> TSC_TO_US_SHIFT;
}

uint64_t wasmlet_time_sec(void) {
    return wasmlet_time_us() / 1000000ULL;
}

void wasmlet_usleep(uint32_t usec) {
    uint64_t target = rdtsc() + ((uint64_t)usec << TSC_TO_US_SHIFT);
    while (rdtsc() < target) {
        __asm__ volatile("pause" ::: "memory");
    }
}

/* ============ 日志输出 ============ */

void wasmlet_log_output(const char *msg) {
    if (msg)
        pal_svsm_debug_print(msg);
}
