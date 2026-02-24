/*
 * wasmlet_vmpl1.h - 简化版 wasmlet 核心（裸机 VMPL1 版本）
 *
 * 从 wasmlet/src/core/ 的 wasmlet.c + runtime_internal.c + module_internal.c
 * 简化而来，适配裸机 VMPL1 环境。
 *
 * 关键简化：
 *   - 去掉线程池、异步执行、结果存储
 *   - 去掉 slot 管理和引用计数（Phase 2 单模块）
 *   - 去掉并发多线程实例化同一模块的逻辑
 *   - 模块加载与实例化/执行分离（serverless 模型）
 *
 * 三层 API 设计：
 *   1. Runtime 生命周期（全局，只调用一次）
 *   2. Module 生命周期（持久，加载后常驻，可多次 invoke）
 *   3. Invocation 生命周期（临时，每次调用 instantiate + execute + deinstantiate）
 */

#ifndef WASMLET_VMPL1_H
#define WASMLET_VMPL1_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ 第一层：Runtime 生命周期 ============ */

/**
 * @brief 初始化 WAMR runtime（+ MPK 分配器）
 *
 * 调用链：
 *   pal_heap_init()             → 初始化全局堆（dlmalloc mspace）
 *   mpk_allocator_init()        → 初始化 MPK 分配器（非 MPK 模式为 no-op）
 *   RegisterMpkAllocatorForWAMR → 注册自定义分配器到 WAMR
 *   wasm_runtime_full_init()    → 初始化 WAMR 解释器
 *
 * @return 0 成功，-1 失败
 */
int wasmlet_runtime_init(void);

/**
 * @brief 销毁 WAMR runtime
 *
 * 调用链：
 *   wasm_runtime_destroy()      → 销毁 WAMR
 *   mpk_allocator_destroy()     → 销毁 MPK 分配器
 */
void wasmlet_runtime_destroy(void);

/* ============ 第二层：Module 生命周期 ============ */

/**
 * @brief 加载 WASM 模块（持久）
 *
 * 调用 wasm_runtime_load() 解析并验证字节码，生成内部数据结构。
 * 模块加载后常驻内存，可多次调用 wasmlet_invoke()。
 *
 * MPK 模式下：为模块分配 pkey，模块内存受 pkey 保护。
 *
 * @param wasm_buf  WASM 字节码缓冲区
 * @param wasm_size 字节码大小
 * @return 0 成功，-1 失败
 */
int wasmlet_load_module(const uint8_t *wasm_buf, uint32_t wasm_size);

/**
 * @brief 卸载 WASM 模块
 *
 * 调用 wasm_runtime_unload() 释放模块内存。
 * MPK 模式下：释放模块的 pkey。
 */
void wasmlet_unload_module(void);

/* ============ 第三层：Invocation 生命周期 ============ */

/**
 * @brief 实例化模块 + 执行函数 + 销毁实例
 *
 * 内部流程：
 *   1. wasm_runtime_instantiate()       → 创建独立的线性内存
 *   2. wasm_runtime_create_exec_env()   → 创建执行环境
 *   3. wasm_runtime_lookup_function()   → 查找函数
 *   4. wasm_runtime_call_wasm()         → 执行函数
 *   5. wasm_runtime_destroy_exec_env()  → 销毁执行环境
 *   6. wasm_runtime_deinstantiate()     → 销毁实例（释放线性内存）
 *
 * 每次调用后实例的线性内存和执行环境都会被销毁，
 * 但模块本身保留，可再次调用 wasmlet_invoke()。
 *
 * @param func_name 函数名
 * @param argc      参数个数
 * @param argv      参数数组（WAMR 将返回值写回 argv[0]）
 * @param result    输出：函数返回值（可为 NULL）
 * @return 0 成功，-1 失败
 */
int wasmlet_invoke(const char *func_name, int argc,
                   uint32_t *argv, uint32_t *result);

#ifdef __cplusplus
}
#endif

#endif /* WASMLET_VMPL1_H */
