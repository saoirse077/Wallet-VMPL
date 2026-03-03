/*
 * pal_monitor_call.h - VMPL1 ↔ VMPL0 CPUID 陷入接口
 *
 * WAMR PAL 独立版本（无 Gramine 依赖）。
 * 基于 gramine-svsm/pal/src/host/svsm/pal_monitor_call.h 改写。
 *
 * 与 VMPL0 Monitor 的所有通信均通过 CPUID 指令陷入完成。
 * Monitor 截获 CPUID 并读写 VMSA 寄存器。
 */

#ifndef WAMR_PAL_MONITOR_CALL_H_
#define WAMR_PAL_MONITOR_CALL_H_

#include <stdint.h>
#include <stddef.h>

/* ========== CPUID 陷入数据结构 ========== */

struct monitor_call_data {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t r8;
    uint64_t r9;
};

/* ========== 底层 CPUID 陷入函数 ========== */

/* 基本 CPUID 陷入（使用 rax, rbx, rcx, rdx） */
void monitor_call(struct monitor_call_data* data);

/* 扩展 CPUID 陷入（额外使用 r8, r9） */
void extended_monitor_call(struct monitor_call_data* data);

/* ========== 基础 PAL 服务 ========== */

/*
 * 调试输出 - 向 SVSM 串口发送单个字符
 * 调用号: 0x4FFFFFFD
 * 发送 '\0' 可刷新 Monitor 中的行缓冲区。
 */
void pal_svsm_debug_putc(char c);

/*
 * 调试输出 - 发送以 null 结尾的字符串
 * 逐字符通过 debug_putc 发送，最后发送 '\0' 以刷新缓冲区。
 */
void pal_svsm_debug_print(const char* str);

/* 调试输出 - 以十六进制格式打印 uint64（如 "0x0000000012345678"） */
void pal_svsm_debug_print_hex(uint64_t val);

/* 调试输出 - 以十进制格式打印 int */
void pal_svsm_debug_print_dec(int val);

/*
 * 退出 VMPL1 执行
 * 调用号: 0x4FFFFFFE
 * 使 handle_process_request() 返回 false，从而跳出
 * early_invoke()/invoke_trustlet() 中的 ap_create 循环。
 */
void pal_svsm_exit(int exitcode);

/*
 * 报告错误并退出
 * 调用号: 0x4FFFFFFF
 */
void pal_svsm_fail(const char* err, int err_no);

/* ========== 内存管理 ========== */

/*
 * 分配虚拟内存（由物理页支撑）
 * 调用号: 0x4FFFFFFC
 * 参数:
 *   addr  - 虚拟地址（必须页对齐）
 *   size  - 分配大小（字节，必须页对齐）
 *   flags - 保护标志（0x7 = RWX）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_virt_alloc(void* addr, uint64_t size, uint64_t flags);

/*
 * 释放虚拟内存
 * 调用号: 0x4FFFFFF6
 * 参数:
 *   addr - 虚拟地址（须与先前 alloc 匹配）
 *   size - 大小（须与先前 alloc 匹配）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_free(void* addr, uint64_t size);

/* ========== MPK 六接口（由 VMPL0 管理）========== */

/*
 * 接口 1: 分配 protection key (pkey)
 * 调用号: 0x4FFFFFF1
 * 返回: pkey (1-15) 成功，负数失败
 */
int pal_svsm_mpk_pkey_alloc(void);

/*
 * 接口 2: 分配带 pkey 标记的内存
 * 调用号: 0x4FFFFFF3
 * 参数:
 *   addr - 虚拟地址（必须页对齐）
 *   size - 分配大小（必须页对齐）
 *   pkey - 保护键（1-15，来自 pkey_alloc）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_mpk_alloc(void* addr, uint64_t size, uint32_t pkey);

/*
 * 接口 3: 进入安全域（在 PKRU 中启用 pkey 的读写权限）
 * 调用号: 0x4FFFFFF0
 * 参数:
 *   pkey - 要启用的保护键（1-15）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_mpk_enter_domain(uint32_t pkey);

/*
 * 接口 4: 退出安全域（在 PKRU 中禁用 pkey）
 * 调用号: 0x4FFFFFEF
 * 参数:
 *   pkey - 要禁用的保护键（1-15）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_mpk_exit_domain(uint32_t pkey);

/*
 * 接口 5: 释放内存（不释放 pkey）
 * 调用号: 0x4FFFFFF2
 * 参数:
 *   addr - 虚拟地址
 *   size - 分配大小
 * 返回: 0 成功，非零失败
 */
int pal_svsm_mpk_free(void* addr, uint64_t size);

/*
 * 接口 6: 释放 pkey（可选同时释放内存）
 * 调用号: 0x4FFFFFEE
 * 参数:
 *   pkey - 要释放的保护键（1-15）
 *   addr - 虚拟地址（0 = 不释放内存）
 *   size - 大小（0 = 不释放内存）
 * 返回: 0 成功，非零失败
 */
int pal_svsm_mpk_free_pkey(uint32_t pkey, void* addr, uint64_t size);

/*
 * 辅助: 查询当前 VMSA.pkru 值（调试用）
 * 调用号: 0x4FFFFFED
 * 返回: 当前 PKRU 值
 */
uint32_t pal_svsm_mpk_query_pkru(void);

/* ========== Trustlet 结果通知 ========== */

/*
 * 通知 SVSM 输出通道中的结果已就绪。
 * 调用号: 0x4FFFFFF8
 *
 * 当 VMPL1 在 invoke_trustlet 期间调用此函数时，SVSM 将：
 *   1. 将输出通道数据拷贝到 Guest 的返回缓冲区
 *     （通过 copy_out 到 guest 页表中的 result_addr）
 *   2. 设置 return_value = GETRESULT (1)
 *   3. 跳出 ap_create 循环（返回 false）
 *   4. Guest 的 ioctl 返回 invocationGetValue (1)
 *
 * 调用返回后，VMPL1 暂停直到 Guest 发起下一次 invoke_trustlet。
 */
void pal_svsm_get_result(void);

/* ========== 线程管理（由 VMPL0 管理）========== */

/*
 * 创建线程（分配 VMSA，执行由 AP 自行启动）
 * 调用号: 0x4FFFFFEC
 * 参数:
 *   entry_rip  - 线程入口函数地址
 *   stack_top  - 预分配栈的栈顶（向下增长）
 *   gs_base    - TLS 的 GS 段基址（TCB 地址）
 *   arg        - 通过 RDI 传递的单个 uint64_t 参数
 * 返回: 线程 ID (0..7)，或 UINT64_MAX 表示失败
 */
uint64_t pal_svsm_thread_create(uint64_t entry_rip, uint64_t stack_top,
                                 uint64_t gs_base, uint64_t arg);

/*
 * 等待线程结束（阻塞直到线程调用 thread_exit）
 * 调用号: 0x4FFFFFEB
 * 参数:
 *   tid - thread_create 返回的线程 ID
 * 返回: 线程的退出码
 */
uint64_t pal_svsm_thread_join(uint64_t tid);

/*
 * 退出当前线程（在线程内部调用）
 * 调用号: 0x4FFFFFEA
 * 参数:
 *   exit_code - 返回给 joiner 的退出码
 * 不返回。
 */
void pal_svsm_thread_exit(uint64_t exit_code);

/* ========== 线程容量查询 ========== */

/*
 * 查询可用的线程 runner 容量（用于线程的 AP 数量）
 * 调用号: 0x4FFFFFE9
 * 返回: 可用的 thread runner 数量
 */
uint64_t pal_svsm_query_thread_capacity(void);

/* ========== 内存通道管理 ========== */

/*
 * 膨胀（扩展）内存通道
 * 调用号: 0x4FFFFFA3
 *
 * 请求 VMPL0 分配物理页并映射到 VMPL1 页表中通道的固定虚拟地址。
 *
 * 参数:
 *   select - 0 = 膨胀输入通道 (0x280_0000_0000)
 *            1 = 膨胀输出通道 (0x300_0000_0000)
 *   size   - 字节大小（将向上取整到页边界）
 * 返回: 0 成功
 */
int pal_svsm_inflate_channel(int select, uint64_t size);

#endif /* WAMR_PAL_MONITOR_CALL_H_ */
