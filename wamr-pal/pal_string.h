/*
 * pal_string.h - Freestanding libc stubs for bare-metal VMPL1
 *
 * Provides minimal string/memory/formatting functions that WAMR needs.
 * No glibc or musl dependency — everything is hand-written.
 *
 * GCC freestanding headers (stdint.h, stddef.h, stdbool.h, stdarg.h)
 * are the ONLY system headers we use.
 */

#ifndef PAL_STRING_H
#define PAL_STRING_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== Memory operations ========== */

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);

/* ========== String operations ========== */

size_t strlen(const char *s);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcat(char *dest, const char *src);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strdup(const char *s);  /* uses pal_malloc */
char  *strndup(const char *s, size_t n);

/* ========== Formatted output ========== */
/*
 * Simplified vsnprintf — supports:
 *   %s %d %u %x %X %p %c %%
 *   %ld %lu %lx %lX (long modifier)
 *   %lld %llu %llx %llX (long long modifier)
 *   Width + zero-fill: %02u, %012X, etc.
 *
 * Does NOT support: %f %e %g (floating point), %n, * width/precision
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* ========== Type conversion ========== */

long          strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int           atoi(const char *nptr);

/* ========== ctype functions ========== */

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isxdigit(int c);
int isspace(int c);
int isprint(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

/* ========== Sorting / Searching ========== */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* ========== Math macros (for WASM FP opcodes) ========== */
/*
 * isnan / signbit / isinf / isfinite — GCC builtins, no libm needed.
 * These are macros in <math.h> but we don't have that header.
 */
#ifndef isnan
#define isnan(x)    __builtin_isnan(x)
#endif
#ifndef isinf
#define isinf(x)    __builtin_isinf(x)
#endif
#ifndef isfinite
#define isfinite(x) __builtin_isfinite(x)
#endif
#ifndef signbit
#define signbit(x)  __builtin_signbit(x)
#endif
#ifndef INFINITY
#define INFINITY    __builtin_inf()
#endif
#ifndef NAN
#define NAN         __builtin_nan("")
#endif
#ifndef HUGE_VAL
#define HUGE_VAL    __builtin_huge_val()
#endif

/* ========== Math stubs (for WASM FP opcodes) ========== */
/*
 * The classic interpreter references these math functions for floating-point
 * WASM opcodes.  Our test only uses integer add, so these will never be
 * called at runtime.  We provide stubs to satisfy the linker.
 */
double fabs(double x);
double ceil(double x);
double floor(double x);
double trunc(double x);
double rint(double x);
double sqrt(double x);
float  fabsf(float x);
float  ceilf(float x);
float  floorf(float x);
float  truncf(float x);
float  rintf(float x);
float  sqrtf(float x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);

/* ========== Absolute value ========== */

long      labs(long x);
long long llabs(long long x);

/* ========== Error handling ========== */

void abort(void) __attribute__((noreturn));

/* ========== errno stub ========== */
/*
 * WAMR code occasionally references errno (behind #if guards we disable).
 * Provide a global stub so any accidental reference links cleanly.
 */
extern int errno;

#ifdef __cplusplus
}
#endif

#endif /* PAL_STRING_H */
