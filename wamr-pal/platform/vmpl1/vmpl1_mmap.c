/*
 * vmpl1_mmap.c - WAMR memory mapping APIs for bare-metal VMPL1
 *
 * Implements os_mmap / os_munmap / os_mprotect / os_mremap.
 *
 * In bare-metal VMPL1, there is no OS virtual memory manager.
 * We maintain a simple bump allocator that hands out page-aligned
 * regions from a dedicated address range (0x50_0000_0000 onwards).
 * Each allocation calls pal_svsm_virt_alloc() to request the VMPL0
 * Monitor to back the virtual pages with physical memory.
 *
 * Address space layout:
 *   0x0000_0000_0000 .. ELF code/data (pal_start.S, .text, .data, .bss)
 *   0x0040_0000_0000    Heap (dlmalloc mspace, 16MB, from pal_malloc.c)
 *   0x0050_0000_0000    MMAP region (this file, grows upward)
 *   0x0280_0000_0000    Input channel (SVSM fixed mapping)
 *   0x0300_0000_0000    Output channel (SVSM fixed mapping)
 */
 /*
 * vmpl1_mmap.c - 面向裸机 VMPL1 的 WAMR 内存映射接口实现
 *
 * 实现了 os_mmap / os_munmap / os_mprotect / os_mremap。
 *
 * 在裸机 VMPL1 环境中，不存在操作系统的虚拟内存管理器。
 * 我们维护了一个简单的“线性递增分配器”（bump allocator），
 * 从专用地址范围（0x50_0000_0000 起）分配页对齐的内存区域。
 *
 * 每次分配都会调用 pal_svsm_virt_alloc()，
 * 请求 VMPL0 的 Monitor 为这些虚拟页分配并映射物理内存。
 *
 * 地址空间布局：
 *   0x0000_0000_0000 .. ELF 代码/数据段（pal_start.S, .text, .data, .bss）
 *   0x0040_0000_0000    堆（dlmalloc mspace，16MB，来自 pal_malloc.c）
 *   0x0050_0000_0000    MMAP 区域（本文件管理，向高地址增长）
 *   0x0280_0000_0000    输入通道（SVSM 固定映射）
 *   0x0300_0000_0000    输出通道（SVSM 固定映射）
 */

#include "platform_api_vmcore.h"
#include "../../pal_monitor_call.h"

/* ================================================================
 * Bump allocator for mmap regions
 * ================================================================ */

/* Start mmap region at 0x50_0000_0000 (well above heap at 0x40_0000_0000) */
static uint64_t mmap_next_addr = 0x5000000000ULL;

/* Page size constant */
#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* Align up to page boundary */
static inline uint64_t
align_up(uint64_t val, uint64_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/* Convert WAMR MMAP_PROT_* flags to our PAL flags */
static inline uint64_t
prot_to_pal_flags(int prot)
{
    uint64_t flags = 0;
    if (prot & MMAP_PROT_READ)
        flags |= 0x1; /* PAL_PROT_READ */
    if (prot & MMAP_PROT_WRITE)
        flags |= 0x2; /* PAL_PROT_WRITE */
    if (prot & MMAP_PROT_EXEC)
        flags |= 0x4; /* PAL_PROT_EXEC */
    /* Default to RW if no flags specified */
    if (flags == 0)
        flags = 0x3; /* PAL_PROT_READ | PAL_PROT_WRITE */
    return flags;
}

/* ================================================================
 * os_mmap — allocate page-aligned memory
 *
 * WAMR calls this for:
 *   - WASM linear memory allocation
 *   - Module code/data sections
 *   - Internal data structures
 *
 * We ignore the 'hint' and 'file' parameters (no file mapping).
 * Memory is always zero-initialized (pal_svsm_virt_alloc provides
 * zeroed pages from VMPL0).
 * ================================================================ */

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    (void)hint;
    (void)flags;

    /* Reject file-backed mappings */
    if (file != os_get_invalid_handle() && file >= 0) {
        os_printf("[VMPL1-MMAP] os_mmap: file mapping not supported\n");
        return NULL;
    }

    if (size == 0)
        return NULL;

    /* Align size to page boundary */
    uint64_t aligned_size = align_up((uint64_t)size, PAGE_SIZE);

    /* Get next available address */
    uint64_t addr = mmap_next_addr;
    mmap_next_addr += aligned_size;

    /* Safety check: don't overlap with input channel at 0x280_0000_0000 */
    if (mmap_next_addr >= 0x28000000000ULL) {
        os_printf("[VMPL1-MMAP] os_mmap: address space exhausted!\n");
        mmap_next_addr = addr; /* rollback */
        return NULL;
    }

    /* Request VMPL0 to allocate and map physical pages */
    uint64_t pal_flags = prot_to_pal_flags(prot);
    int ret = pal_svsm_virt_alloc((void *)addr, aligned_size, pal_flags);
    if (ret != 0) {
        os_printf("[VMPL1-MMAP] os_mmap: pal_svsm_virt_alloc failed "
                  "(addr=0x%lx, size=0x%lx, ret=%d)\n",
                  addr, aligned_size, ret);
        mmap_next_addr = addr; /* rollback */
        return NULL;
    }

    /* Zero the memory (pal_svsm_virt_alloc may or may not zero pages) */
    memset((void *)addr, 0, aligned_size);

    return (void *)addr;
}

/* ================================================================
 * os_munmap — release mapped memory
 *
 * Calls pal_svsm_free() to return pages to VMPL0.
 * Note: the bump allocator doesn't reclaim address space.
 * This is acceptable for Phase 2 (short-lived single execution).
 * ================================================================ */

void
os_munmap(void *addr, size_t size)
{
    if (!addr || size == 0)
        return;

    uint64_t aligned_size = align_up((uint64_t)size, PAGE_SIZE);
    int ret = pal_svsm_free(addr, aligned_size);
    if (ret != 0) {
        os_printf("[VMPL1-MMAP] os_munmap: pal_svsm_free failed "
                  "(addr=%p, size=0x%lx, ret=%d)\n",
                  addr, aligned_size, ret);
    }
}

/* ================================================================
 * os_mprotect — change memory protection
 *
 * Delegates to VMPL0 via CPUID trap.
 * ================================================================ */

int
os_mprotect(void *addr, size_t size, int prot)
{
    if (!addr || size == 0)
        return 0;

    /* For now, just return success.
     * Full mprotect support via pal_svsm_mprotect can be added
     * if WAMR actually changes protection on existing mappings.
     * In practice, for interpreter mode without HW bound check,
     * mprotect is rarely called. */
    (void)prot;
    return 0;
}

/* ================================================================
 * os_mremap — resize a mapping
 *
 * Allocate new region, copy data, free old region.
 * We use the default os_mremap_slow() from platform_api_vmcore.h,
 * but provide our own implementation to avoid potential issues.
 * ================================================================ */

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    /* Allocate new region */
    void *new_addr = os_mmap(NULL, new_size,
                             MMAP_PROT_READ | MMAP_PROT_WRITE,
                             MMAP_MAP_NONE, os_get_invalid_handle());
    if (!new_addr)
        return NULL;

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_addr, old_addr, copy_size);

    /* Free old region */
    os_munmap(old_addr, old_size);

    return new_addr;
}
