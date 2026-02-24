/*
 * mpk_allocator_vmpl1.h - MPK 内存分配器（裸机 VMPL1 版本）
 *
 * 适配 wasmlet 的 mpk_allocator.c 到裸机 VMPL1 环境。
 * 主要变更：
 *   - mmap/munmap → pal_svsm_mpk_alloc / pal_svsm_mpk_free
 *   - pkey_alloc/pkey_free → pal_svsm_mpk_pkey_alloc / pal_svsm_mpk_free_pkey
 *   - wrpkru → pal_svsm_mpk_enter_domain / pal_svsm_mpk_exit_domain
 *   - pthread_mutex → pal_spinlock
 *   - TLS → 全局变量（单线程 Phase 2）
 *   - lockfree_queue → 固定数组（最多 15 个 pkey）
 *
 * 编译开关：ENABLE_MPK_ISOLATION
 *   - =0（默认）：所有 domain 函数为 no-op，mpk_malloc 退化为 pal_malloc
 *   - =1：启用 MPK 隔离
 */

#ifndef MPK_ALLOCATOR_VMPL1_H
#define MPK_ALLOCATOR_VMPL1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 全局分配器（注册到 WAMR） ============ */

void *mpk_malloc(size_t size);
void  mpk_free(void *ptr);
void *mpk_realloc(void *ptr, size_t size);

/*
 * WAMR 适配器：注册 mpk 分配器为 WAMR 运行时的内存分配器。
 * 需要在 wasm_runtime_full_init 之前调用。
 *
 * 前向声明 RuntimeInitArgs 以避免包含 wasm_export.h
 * （wasm_export.h 会引入大量 WAMR 内部头文件）
 */
struct RuntimeInitArgs;
int RegisterMpkAllocatorForWAMR(struct RuntimeInitArgs *args);

#if ENABLE_MPK_ISOLATION

/* ============ MPK Domain 数据结构 ============ */

/*
 * MPK 域：一个 PKEY 关联的持久内存区域（模块级）。
 *
 * 在裸机 VMPL1 中：
 *   - region_base 通过 pal_svsm_mpk_alloc 分配（带 pkey 标记）
 *   - module_msp 是 dlmalloc mspace（在 region 上创建）
 *   - PKRU 切换通过 pal_svsm_mpk_enter/exit_domain 完成
 */
typedef struct {
    int    pkey;            /* SVSM 分配的 PKEY (1-15) */
    void  *region_base;     /* 内存区域基地址 */
    size_t region_size;     /* 内存区域大小 */
    void  *module_msp;      /* dlmalloc mspace (持久堆，首次 enter 时惰性创建) */
} mpk_domain_t;

/* ============ 全局分配器初始化/销毁 ============ */

/**
 * @brief 初始化 MPK 分配器全局状态
 * @return 0 成功，-1 失败
 *
 * 内部依次：初始化 PKEY 池 → 创建 PKEY 0 的默认 mspace。
 */
int mpk_allocator_init(void);

/**
 * @brief 销毁 MPK 分配器全局状态
 */
void mpk_allocator_destroy(void);

/* ============ Domain 生命周期 ============ */

/**
 * @brief 创建 PKEY 域
 * @param module_size 持久堆大小（字节）
 * @param domain 输出
 * @return 0 成功，-1 失败
 */
int mpk_domain_create(size_t module_size, mpk_domain_t *domain);

/**
 * @brief 销毁 PKEY 域
 * @param domain 域结构体
 */
void mpk_domain_destroy(mpk_domain_t *domain);

/* ============ 进入/退出域 ============ */

/**
 * @brief 进入 PKEY 域（启用 PKRU 访问权限 + 切换分配器到域堆）
 * @return 0 成功，-1 失败
 */
int mpk_domain_enter(mpk_domain_t *domain);

/**
 * @brief 退出 PKEY 域（恢复 PKRU + 切换分配器到默认堆）
 */
void mpk_domain_exit(mpk_domain_t *domain);

/* ============ 设置当前 pkey（用于 load_module 时标记内存） ============ */

void mpk_set_current_pkey(int pkey);
int  mpk_get_current_pkey(void);

#else /* !ENABLE_MPK_ISOLATION */

/* ============ 非 MPK 模式：domain 为空壳，所有操作为 no-op ============ */

typedef struct {
    int _dummy;   /* 占位 */
} mpk_domain_t;

static inline int  mpk_allocator_init(void) { return 0; }
static inline void mpk_allocator_destroy(void) {}

static inline int  mpk_domain_create(size_t s, mpk_domain_t *d)
    { (void)s; (void)d; return 0; }
static inline void mpk_domain_destroy(mpk_domain_t *d) { (void)d; }
static inline int  mpk_domain_enter(mpk_domain_t *d) { (void)d; return 0; }
static inline void mpk_domain_exit(mpk_domain_t *d) { (void)d; }

static inline void mpk_set_current_pkey(int pkey) { (void)pkey; }
static inline int  mpk_get_current_pkey(void) { return 0; }

#endif /* ENABLE_MPK_ISOLATION */

#ifdef __cplusplus
}
#endif

#endif /* MPK_ALLOCATOR_VMPL1_H */
