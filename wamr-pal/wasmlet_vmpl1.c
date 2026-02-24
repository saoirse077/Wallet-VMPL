/*
 * wasmlet_vmpl1.c - 简化版 wasmlet 核心（裸机 VMPL1 版本）
 *
 * 从 wasmlet/src/core/ 的 wasmlet.c + runtime_internal.c + module_internal.c
 * 简化而来。
 *
 * 关键设计：
 *   - 三层生命周期：Runtime → Module（持久）→ Invocation（临时）
 *   - Phase 2 单模块，不需要 slot 管理和引用计数
 *   - ENABLE_MPK_ISOLATION 编译开关控制 MPK 隔离
 */

#include "wasmlet_vmpl1.h"
#include "mpk_allocator_vmpl1.h"
#include "pal_monitor_call.h"
#include "pal_malloc.h"
#include "pal_string.h"

/* WAMR public API */
#include "wasm_export.h"

/* ============ 内部状态 ============ */

static int g_runtime_initialized = 0;

/* 持久：已加载的模块 */
static wasm_module_t g_module = NULL;

/* MPK 模式下：模块的 pkey 域 */
#if ENABLE_MPK_ISOLATION
static mpk_domain_t g_module_domain;
static int g_domain_created = 0;
#endif

/* ============ 配置常量 ============ */

#define WASM_STACK_SIZE  (32 * 1024)  /* 32KB stack for WASM instance */
#define WASM_HEAP_SIZE   (32 * 1024)  /* 32KB heap for WASM instance */
#define MPK_MODULE_HEAP  (4 * 1024 * 1024)  /* 4MB MPK domain for module */

/* ============ 第一层：Runtime 生命周期 ============ */

int wasmlet_runtime_init(void)
{
    RuntimeInitArgs init_args;

    if (g_runtime_initialized) {
        pal_svsm_debug_print("[WASMLET] Runtime already initialized\n");
        return -1;
    }

    pal_svsm_debug_print("[WASMLET] Initializing runtime...\n");

    /* 1. 初始化 MPK 分配器（非 MPK 模式为 no-op） */
    if (mpk_allocator_init() != 0) {
        pal_svsm_debug_print("[WASMLET] MPK allocator init failed\n");
        return -1;
    }

    /* 2. 配置 WAMR 初始化参数 */
    memset(&init_args, 0, sizeof(init_args));

    /* 注册 MPK 分配器（非 MPK 模式下不注册，使用 WAMR 默认的 os_malloc） */
    RegisterMpkAllocatorForWAMR(&init_args);

    /*
     * 如果 RegisterMpkAllocatorForWAMR 没有设置 mem_alloc_type
     * （非 MPK 模式），则使用系统分配器（即我们的 os_malloc → pal_malloc）
     */
    if (init_args.mem_alloc_type == 0) {
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
    }

    /* 3. 初始化 WAMR runtime */
    if (!wasm_runtime_full_init(&init_args)) {
        pal_svsm_debug_print("[WASMLET] WAMR runtime init failed\n");
        mpk_allocator_destroy();
        return -1;
    }

    g_runtime_initialized = 1;
    pal_svsm_debug_print("[WASMLET] Runtime initialized OK\n");
    return 0;
}

void wasmlet_runtime_destroy(void)
{
    if (!g_runtime_initialized) {
        return;
    }

    pal_svsm_debug_print("[WASMLET] Destroying runtime...\n");

    /* 确保模块已卸载 */
    wasmlet_unload_module();

    /* 销毁 WAMR runtime */
    wasm_runtime_destroy();

    /* 销毁 MPK 分配器 */
    mpk_allocator_destroy();

    g_runtime_initialized = 0;
    pal_svsm_debug_print("[WASMLET] Runtime destroyed\n");
}

/* ============ 第二层：Module 生命周期 ============ */

int wasmlet_load_module(const uint8_t *wasm_buf, uint32_t wasm_size)
{
    char error_buf[128];

    if (!g_runtime_initialized) {
        pal_svsm_debug_print("[WASMLET] Runtime not initialized\n");
        return -1;
    }

    if (!wasm_buf || wasm_size == 0) {
        pal_svsm_debug_print("[WASMLET] Invalid wasm buffer\n");
        return -1;
    }

    if (g_module) {
        pal_svsm_debug_print("[WASMLET] Module already loaded, unload first\n");
        return -1;
    }

    pal_svsm_debug_print("[WASMLET] Loading module (");
    pal_svsm_debug_print_dec((int)wasm_size);
    pal_svsm_debug_print(" bytes)...\n");

#if ENABLE_MPK_ISOLATION
    /* MPK 模式：为模块创建隔离域 */
    if (mpk_domain_create(MPK_MODULE_HEAP, &g_module_domain) != 0) {
        pal_svsm_debug_print("[WASMLET] MPK domain create failed\n");
        return -1;
    }
    g_domain_created = 1;

    /* 进入域（启用 PKRU + 切换分配器到域堆） */
    if (mpk_domain_enter(&g_module_domain) != 0) {
        pal_svsm_debug_print("[WASMLET] MPK domain enter failed\n");
        mpk_domain_destroy(&g_module_domain);
        g_domain_created = 0;
        return -1;
    }
#endif

    /* 加载模块（WAMR 内部会 malloc 解析结构，MPK 模式下走域堆） */
    g_module = wasm_runtime_load((uint8_t *)wasm_buf, wasm_size,
                                 error_buf, sizeof(error_buf));

#if ENABLE_MPK_ISOLATION
    /* 退出域（恢复 PKRU + 切换分配器回默认堆） */
    mpk_domain_exit(&g_module_domain);
#endif

    if (!g_module) {
        pal_svsm_debug_print("[WASMLET] Load failed: ");
        pal_svsm_debug_print(error_buf);
        pal_svsm_debug_print("\n");
#if ENABLE_MPK_ISOLATION
        mpk_domain_destroy(&g_module_domain);
        g_domain_created = 0;
#endif
        return -1;
    }

    pal_svsm_debug_print("[WASMLET] Module loaded OK\n");
    return 0;
}

void wasmlet_unload_module(void)
{
    if (!g_module) {
        return;
    }

    pal_svsm_debug_print("[WASMLET] Unloading module...\n");

#if ENABLE_MPK_ISOLATION
    /* 进入域以释放域内分配的模块内存 */
    if (g_domain_created) {
        mpk_domain_enter(&g_module_domain);
    }
#endif

    wasm_runtime_unload(g_module);
    g_module = NULL;

#if ENABLE_MPK_ISOLATION
    if (g_domain_created) {
        /* 先退出域（恢复 PKRU），再销毁域（释放 mspace + mmap 区域 + 归还 pkey） */
        mpk_domain_exit(&g_module_domain);
        mpk_domain_destroy(&g_module_domain);
        g_domain_created = 0;
    }
#endif

    pal_svsm_debug_print("[WASMLET] Module unloaded\n");
}

/* ============ 第三层：Invocation 生命周期 ============ */

int wasmlet_invoke(const char *func_name, int argc,
                   uint32_t *argv, uint32_t *result)
{
    char error_buf[128];
    wasm_module_inst_t inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t func = NULL;
    int ret = -1;

    if (!g_runtime_initialized || !g_module) {
        pal_svsm_debug_print("[WASMLET] Runtime or module not ready\n");
        return -1;
    }

    if (!func_name) {
        pal_svsm_debug_print("[WASMLET] Function name is NULL\n");
        return -1;
    }

    pal_svsm_debug_print("[WASMLET] Invoking '");
    pal_svsm_debug_print(func_name);
    pal_svsm_debug_print("'...\n");

#if ENABLE_MPK_ISOLATION
    /* 进入域：实例化和执行都在域内进行 */
    if (g_domain_created) {
        if (mpk_domain_enter(&g_module_domain) != 0) {
            pal_svsm_debug_print("[WASMLET] MPK domain enter failed\n");
            return -1;
        }
    }
#endif

    /* 1. 实例化模块（临时：分配独立的线性内存） */
    pal_svsm_debug_print("[WASMLET] Instantiating module...\n");
    inst = wasm_runtime_instantiate(g_module,
                                    WASM_STACK_SIZE, WASM_HEAP_SIZE,
                                    error_buf, sizeof(error_buf));
    if (!inst) {
        pal_svsm_debug_print("[WASMLET] Instantiate failed: ");
        pal_svsm_debug_print(error_buf);
        pal_svsm_debug_print("\n");
        goto cleanup;
    }

    /* 2. 创建执行环境（临时） */
    exec_env = wasm_runtime_create_exec_env(inst, WASM_STACK_SIZE);
    if (!exec_env) {
        pal_svsm_debug_print("[WASMLET] Create exec env failed\n");
        goto cleanup;
    }

    /* 3. 查找函数 */
    func = wasm_runtime_lookup_function(inst, func_name);
    if (!func) {
        pal_svsm_debug_print("[WASMLET] Function '");
        pal_svsm_debug_print(func_name);
        pal_svsm_debug_print("' not found\n");
        goto cleanup;
    }

    /* 4. 调用函数 */
    if (!wasm_runtime_call_wasm(exec_env, func, (uint32_t)argc, argv)) {
        const char *exception = wasm_runtime_get_exception(inst);
        pal_svsm_debug_print("[WASMLET] Call failed: ");
        if (exception) {
            pal_svsm_debug_print(exception);
        }
        pal_svsm_debug_print("\n");
        goto cleanup;
    }

    /* 5. 获取返回值（WAMR 将返回值写回 argv[0]） */
    if (result) {
        *result = argv[0];
    }

    pal_svsm_debug_print("[WASMLET] Call succeeded, result=");
    pal_svsm_debug_print_dec((int)argv[0]);
    pal_svsm_debug_print("\n");

    ret = 0;

cleanup:
    /* 6. 销毁临时资源（执行环境 + 实例） */
    if (exec_env) {
        wasm_runtime_destroy_exec_env(exec_env);
    }
    if (inst) {
        wasm_runtime_deinstantiate(inst);
        pal_svsm_debug_print("[WASMLET] Instance destroyed\n");
    }

#if ENABLE_MPK_ISOLATION
    /* 退出域 */
    if (g_domain_created) {
        mpk_domain_exit(&g_module_domain);
    }
#endif

    return ret;
}
