/*
 * vmpl1_time.c - WAMR time APIs for bare-metal VMPL1
 *
 * In bare-metal VMPL1, we have no clock source.
 * Both time functions return 0.
 *
 * If precise timing is needed in the future, we could:
 *   - Use RDTSC instruction (x86-64 timestamp counter)
 *   - Add a CPUID trap to read VMPL0's clock
 */
 /*
 * vmpl1_time.c - 面向裸机 VMPL1 的 WAMR 时间相关接口实现
 *
 * 在裸机 VMPL1 环境中，我们没有可用的时钟源。
 * 因此这两个时间相关函数都直接返回 0。
 *
 * 如果将来需要精确计时，可以考虑：
 *   - 使用 RDTSC 指令（x86-64 的时间戳计数器）
 *   - 增加一个 CPUID trap，通过 VMPL0 读取其时钟
 */

#include "platform_api_vmcore.h"

uint64
os_time_get_boot_us(void)
{
    /*
     * Return a monotonically increasing counter based on RDTSC.
     * This is approximate but sufficient for WAMR's internal use
     * (timeout comparisons, profiling timestamps).
     *
     * Assumption: ~2 GHz TSC frequency → 1 us ≈ 2000 ticks
     * This doesn't need to be precise for Phase 2.
     */
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint64 tsc = ((uint64)hi << 32) | lo;
    /* Rough conversion: divide by ~2000 to get microseconds.
     * Use shift approximation: >> 11 ≈ /2048 ≈ /2000 */
    return tsc >> 11;
}

uint64
os_time_thread_cputime_us(void)
{
    /* Same as boot time for single-thread */
    return os_time_get_boot_us();
}
