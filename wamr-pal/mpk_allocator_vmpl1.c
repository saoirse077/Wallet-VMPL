/*
 * mpk_allocator_vmpl1.c - MPK 内存分配器（裸机 VMPL1 版本）
 *
 * 适配 wasmlet 的 mpk_allocator.c 到裸机 VMPL1 环境。
 *
 * 主要适配点：
 *   1. mmap/munmap       → pal_svsm_mpk_alloc / pal_svsm_mpk_free
 *   2. pkey_alloc/free   → pal_svsm_mpk_pkey_alloc / pal_svsm_mpk_free_pkey
 *   3. wrpkru            → pal_svsm_mpk_enter_domain / pal_svsm_mpk_exit_domain
 *   4. pthread_mutex     → pal_spinlock（Phase 2 单线程实际不竞争）
 *   5. TLS               → 全局变量（Phase 2 单线程）
 *   6. lockfree_queue    → 固定数组（最多 15 个 pkey）
 *   7. madvise           → memset 清零（裸机无 madvise）
 *
 * 编译条件：仅当 ENABLE_MPK_ISOLATION=1 时编译此文件。
 */

#include "mpk_allocator_vmpl1.h"
#include "pal_monitor_call.h"
#include "pal_malloc.h"
#include "pal_string.h"
#include "pal_spinlock.h"

/* ================================================================
 * 非 MPK 模式：mpk_malloc/free/realloc 退化为 pal_malloc
 * ================================================================ */

#if !ENABLE_MPK_ISOLATION

void *mpk_malloc(size_t size)
{
    return pal_malloc(size);
}

void mpk_free(void *ptr)
{
    pal_free(ptr);
}

void *mpk_realloc(void *ptr, size_t size)
{
    return pal_realloc(ptr, size);
}

int RegisterMpkAllocatorForWAMR(struct RuntimeInitArgs *args)
{
    (void)args;
    /* 非 MPK 模式：不注册自定义分配器，使用 WAMR 默认的 os_malloc */
    return 0;
}

#else /* ENABLE_MPK_ISOLATION */

/* ================================================================
 * MPK 模式完整实现
 * ================================================================ */

/* dlmalloc mspace API */
typedef void *mspace;
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void   destroy_mspace(mspace msp);
extern void  *mspace_malloc(mspace msp, size_t bytes);
extern void   mspace_free(mspace msp, void *mem);
extern void  *mspace_realloc(mspace msp, void *mem, size_t newsize);
extern void   mspace_track_large_chunks(mspace msp, int enable);

/* WAMR RuntimeInitArgs forward declaration */
#include "wasm_export.h"

/* ============ 常量 ============ */

#define PAGE_SIZE       4096ULL
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define MIN_MSPACE_SIZE (PAGE_SIZE * 4)  /* 16KB */

/* 默认堆大小（Phase 2b 减小到 4MB 以减少 SVSM 日志量） */
#define DEFAULT_HEAP_SIZE (4ULL * 1024 * 1024)  /* 4MB */

/* MPK 域内存区域的起始地址（在 mmap 区域之后） */
/* 注意：这个地址需要和 vmpl1_mmap.c 的 bump allocator 不冲突
 * vmpl1_mmap.c 从 0x50_0000_0000 开始递增
 * 我们的 MPK 域从 0x60_0000_0000 开始，留出足够空间 */
#define MPK_REGION_BASE 0x6000000000ULL

/* PKEY 池最大容量（Phase 2b 只需 1-2 个，减少预分配以降低 SVSM 日志量） */
#define PKEY_POOL_SIZE 4
#define PKEY_POOL_MAX 15

/* ============ PKEY 池（简化版，固定数组） ============ */

static int  g_pkey_pool[PKEY_POOL_MAX];    /* 从 SVSM 分配的所有 pkey */
static int  g_pkey_free[PKEY_POOL_MAX];    /* 空闲 pkey 栈 */
static int  g_pkey_free_top = 0;           /* 栈顶指针 */
static int  g_pkey_total = 0;              /* 总共分配的 pkey 数量 */
static pal_spinlock_t g_pkey_lock = PAL_SPINLOCK_INIT;

/* ============ 全局状态 ============ */

/* 默认堆（PKEY 0，全局共享） */
static mspace g_default_msp = (void *)0;
static void  *g_default_base = (void *)0;
static size_t g_default_size = 0;
static int    g_allocator_initialized = 0;

/* 当前分配目标 mspace（Phase 2 单线程，用全局变量代替 TLS） */
static mspace g_current_msp = (void *)0;

/* 当前 pkey（用于 load_module 时让 WAMR 内部分配到正确的域） */
static int g_current_pkey = 0;

/* MPK 域地址分配器（类似 vmpl1_mmap.c 的 bump allocator） */
static uint64_t g_mpk_next_addr = MPK_REGION_BASE;

/* ============ 内部：PKEY 池操作 ============ */

static int pkey_pool_init(void)
{
    int i;

    pal_svsm_debug_print("[MPK] Initializing PKEY pool (max=");
    pal_svsm_debug_print_dec(PKEY_POOL_SIZE);
    pal_svsm_debug_print(")...\n");

    /* 从 SVSM 预分配 pkey（限制为 PKEY_POOL_SIZE 以减少日志量） */
    for (i = 0; i < PKEY_POOL_SIZE; i++) {
        int pkey = pal_svsm_mpk_pkey_alloc();
        if (pkey < 1 || pkey > 15) {
            /* SVSM 没有更多可用的 pkey */
            pal_svsm_debug_print("[MPK] pkey_alloc returned ");
            pal_svsm_debug_print_dec(pkey);
            pal_svsm_debug_print(", stopping\n");
            break;
        }
        pal_svsm_debug_print("[MPK] Allocated pkey=");
        pal_svsm_debug_print_dec(pkey);
        pal_svsm_debug_print("\n");
        g_pkey_pool[g_pkey_total] = pkey;
        g_pkey_free[g_pkey_total] = pkey;
        g_pkey_total++;
    }

    if (g_pkey_total == 0) {
        pal_svsm_debug_print("[MPK] Failed to allocate any PKEY from SVSM\n");
        return -1;
    }

    g_pkey_free_top = g_pkey_total;

    pal_svsm_debug_print("[MPK] PKEY pool initialized: ");
    pal_svsm_debug_print_dec(g_pkey_total);
    pal_svsm_debug_print(" pkeys available\n");
    return 0;
}

static void pkey_pool_destroy(void)
{
    int i;
    for (i = 0; i < g_pkey_total; i++) {
        pal_svsm_mpk_free_pkey((uint32_t)g_pkey_pool[i], (void *)0, 0);
    }
    g_pkey_total = 0;
    g_pkey_free_top = 0;
}

static int pkey_pool_alloc(void)
{
    int pkey;

    pal_spin_lock(&g_pkey_lock);
    if (g_pkey_free_top <= 0) {
        pal_spin_unlock(&g_pkey_lock);
        pal_svsm_debug_print("[MPK] PKEY pool exhausted\n");
        return -1;
    }
    g_pkey_free_top--;
    pkey = g_pkey_free[g_pkey_free_top];
    pal_spin_unlock(&g_pkey_lock);

    return pkey;
}

static void pkey_pool_free(int pkey)
{
    pal_spin_lock(&g_pkey_lock);
    if (g_pkey_free_top < g_pkey_total) {
        g_pkey_free[g_pkey_free_top] = pkey;
        g_pkey_free_top++;
    }
    pal_spin_unlock(&g_pkey_lock);
}

/* ============ 内部：内存区域映射 ============ */

/*
 * 分配一块带 pkey 标记的内存区域。
 * 使用 pal_svsm_mpk_alloc（CPUID trap 到 VMPL0）。
 */
static void *mpk_region_map(size_t size, int pkey)
{
    uint64_t addr;

    size = ALIGN_UP(size, PAGE_SIZE);

    /* 从 bump allocator 分配地址 */
    addr = g_mpk_next_addr;
    g_mpk_next_addr += size;

    /* 安全检查：不要超过 input channel */
    if (g_mpk_next_addr >= 0x28000000000ULL) {
        pal_svsm_debug_print("[MPK] Address space exhausted\n");
        g_mpk_next_addr = addr; /* rollback */
        return (void *)0;
    }

    /* 通过 SVSM 分配带 pkey 的内存 */
    if (pkey > 0) {
        int ret = pal_svsm_mpk_alloc((void *)addr, size, (uint32_t)pkey);
        if (ret != 0) {
            pal_svsm_debug_print("[MPK] mpk_alloc failed for pkey=");
            pal_svsm_debug_print_dec(pkey);
            pal_svsm_debug_print("\n");
            g_mpk_next_addr = addr; /* rollback */
            return (void *)0;
        }
    }
    else {
        /* pkey 0: 使用普通 virt_alloc */
        int ret = pal_svsm_virt_alloc((void *)addr, size, 0x3 /* RW */);
        if (ret != 0) {
            pal_svsm_debug_print("[MPK] virt_alloc failed for default heap\n");
            g_mpk_next_addr = addr; /* rollback */
            return (void *)0;
        }
    }

    /* 清零：
     * - pkey 0 区域：需要显式清零（pal_svsm_virt_alloc 分配的页可能未清零）
     * - pkey > 0 区域：SVSM 的 add_pages() 内部已经清零（分配物理页时），
     *   而且此时 PKRU 还没有授权访问该 pkey，memset 会触发 MPK PF！
     *   所以必须跳过。
     */
    if (pkey == 0) {
        memset((void *)addr, 0, size);
    }
    /* pkey > 0: pages are already zeroed by SVSM's add_pages() */

    return (void *)addr;
}

/*
 * 释放内存区域（带 pkey 参数）。
 *
 * pkey > 0: 通过 pal_svsm_mpk_free 释放（SVSM MpkMemoryManager 管理的区域）
 * pkey = 0: 不调用 mpk_free（该区域由 pal_svsm_virt_alloc 分配，
 *           不在 SVSM MpkMemoryManager 中跟踪，SVSM 会在 Trustlet 退出时自动清理）
 *
 * 注意：bump allocator 不回收虚拟地址空间。
 */
static void mpk_region_unmap_pkey(void *base, size_t size, int pkey)
{
    if (!base) {
        return;
    }
    size = ALIGN_UP(size, PAGE_SIZE);

    pal_svsm_debug_print("[MPK] Unmapping region: base=");
    pal_svsm_debug_print_hex((uint64_t)base);
    pal_svsm_debug_print(", size=");
    pal_svsm_debug_print_dec((int)size);
    pal_svsm_debug_print(", pkey=");
    pal_svsm_debug_print_dec(pkey);
    pal_svsm_debug_print("\n");

    if (pkey > 0) {
        int ret = pal_svsm_mpk_free(base, size);
        if (ret != 0) {
            pal_svsm_debug_print("[MPK] mpk_free FAILED for pkey=");
            pal_svsm_debug_print_dec(pkey);
            pal_svsm_debug_print(", ret=");
            pal_svsm_debug_print_dec(ret);
            pal_svsm_debug_print("\n");
        }
    } else {
        /* pkey 0 regions are allocated via pal_svsm_virt_alloc and not
         * tracked by SVSM's MpkMemoryManager. SVSM will clean up all
         * resources at Trustlet exit, so no explicit free needed here. */
        pal_svsm_debug_print("[MPK] Skipping mpk_free for pkey 0 region\n");
    }
}

/* ============ mspace 创建辅助 ============ */

static mspace create_tracked_mspace(void *base, size_t size)
{
    mspace msp = create_mspace_with_base(base, size, 1);
    if (msp) {
        mspace_track_large_chunks(msp, 1);
    }
    return msp;
}

/* ============ 全局分配器 init/destroy ============ */

int mpk_allocator_init(void)
{
    if (g_allocator_initialized) {
        return -1;
    }

    pal_svsm_debug_print("[MPK] ========== MPK Allocator Init ==========\n");

    /* 1. 初始化 PKEY 池 */
    if (pkey_pool_init() != 0) {
        return -1;
    }

    /* 2. 创建默认堆（PKEY 0） */
    g_default_size = DEFAULT_HEAP_SIZE;
    pal_svsm_debug_print("[MPK] Creating default heap (pkey=0, size=");
    pal_svsm_debug_print_dec((int)(g_default_size / (1024 * 1024)));
    pal_svsm_debug_print("MB)...\n");

    g_default_base = mpk_region_map(g_default_size, 0);
    if (!g_default_base) {
        pal_svsm_debug_print("[MPK] Failed to create default heap\n");
        pkey_pool_destroy();
        return -1;
    }

    g_default_msp = create_tracked_mspace(g_default_base, g_default_size);
    if (!g_default_msp) {
        pal_svsm_debug_print("[MPK] Failed to create default mspace\n");
        mpk_region_unmap_pkey(g_default_base, g_default_size, 0);
        g_default_base = (void *)0;
        pkey_pool_destroy();
        return -1;
    }

    g_current_msp = g_default_msp;
    g_allocator_initialized = 1;

    pal_svsm_debug_print("[MPK] Allocator initialized (default heap=");
    pal_svsm_debug_print_dec((int)(g_default_size / (1024 * 1024)));
    pal_svsm_debug_print("MB)\n");
    return 0;
}

void mpk_allocator_destroy(void)
{
    if (!g_allocator_initialized) {
        return;
    }

    pal_svsm_debug_print("[MPK] Destroying allocator...\n");

    if (g_default_msp) {
        destroy_mspace(g_default_msp);
        g_default_msp = (void *)0;
    }
    if (g_default_base) {
        mpk_region_unmap_pkey(g_default_base, g_default_size, 0);
        g_default_base = (void *)0;
    }

    pkey_pool_destroy();

    g_current_msp = (void *)0;
    g_allocator_initialized = 0;

    pal_svsm_debug_print("[MPK] Allocator destroyed\n");
}

/* ============ Domain 生命周期 ============ */

int mpk_domain_create(size_t module_size, mpk_domain_t *domain)
{
    if (!domain || module_size < MIN_MSPACE_SIZE) {
        pal_svsm_debug_print("[MPK] domain_create: invalid args\n");
        return -1;
    }

    pal_svsm_debug_print("[MPK] Creating domain (requested size=");
    pal_svsm_debug_print_dec((int)(module_size / 1024));
    pal_svsm_debug_print("KB)...\n");

    /* 1. 从池中分配 PKEY */
    int pkey = pkey_pool_alloc();
    if (pkey < 0) {
        pal_svsm_debug_print("[MPK] domain_create: pkey_pool_alloc failed\n");
        return -1;
    }

    module_size = ALIGN_UP(module_size, PAGE_SIZE);

    /* 2. 分配带 pkey 标记的内存区域 */
    pal_svsm_debug_print("[MPK] Allocating region: pkey=");
    pal_svsm_debug_print_dec(pkey);
    pal_svsm_debug_print(", size=");
    pal_svsm_debug_print_dec((int)(module_size / 1024));
    pal_svsm_debug_print("KB\n");

    void *base = mpk_region_map(module_size, pkey);
    if (!base) {
        pal_svsm_debug_print("[MPK] domain_create: mpk_region_map failed\n");
        pkey_pool_free(pkey);
        return -1;
    }

    domain->pkey = pkey;
    domain->region_base = base;
    domain->region_size = module_size;
    domain->module_msp = (void *)0;  /* 延迟到首次 enter 创建 */

    pal_svsm_debug_print("[MPK] Domain created: pkey=");
    pal_svsm_debug_print_dec(pkey);
    pal_svsm_debug_print(", base=");
    pal_svsm_debug_print_hex((uint64_t)base);
    pal_svsm_debug_print(", size=");
    pal_svsm_debug_print_dec((int)(module_size / 1024));
    pal_svsm_debug_print("KB\n");
    return 0;
}

void mpk_domain_destroy(mpk_domain_t *domain)
{
    if (!domain || !domain->region_base) {
        return;
    }

    pal_svsm_debug_print("[MPK] Destroying domain: pkey=");
    pal_svsm_debug_print_dec(domain->pkey);
    pal_svsm_debug_print("\n");

    /* If module_msp exists, destroy it.
     * We need to temporarily enter the domain because destroy_mspace
     * accesses the domain's memory (which is protected by pkey).
     * After mpk_domain_exit(), the pkey's AD bit is set in PKRU,
     * so we can't access the memory without re-entering. */
    if (domain->module_msp) {
        pal_svsm_debug_print("[MPK] Temporarily entering domain to destroy mspace...\n");
        if (mpk_domain_enter(domain) != 0) {
            pal_svsm_debug_print("[MPK] FATAL: Failed to enter domain for mspace destroy\n");
            /* Attempt to proceed, but this is a critical error */
        } else {
            destroy_mspace(domain->module_msp);
            pal_svsm_debug_print("[MPK] mspace destroyed. Exiting domain...\n");
            mpk_domain_exit(domain);
        }
        domain->module_msp = (void *)0;
    }

    /* Free memory region (with correct pkey for proper cleanup) */
    mpk_region_unmap_pkey(domain->region_base, domain->region_size, domain->pkey);

    int pkey = domain->pkey;

    domain->region_base = (void *)0;
    domain->region_size = 0;
    domain->pkey = 0;

    /* 归还 PKEY 到池 */
    pkey_pool_free(pkey);

    pal_svsm_debug_print("[MPK] Domain destroyed: pkey=");
    pal_svsm_debug_print_dec(pkey);
    pal_svsm_debug_print("\n");
}

/* ============ Enter / Exit ============ */

int mpk_domain_enter(mpk_domain_t *domain)
{
    if (!domain || !domain->region_base) {
        pal_svsm_debug_print("[MPK] domain_enter: invalid domain\n");
        return -1;
    }

    pal_svsm_debug_print("[MPK] Entering domain: pkey=");
    pal_svsm_debug_print_dec(domain->pkey);
    pal_svsm_debug_print("\n");

    /* 1. 通过 SVSM 启用域的 PKEY（修改 VMSA.PKRU） */
    int ret = pal_svsm_mpk_enter_domain((uint32_t)domain->pkey);
    if (ret != 0) {
        pal_svsm_debug_print("[MPK] enter_domain FAILED for pkey=");
        pal_svsm_debug_print_dec(domain->pkey);
        pal_svsm_debug_print(", ret=");
        pal_svsm_debug_print_dec(ret);
        pal_svsm_debug_print("\n");
        return -1;
    }

    /* 2. 惰性创建持久堆（首次 enter 时） */
    if (!domain->module_msp) {
        pal_svsm_debug_print("[MPK] Creating module mspace (first enter)...\n");
        domain->module_msp = create_tracked_mspace(
            domain->region_base, domain->region_size);
        if (!domain->module_msp) {
            pal_svsm_debug_print("[MPK] Failed to create module mspace\n");
            pal_svsm_mpk_exit_domain((uint32_t)domain->pkey);
            return -1;
        }
        pal_svsm_debug_print("[MPK] Module mspace created OK\n");
    }

    /* 3. 切换分配器到域堆 */
    g_current_msp = domain->module_msp;

    pal_svsm_debug_print("[MPK] Domain entered OK: pkey=");
    pal_svsm_debug_print_dec(domain->pkey);
    pal_svsm_debug_print("\n");
    return 0;
}

void mpk_domain_exit(mpk_domain_t *domain)
{
    if (domain) {
        pal_svsm_debug_print("[MPK] Exiting domain: pkey=");
        pal_svsm_debug_print_dec(domain->pkey);
        pal_svsm_debug_print("\n");
    }

    /* 1. 恢复 PKRU 到默认（设置 AD 位禁止访问域内存） */
    if (domain) {
        pal_svsm_mpk_exit_domain((uint32_t)domain->pkey);
    }

    /* 2. 切换分配器回默认堆 */
    g_current_msp = g_default_msp;

    pal_svsm_debug_print("[MPK] Domain exited OK\n");
}

/* ============ 设置/获取当前 pkey ============ */

void mpk_set_current_pkey(int pkey)
{
    g_current_pkey = pkey;
}

int mpk_get_current_pkey(void)
{
    return g_current_pkey;
}

/* ============ 全局分配器 ============ */

void *mpk_malloc(size_t size)
{
    mspace msp = g_current_msp;
    if (!msp) {
        msp = g_default_msp;
    }
    if (!msp) {
        /* 分配器未初始化，回退到 pal_malloc */
        return pal_malloc(size);
    }
    return mspace_malloc(msp, size);
}

void mpk_free(void *ptr)
{
    mspace msp;
    if (!ptr)
        return;
    msp = g_current_msp;
    if (!msp)
        msp = g_default_msp;
    if (msp) {
        mspace_free(msp, ptr);
        return;
    }
    pal_free(ptr);
}

void *mpk_realloc(void *ptr, size_t size)
{
    mspace msp;
    if (!ptr)
        return mpk_malloc(size);
    if (size == 0) {
        mpk_free(ptr);
        return (void *)0;
    }
    msp = g_current_msp;
    if (!msp)
        msp = g_default_msp;
    if (msp)
        return mspace_realloc(msp, ptr, size);
    return pal_realloc(ptr, size);
}

/* ============ WAMR 适配器 ============ */

int RegisterMpkAllocatorForWAMR(struct RuntimeInitArgs *args)
{
    if (!args) {
        return -1;
    }

    args->mem_alloc_type = Alloc_With_Allocator;
    args->mem_alloc_option.allocator.malloc_func = mpk_malloc;
    args->mem_alloc_option.allocator.realloc_func = mpk_realloc;
    args->mem_alloc_option.allocator.free_func = mpk_free;
    return 0;
}

#endif /* ENABLE_MPK_ISOLATION */
