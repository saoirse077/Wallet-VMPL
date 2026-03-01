/**
 * @file platform_vmpl1.c
 * @brief VMPL1 (bare-metal) implementation of wasmlet platform abstraction layer
 *
 * TLS: global array (single-threaded initially, GS.base TCB later)
 * Mutex: pal_spinlock_t
 * Thread: stubs (single-threaded initially, SVSM thread_create later)
 * MPK: SVSM monitor calls
 * Time: RDTSC
 * Logging: pal_svsm_debug_print
 */

#include "wasmlet_platform.h"
#include "pal_monitor_call.h"
#include "pal_spinlock.h"
#include "pal_string.h"

/* ============ TLS (global array, single-threaded) ============ */

static void *tls_slots[WASMLET_TLS_MAX_KEYS];
static int   tls_next_key = 0;

int wasmlet_tls_create(wasmlet_tls_key_t *key) {
    if (!key || tls_next_key >= WASMLET_TLS_MAX_KEYS)
        return -1;
    int idx = tls_next_key++;
    tls_slots[idx] = (void *)0;
    *key = idx;
    return 0;
}

void wasmlet_tls_delete(wasmlet_tls_key_t key) {
    if (key >= 0 && key < WASMLET_TLS_MAX_KEYS)
        tls_slots[key] = (void *)0;
}

void *wasmlet_tls_get(wasmlet_tls_key_t key) {
    if (key < 0 || key >= WASMLET_TLS_MAX_KEYS)
        return (void *)0;
    return tls_slots[key];
}

int wasmlet_tls_set(wasmlet_tls_key_t key, void *value) {
    if (key < 0 || key >= WASMLET_TLS_MAX_KEYS)
        return -1;
    tls_slots[key] = value;
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

/* ============ Thread (stubs, single-threaded) ============ */

int wasmlet_thread_create(wasmlet_thread_t *t,
                          void *(*start)(void *), void *arg,
                          uint32_t stack_size) {
    (void)t; (void)start; (void)arg; (void)stack_size;
    return -1;  /* not supported yet */
}

int wasmlet_thread_join(wasmlet_thread_t t, void **retval) {
    (void)t; (void)retval;
    return -1;
}

uint64_t wasmlet_thread_self(void) {
    return 0;  /* main thread */
}

void wasmlet_thread_exit(void *retval) {
    (void)retval;
    pal_svsm_exit(0);
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
