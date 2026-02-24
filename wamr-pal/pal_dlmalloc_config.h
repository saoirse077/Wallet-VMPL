/*
 * pal_dlmalloc_config.h - Freestanding environment shims for dlmalloc
 *
 * This header is force-included (-include) when compiling dlmalloc in our
 * bare-metal VMPL1 environment. It provides the minimal set of declarations
 * and definitions that dlmalloc expects from system headers but that are
 * unavailable in a freestanding build.
 */
/*
 * pal_dlmalloc_config.h - 面向 freestanding 环境的 dlmalloc 适配层
 *
 * 在我们的裸机 VMPL1 环境中编译 dlmalloc 时，
 * 该头文件会通过 -include 选项被强制包含。
 * 它提供了 dlmalloc 期望从系统头文件中获得的最小声明和定义，
 * 而这些内容在 freestanding 编译环境中是不可用的。
 */
#ifndef WAMR_PAL_DLMALLOC_CONFIG_H_
#define WAMR_PAL_DLMALLOC_CONFIG_H_

/* ---- size_t, ptrdiff_t, NULL ---- */
#include <stddef.h>
#include <stdint.h>

/* ---- memset / memcpy / memmove declarations ----
 * dlmalloc calls these directly even with LACKS_STRING_H.
 * Our implementations are in pal_string.c.
 */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);

/* ---- errno values ----
 * dlmalloc's posix_memalign uses EINVAL and ENOMEM.
 * We define them to standard Linux values.
 */
#define EINVAL 22
#define ENOMEM 12

/* ---- pal_svsm_exit declaration (for ABORT macro) ---- */
void pal_svsm_exit(int exitcode);

#endif /* WAMR_PAL_DLMALLOC_CONFIG_H_ */
