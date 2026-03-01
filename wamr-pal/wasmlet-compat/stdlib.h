/*
 * stdlib.h compat shim for wasmlet on VMPL1
 *
 * Redirects standard allocator calls to pal_malloc/free.
 */

#ifndef _WASMLET_COMPAT_STDLIB_H
#define _WASMLET_COMPAT_STDLIB_H

#include <stddef.h>
#include "pal_malloc.h"

#define malloc  pal_malloc
#define free    pal_free
#define realloc pal_realloc
#define calloc  pal_calloc

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

extern void abort(void) __attribute__((noreturn));

#endif /* _WASMLET_COMPAT_STDLIB_H */
