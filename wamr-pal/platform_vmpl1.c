/**
 * @file platform_vmpl1.c
 * @brief VMPL1 (bare-metal) implementation of wasmlet platform abstraction layer
 *
 * TLS: per-thread TCB accessed via GS.base
 * Mutex: pal_spinlock_t
 * Thread: SVSM thread_create/join/exit monitor calls
 * MPK: SVSM monitor calls
 * Time: RDTSC
 * Logging: pal_svsm_debug_print
 */

#include "wasmlet_platform.h"
#include "pal_monitor_call.h"
#include "pal_spinlock.h"
#include "pal_string.h"

/* ============ TCB (Thread Control Block, pointed to by GS.base) ============ */

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

/* ============ TLS (per-thread via GS.base TCB) ============ */

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

/* ============ Mutex (spinlock wrapper) ============ */

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

/* ============ Thread (SVSM thread_create/join/exit) ============ */

#define THREAD_STACK_DEFAULT_SIZE  (64 * 1024)
#define THREAD_STACK_REGION_START  0x70000000000ULL   /* 7TB, dedicated for thread stacks */

struct thread_start_info {
    void *(*start_fn)(void *);
    void  *user_arg;
};

static uint64_t next_stack_addr = THREAD_STACK_REGION_START;

/*
 * Thread entry point. SVSM sets RDI = arg (pointer to thread_start_info),
 * RSP = stack_top, GS.base = TCB address.
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
    /* Reserve extra page for TCB + start_info at the bottom */
    uint64_t total = aligned + 0x1000;

    void *base = (void *)next_stack_addr;
    next_stack_addr += total;

    int ret = pal_svsm_virt_alloc(base, total, 0x3 /* RW */);
    if (ret != 0)
        return -1;
    memset(base, 0, total);

    /* TCB at the very beginning of the region */
    struct thread_tcb *tcb = (struct thread_tcb *)base;
    tcb->self = tcb;

    /* start_info right after TCB */
    struct thread_start_info *info =
        (struct thread_start_info *)((uint8_t *)tcb + sizeof(*tcb));
    info->start_fn = start;
    info->user_arg = arg;

    uint64_t stack_top = (uint64_t)base + total;

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
    struct thread_tcb *tcb = get_tcb();
    return tcb->thread_id;
}

void wasmlet_thread_exit(void *retval) {
    pal_svsm_thread_exit((uint64_t)(uintptr_t)retval);
    __builtin_unreachable();
}

/* ============ MPK Primitives (SVSM monitor calls) ============ */

int wasmlet_pkey_alloc(void) {
    return pal_svsm_mpk_pkey_alloc();
}

void *wasmlet_mem_map(size_t size, int pkey) {
    /* Page-align size */
    size = (size + 0xFFF) & ~0xFFFULL;

    /*
     * For pkey > 0: use SVSM mpk_alloc which allocates memory tagged with pkey.
     * For pkey == 0: use pal_svsm_virt_alloc (general allocation).
     *
     * We need a fixed virtual address for each allocation. Use a simple
     * bump allocator from a reserved region.
     */
    static uint64_t next_addr = 0x60000000000ULL;  /* 6TB, above mmap region */

    void *addr = (void *)next_addr;
    next_addr += size;

    int ret;
    if (pkey > 0) {
        /* pal_svsm_mpk_alloc internally does virt_alloc + pkey tagging */
        ret = pal_svsm_mpk_alloc(addr, (uint64_t)size, (uint32_t)pkey);
    } else {
        ret = pal_svsm_virt_alloc(addr, (uint64_t)size, 0x3 /* RW */);
    }

    if (ret != 0) {
        next_addr -= size;
        return (void *)0;
    }

    /*
     * Only zero-fill pkey==0 memory (accessible without PKRU change).
     * pkey>0 pages are zero-filled by SVSM on allocation; the caller
     * must enter the domain (update PKRU) before accessing them.
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
     * pkey=0 regions are allocated via pal_svsm_virt_alloc and NOT tracked
     * by SVSM's MpkMemoryManager.  There is no pal_svsm_virt_free, so we
     * skip the free; SVSM reclaims all Trustlet resources at exit.
     *
     * pkey>0 regions are freed via wasmlet_pkey_free (mpk_free_pkey),
     * not through this function, so nothing to do here.
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
     * SVSM does not support re-tagging existing pages with a different pkey
     * (free + realloc at the same VA panics the page allocator).
     * For the exec heap, keep it at pkey=0 which is always accessible
     * regardless of PKRU state.  True per-domain exec isolation can be
     * revisited once SVSM adds pkey_mprotect support.
     */
    (void)addr;
    (void)size;
    (void)pkey;
    return 0;
}

/* ============ Time (RDTSC based) ============ */

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ~2 GHz TSC assumed; >> 11 ≈ / 2048 ≈ / 2000 for microseconds */
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

/* ============ Logging ============ */

void wasmlet_log_output(const char *msg) {
    if (msg)
        pal_svsm_debug_print(msg);
}
