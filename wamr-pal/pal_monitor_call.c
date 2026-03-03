/*
 * pal_monitor_call.c - VMPL1 ↔ VMPL0 CPUID 陷入实现
 *
 * WAMR PAL 独立版本（无 Gramine 依赖）。
 * 基于 gramine-svsm/pal/src/host/svsm/pal_monitor_call.c 改写。
 *
 * 所有函数通过 CPUID 指令陷入 VMPL0 Monitor。
 * Monitor 读取内联汇编设置的 VMSA 寄存器。
 */

#include "pal_monitor_call.h"

#define vc_injection "cpuid"

/* ========== 底层 CPUID 陷入 ========== */

void monitor_call(struct monitor_call_data* data) {
    __asm__ volatile(
        vc_injection
        : "+a" (data->rax), "+b" (data->rbx),
          "+c" (data->rcx), "+d" (data->rdx)
        );
}

void extended_monitor_call(struct monitor_call_data* data) {
    __asm__ volatile(
        "mov %4, %%r8\r\n"
        "mov %5, %%r9\r\n"
        vc_injection
        : "+a" (data->rax), "+b" (data->rbx),
          "+c" (data->rcx), "+d" (data->rdx)
        : "r" (data->r8), "r" (data->r9)
        : "r8", "r9"
        );
}

/* ========== 错误报告与退出 ========== */

void pal_svsm_fail(const char* err, int err_no) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFFF;
    data.rbx = (uint64_t)err;
    data.rcx = (uint64_t)err_no;
    data.rdx = 0;
    monitor_call(&data);
}

void pal_svsm_exit(int exitcode) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFFE;
    data.rbx = (uint64_t)exitcode;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
}

/* ========== 调试输出 ========== */

void pal_svsm_debug_putc(char c) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFFD;
    data.rbx = (uint64_t)c;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
}

void pal_svsm_debug_print(const char* str) {
    if (!str) return;
    while (*str) {
        pal_svsm_debug_putc(*str);
        str++;
    }
    /* 发送 '\0' 触发 Monitor 刷新日志行 */
    pal_svsm_debug_putc('\0');
}

void pal_svsm_debug_print_hex(uint64_t val) {
    static const char hex_chars[] = "0123456789abcdef";
    char buf[19];
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 16; i++) {
        buf[2 + i] = hex_chars[(val >> (60 - i * 4)) & 0xF];
    }
    buf[18] = '\0';
    pal_svsm_debug_print(buf);
}

void pal_svsm_debug_print_dec(int val) {
    char buf[12];
    int i = 10;
    int neg = 0;
    unsigned int uval;
    buf[11] = '\0';
    if (val < 0) {
        neg = 1;
        uval = (unsigned int)(-(val + 1)) + 1;
    } else {
        uval = (unsigned int)val;
    }
    if (uval == 0) {
        pal_svsm_debug_print("0");
        return;
    }
    while (uval > 0) {
        buf[i--] = '0' + (uval % 10);
        uval /= 10;
    }
    if (neg) {
        buf[i--] = '-';
    }
    pal_svsm_debug_print(&buf[i + 1]);
}

/* ========== 内存管理 ========== */

int pal_svsm_virt_alloc(void* addr, uint64_t size, uint64_t flags) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFFC;
    data.rbx = (uint64_t)addr;
    data.rcx = size;
    data.rdx = flags;
    monitor_call(&data);
    return (int)data.rcx;
}

int pal_svsm_free(void* addr, uint64_t size) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF6;
    data.rbx = (uint64_t)addr;
    data.rcx = size;
    data.rdx = 0;
    monitor_call(&data);
    return (int)data.rcx;
}

/* ========== MPK 六接口 ========== */

int pal_svsm_mpk_pkey_alloc(void) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF1;
    data.rbx = 0;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    if (data.rcx != 0) {
        return -(int)data.rcx;  /* 失败: 返回负数错误码 */
    }
    return (int)data.rax;       /* 成功: 返回 pkey (1-15) */
}

int pal_svsm_mpk_alloc(void* addr, uint64_t size, uint32_t pkey) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF3;
    data.rbx = (uint64_t)addr;
    data.rcx = size;
    data.rdx = (uint64_t)pkey;
    monitor_call(&data);
    return (int)data.rcx;
}

int pal_svsm_mpk_enter_domain(uint32_t pkey) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF0;
    data.rbx = (uint64_t)pkey;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    return (int)data.rcx;
}

int pal_svsm_mpk_exit_domain(uint32_t pkey) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFEF;
    data.rbx = (uint64_t)pkey;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    return (int)data.rcx;
}

int pal_svsm_mpk_free(void* addr, uint64_t size) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF2;
    data.rbx = (uint64_t)addr;
    data.rcx = size;
    data.rdx = 0;
    monitor_call(&data);
    return (int)data.rcx;
}

int pal_svsm_mpk_free_pkey(uint32_t pkey, void* addr, uint64_t size) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFEE;
    data.rbx = (uint64_t)pkey;
    data.rcx = (uint64_t)addr;
    data.rdx = size;
    monitor_call(&data);
    return (int)data.rcx;
}

uint32_t pal_svsm_mpk_query_pkru(void) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFED;
    data.rbx = 0;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    return (uint32_t)data.rax;
}

/* ========== Trustlet 结果通知 ========== */

void pal_svsm_get_result(void) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF8;
    data.rbx = 0;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    /* 返回后，VMPL1 已被下一次 invoke_trustlet 唤醒
     *（若无后续调用则永不返回）。 */
}

/* ========== 线程容量查询 ========== */

uint64_t pal_svsm_query_thread_capacity(void) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFE9;
    data.rbx = 0;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    return data.rax;
}

/* ========== 内存通道管理 ========== */

int pal_svsm_inflate_channel(int select, uint64_t size) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFA3;
    data.rbx = 0;
    data.rcx = (uint64_t)select;
    data.rdx = size;
    monitor_call(&data);
    return 0;
}

/* ========== 线程管理 ========== */

uint64_t pal_svsm_thread_create(uint64_t entry_rip, uint64_t stack_top,
                                 uint64_t gs_base, uint64_t arg) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFEC;
    data.rbx = entry_rip;
    data.rcx = stack_top;
    data.rdx = gs_base;
    data.r8  = arg;
    data.r9  = 0;
    extended_monitor_call(&data);
    return data.rax;
}

uint64_t pal_svsm_thread_join(uint64_t tid) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFEB;
    data.rbx = tid;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    return data.rax;
}

void pal_svsm_thread_exit(uint64_t exit_code) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFEA;
    data.rbx = exit_code;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
}
