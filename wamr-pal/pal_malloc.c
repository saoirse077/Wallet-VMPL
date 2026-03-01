/*
 * pal_malloc.c - Heap allocator for bare-metal VMPL1
 *
 * Architecture:
 *
 *   pal_heap_init()
 *       |
 *       v
 *   pal_svsm_virt_alloc(HEAP_BASE, HEAP_SIZE)   <-- CPUID trap to VMPL0
 *       |                                             allocates physical pages
 *       v
 *   create_mspace_with_base(HEAP_BASE, HEAP_SIZE)  <-- dlmalloc creates mspace
 *       |
 *       v
 *   pal_malloc/free/realloc  -->  mspace_malloc/free/realloc
 *
 * The heap lives at a fixed virtual address (0x40_0000_0000) to avoid
 * conflicts with the ELF image, mmap region, and memory channels.
 *
 * See address space layout:
 *   0x0000_0000_0000  ELF code/data
 *   0x0040_0000_0000  Heap (this allocator, 16 MB)
 *   0x0050_0000_0000  os_mmap region (vmpl1_mmap.c)
 *   0x0280_0000_0000  Input Channel (SVSM fixed)
 *   0x0300_0000_0000  Output Channel (SVSM fixed)
 */

#include "pal_malloc.h"
#include "pal_monitor_call.h"
#include "pal_string.h"

/* dlmalloc mspace API — defined in wasmlet/third_party/dlmalloc/malloc.h */
typedef void *mspace;
extern mspace create_mspace_with_base(void *base, size_t capacity, int locked);
extern void  *mspace_malloc(mspace msp, size_t bytes);
extern void   mspace_free(mspace msp, void *mem);
extern void  *mspace_realloc(mspace msp, void *mem, size_t newsize);
extern void  *mspace_calloc(mspace msp, size_t n_elements, size_t elem_size);

/* ========== Configuration ========== */

/*
 * Heap base address: 0x40_0000_0000 (256 GB)
 * Well above the ELF image (loaded near 0x0) and below the mmap region.
 */
#define HEAP_BASE_ADDR  0x4000000000ULL

/*
 * Heap size: 16 MB
 * Enough for WAMR runtime initialization + loading a small wasm module.
 * Can be increased if needed.
 */
#define HEAP_SIZE       (16ULL * 1024 * 1024)

/*
 * Protection flags for pal_svsm_virt_alloc:
 * 0x3 = Read + Write (no Execute needed for heap)
 */
#define HEAP_PROT_FLAGS 0x3

/* ========== Global state ========== */

static mspace g_mspace = (void *)0;
static int    g_heap_initialized = 0;

/* ========== Implementation ========== */

int pal_heap_init(void)
{
    int ret;

    if (g_heap_initialized)
        return 0;

    pal_svsm_debug_print("[WAMR-PAL] Initializing heap at 0x");
    pal_svsm_debug_print_hex(HEAP_BASE_ADDR);
    pal_svsm_debug_print(" size=");
    pal_svsm_debug_print_dec((int)(HEAP_SIZE / (1024 * 1024)));
    pal_svsm_debug_print(" MB\n");

    /* Ask VMPL0 to allocate physical pages and map them */
    ret = pal_svsm_virt_alloc((void *)HEAP_BASE_ADDR, HEAP_SIZE, HEAP_PROT_FLAGS);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: heap virt_alloc failed, error=");
        pal_svsm_debug_print_dec(ret);
        pal_svsm_debug_print("\n");
        return -1;
    }

    /* Zero the memory (VMPL0 may or may not zero it) */
    memset((void *)HEAP_BASE_ADDR, 0, HEAP_SIZE);

    /* Create dlmalloc mspace on top of the allocated memory */
    g_mspace = create_mspace_with_base((void *)HEAP_BASE_ADDR, HEAP_SIZE, 1);
    if (!g_mspace) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: create_mspace_with_base failed\n");
        return -1;
    }

    g_heap_initialized = 1;
    pal_svsm_debug_print("[WAMR-PAL] Heap initialized successfully (16 MB)\n");
    return 0;
}

void *pal_malloc(size_t size)
{
    if (!g_heap_initialized || !g_mspace)
        return (void *)0;
    return mspace_malloc(g_mspace, size);
}

void pal_free(void *ptr)
{
    if (!g_heap_initialized || !g_mspace || !ptr)
        return;
    mspace_free(g_mspace, ptr);
}

void *pal_realloc(void *ptr, size_t size)
{
    if (!g_heap_initialized || !g_mspace)
        return (void *)0;
    return mspace_realloc(g_mspace, ptr, size);
}

void *pal_calloc(size_t nmemb, size_t size)
{
    if (!g_heap_initialized || !g_mspace)
        return (void *)0;
    return mspace_calloc(g_mspace, nmemb, size);
}
