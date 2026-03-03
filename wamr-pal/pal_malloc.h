/*
 * pal_malloc.h - Heap allocator for bare-metal VMPL1
 *
 * Uses pal_svsm_virt_alloc (CPUID trap) to get a large chunk of memory
 * from VMPL0, then runs dlmalloc's mspace on top of it.
 *
 * Call pal_heap_init() once at startup before any malloc/free.
 */
#ifndef PAL_MALLOC_H
#define PAL_MALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the global heap.
 * Allocates HEAP_SIZE bytes via pal_svsm_virt_alloc at HEAP_BASE_ADDR,
 * then creates a dlmalloc mspace on that memory.
 *
 * Returns: 0 on success, -1 on failure.
 */
int pal_heap_init(void);

/* Standard allocator interface (backed by dlmalloc mspace) */
void *pal_malloc(size_t size);
void  pal_free(void *ptr);
void *pal_realloc(void *ptr, size_t size);
void *pal_calloc(size_t nmemb, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PAL_MALLOC_H */
