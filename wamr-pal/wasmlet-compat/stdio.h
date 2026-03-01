/*
 * stdio.h compat shim for wasmlet on VMPL1
 *
 * Provides snprintf/vsnprintf (from pal_string.h).
 * FILE-based I/O is not available; fprintf/fopen/etc. are stubbed.
 */

#ifndef _WASMLET_COMPAT_STDIO_H
#define _WASMLET_COMPAT_STDIO_H

#include "pal_string.h"  /* snprintf, vsnprintf */

typedef void FILE;

#define stdin  ((FILE *)0)
#define stdout ((FILE *)0)
#define stderr ((FILE *)0)

#define EOF (-1)

static inline int fprintf(FILE *f, const char *fmt, ...) {
    (void)f; (void)fmt;
    return 0;
}

static inline int fflush(FILE *f) { (void)f; return 0; }

static inline FILE *fopen(const char *path, const char *mode) {
    (void)path; (void)mode;
    return (FILE *)0;
}

static inline int fclose(FILE *f) { (void)f; return 0; }

#endif /* _WASMLET_COMPAT_STDIO_H */
