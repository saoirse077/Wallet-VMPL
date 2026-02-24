/*
 * platform_internal.h - WAMR platform layer for bare-metal VMPL1
 *
 * This header is the first file included by WAMR's platform_common.h.
 * It must provide:
 *   1. All types WAMR expects (korp_mutex, korp_cond, korp_thread, etc.)
 *   2. All standard-library-like declarations WAMR uses
 *   3. Macros to disable features unavailable in bare-metal
 *
 * We replace the SGX platform's <inttypes.h>, <string.h>, <stdio.h>,
 * <stdlib.h>, <math.h>, <pthread.h>, etc. with our own freestanding
 * equivalents from pal_string.h / pal_malloc.h / pal_spinlock.h.
 */
/*
 * platform_internal.h - 面向裸机 VMPL1 的 WAMR 平台适配层
 *
 * 这个头文件是 WAMR 的 platform_common.h 首先包含的文件。
 * 它必须提供：
 *   1. WAMR 所期望的所有类型定义（如 korp_mutex、korp_cond、korp_thread 等）
 *   2. WAMR 使用到的所有类似标准库的函数声明
 *   3. 用于禁用裸机环境中不可用功能的宏定义
 *
 * 我们使用自己实现的 freestanding 版本
 * （来自 pal_string.h / pal_malloc.h / pal_spinlock.h）
 * 来替代 SGX 平台中的 <inttypes.h>、<string.h>、<stdio.h>、
 * <stdlib.h>、<math.h>、<pthread.h> 等标准头文件。
 */
 
#ifndef _PLATFORM_INTERNAL_H
#define _PLATFORM_INTERNAL_H

/* ================================================================
 * 1. GCC freestanding headers (the ONLY system headers we use)
 * ================================================================ */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* ================================================================
 * 2. limits.h replacements
 *
 * GCC's freestanding <limits.h> does #include_next to find the
 * system limits.h, which doesn't exist with -nostdinc.
 * We define the constants WAMR needs manually.
 * ================================================================ */
#ifndef CHAR_BIT
#define CHAR_BIT    8
#endif
#ifndef SCHAR_MAX
#define SCHAR_MAX   127
#endif
#ifndef UCHAR_MAX
#define UCHAR_MAX   255
#endif
#ifndef SHRT_MAX
#define SHRT_MAX    32767
#endif
#ifndef USHRT_MAX
#define USHRT_MAX   65535
#endif
#ifndef INT_MAX
#define INT_MAX     0x7FFFFFFF
#endif
#ifndef INT_MIN
#define INT_MIN     (-INT_MAX - 1)
#endif
#ifndef UINT_MAX
#define UINT_MAX    0xFFFFFFFFU
#endif
#ifndef LONG_MAX
#define LONG_MAX    0x7FFFFFFFFFFFFFFFL
#endif
#ifndef LONG_MIN
#define LONG_MIN    (-LONG_MAX - 1L)
#endif
#ifndef ULONG_MAX
#define ULONG_MAX   0xFFFFFFFFFFFFFFFFUL
#endif
#ifndef LLONG_MAX
#define LLONG_MAX   0x7FFFFFFFFFFFFFFFll
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX  0xFFFFFFFFFFFFFFFFull
#endif

/* ================================================================
 * 3. Our PAL headers (libc stubs, heap, spinlock)
 * ================================================================ */
#include "../../pal_string.h"
#include "../../pal_malloc.h"
#include "../../pal_spinlock.h"

/* ================================================================
 * 4. Platform identification
 * ================================================================ */
#ifndef BH_PLATFORM_VMPL1
#define BH_PLATFORM_VMPL1
#endif

/* ================================================================
 * 5. WAMR kernel type definitions
 *
 * WAMR's platform layer requires these typedefs:
 *   korp_mutex  — mutex type (we use a uint32 flag for spinlock)
 *   korp_cond   — condition variable (stub, not used in single-thread)
 *   korp_thread / korp_tid — thread ID type
 *   korp_rwlock — read-write lock (spinlock-based)
 *   korp_sem    — semaphore (stub)
 * ================================================================ */
typedef pal_spinlock_t korp_mutex;
typedef unsigned int   korp_cond;
typedef unsigned long  korp_tid;
typedef unsigned long  korp_thread;
typedef pal_spinlock_t korp_rwlock;
typedef unsigned int   korp_sem;

/* ================================================================
 * 6. File handle types (stubs — no filesystem in VMPL1)
 * ================================================================ */
typedef int   os_file_handle;
typedef void *os_dir_stream;
typedef int   os_raw_file_handle;

/* Poll types (stubs — no poll/select in VMPL1) */
struct _vmpl1_pollfd {
    int fd;
    short events;
    short revents;
};
typedef struct _vmpl1_pollfd os_poll_file_handle;
typedef unsigned long os_nfds_t;

static inline os_file_handle
os_get_invalid_handle(void)
{
    return -1;
}

/* ================================================================
 * 7. Feature control macros
 * ================================================================ */

/* Disable dynamic loading (no dlopen in bare-metal) */
#define BH_HAS_DLFCN 0

/* Disable WASI (no filesystem/sockets) */
#define SGX_DISABLE_WASI
/* Disable pthread (no OS threads) */
#define SGX_DISABLE_PTHREAD

/* Page size */
#define os_getpagesize() 4096
#define getpagesize()    4096

/* Stack size adjustment */
#define _STACK_SIZE_ADJUSTMENT (32 * 1024)
#define BH_APPLET_PRESERVED_STACK_SIZE (8 * 1024 + _STACK_SIZE_ADJUSTMENT)
#define BH_THREAD_DEFAULT_PRIORITY 0

/* Atomic memory ordering (GCC builtins) */
#define os_memory_order_acquire __ATOMIC_ACQUIRE
#define os_memory_order_release __ATOMIC_RELEASE
#define os_memory_order_seq_cst __ATOMIC_SEQ_CST
#define os_atomic_thread_fence  __atomic_thread_fence

/* ================================================================
 * 8. Print function callback type
 * ================================================================ */
typedef int (*os_print_function_t)(const char *message);
void os_set_print_function(os_print_function_t pf);

/* ================================================================
 * 9. assert() replacement
 *
 * WAMR uses assert() in some places. In freestanding mode we
 * provide a simple macro that calls our abort().
 * ================================================================ */
#ifndef assert
#define assert(expr)                                              \
    do {                                                          \
        if (!(expr)) {                                            \
            extern void pal_svsm_debug_print(const char *);      \
            pal_svsm_debug_print("[ASSERT FAIL] " #expr "\n");   \
            abort();                                              \
        }                                                         \
    } while (0)
#endif

/* ================================================================
 * 10. Disable write to GS base (bare-metal, no segmentation setup)
 * ================================================================ */
#ifndef WASM_DISABLE_WRITE_GS_BASE
#define WASM_DISABLE_WRITE_GS_BASE 1
#endif

#endif /* end of _PLATFORM_INTERNAL_H */
