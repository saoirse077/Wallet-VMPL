/*
 * pal_string.c - Freestanding libc stubs for bare-metal VMPL1
 *
 * Hand-written implementations of string, memory, formatting, and utility
 * functions needed by WAMR.  No external dependencies except our own headers.
 */
 /*
 * pal_string.c - 面向裸机 VMPL1 环境的精简 libc 替代实现
 *
 * 手写实现了 WAMR 所需的字符串处理、内存操作、格式化以及工具函数。
 * 除了我们自己的头文件外，不依赖任何外部库。
 */

#include "pal_string.h"
#include "pal_monitor_call.h"   /* pal_svsm_exit, pal_svsm_debug_print */

/* Forward declaration — defined in pal_malloc.c (Step 2) */
extern void *pal_malloc(size_t size);

/* ========== errno stub ========== */

int errno = 0;

/* ================================================================
 * Memory operations — x86-64 optimized with REP string instructions
 *
 * On CPUs with ERMS (Enhanced REP MOVSB/STOSB), these instructions
 * are hardware-accelerated: the CPU internally selects optimal
 * transfer widths (8/16/32/64 bytes) and handles alignment
 * automatically.  Performance for large copies approaches SIMD
 * implementations, without requiring SSE/AVX state (critical in
 * bare-metal VMPL1 where FPU/SIMD may not be initialized).
 *
 * For small, compile-time-known sizes, GCC -O2 will bypass these
 * functions entirely and emit inline MOV instructions (builtin
 * optimization), so the REP prefix startup cost is irrelevant.
 * ================================================================ */

void *memset(void *s, int c, size_t n)
{
    void *ret = s;
    __asm__ volatile (
        "rep stosb"
        : "+D"(s),      /* rdi = destination (updated) */
          "+c"(n)        /* rcx = count      (updated) */
        : "a"((unsigned char)c) /* al = fill byte */
        : "memory"
    );
    return ret;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    void *ret = dest;
    __asm__ volatile (
        "rep movsb"
        : "+D"(dest),    /* rdi = destination (updated) */
          "+S"(src),      /* rsi = source      (updated) */
          "+c"(n)         /* rcx = count       (updated) */
        :
        : "memory"
    );
    return ret;
}

void *memmove(void *dest, const void *src, size_t n)
{
    if (n == 0)
        return dest;

    if ((uintptr_t)dest <= (uintptr_t)src ||
        (uintptr_t)dest >= (uintptr_t)src + n) {
        /* No overlap or forward-safe: use fast forward REP MOVSB */
        void *ret = dest;
        void *d = dest;
        const void *s = src;
        size_t c = n;
        __asm__ volatile (
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(c)
            :
            : "memory"
        );
        return ret;
    }

    /* Overlapping with dest > src: must copy backward.
     * Set DF (direction flag) so REP MOVSB decrements rdi/rsi,
     * then clear DF to restore the default forward direction.
     * rdi/rsi must point to the LAST byte before the REP. */
    void *ret = dest;
    char *d_last = (char *)dest + n - 1;
    const char *s_last = (const char *)src + n - 1;
    __asm__ volatile (
        "std\n\t"
        "rep movsb\n\t"
        "cld"
        : "+D"(d_last), "+S"(s_last), "+c"(n)
        :
        : "memory"
    );
    return ret;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    if (n == 0)
        return 0;

    /* REPE CMPSB: compare bytes while equal, stops at first mismatch.
     * After execution:
     *   - ZF=1 if all bytes matched (rcx reached 0)
     *   - ZF=0 if mismatch found; rsi/rdi point past the differing bytes
     */
    const unsigned char *a = (const unsigned char *)s1;
    const unsigned char *b = (const unsigned char *)s2;

    __asm__ volatile (
        "repe cmpsb"
        : "+S"(a), "+D"(b), "+c"(n)
        :
        : "memory", "cc"
    );

    /* After REPE CMPSB, a and b point one past the last compared byte.
     * If all matched, they point past the end and we return 0.
     * If mismatch, back up one byte to get the differing pair. */
    if (n == 0)
        return 0;  /* all bytes matched */
    return (int)a[-1] - (int)b[-1];
}

/* ================================================================
 * String operations
 * ================================================================ */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : (void *)0;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen;
    if (!*needle)
        return (char *)haystack;
    nlen = strlen(needle);
    while (*haystack) {
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return (void *)0;
}

char *strdup(const char *s)
{
    size_t len = strlen(s) + 1;
    char *dup = (char *)pal_malloc(len);
    if (dup)
        memcpy(dup, s, len);
    return dup;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len > n)
        len = n;
    char *dup = (char *)pal_malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

/* ================================================================
 * ctype functions
 * ================================================================ */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalpha(int c)  { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int isprint(int c)  { return c >= 0x20 && c <= 0x7e; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }

/* ================================================================
 * Type conversion
 * ================================================================ */

static int char_to_digit(char c, int base)
{
    int val;
    if (c >= '0' && c <= '9')
        val = c - '0';
    else if (c >= 'a' && c <= 'z')
        val = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        val = c - 'A' + 10;
    else
        return -1;
    return (val < base) ? val : -1;
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    unsigned long result = 0;
    int digit;

    /* skip whitespace */
    while (isspace((unsigned char)*s))
        s++;

    /* optional '+' */
    if (*s == '+')
        s++;

    /* auto-detect base */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') {
                base = 16;
                s++;
            } else {
                base = 8;
            }
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
    }

    while ((digit = char_to_digit(*s, base)) >= 0) {
        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)s;
    return result;
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    int neg = 0;

    while (isspace((unsigned char)*s))
        s++;

    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    unsigned long val = strtoul(s, endptr, base);
    return neg ? -(long)val : (long)val;
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, (void *)0, 10);
}

/* ================================================================
 * Formatted output (vsnprintf / snprintf)
 *
 * Supports: %s %d %u %x %X %p %c %%
 *           %ld %lu %lx %lX %lld %llu %llx %llX
 *           Width + zero-fill: %02u, %012X, etc.
 * Does NOT support: %f %e %g %n, * width/precision, '-' flag
 * ================================================================ */

/* Write an unsigned 64-bit integer in the given base (2-16) */
static int fmt_uint64(char *buf, size_t remain, uint64_t val,
                      int base, int uppercase, int width, char fill)
{
    char tmp[20]; /* max 20 digits for uint64 decimal */
    int len = 0;
    int written = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        const char *digits = uppercase
            ? "0123456789ABCDEF"
            : "0123456789abcdef";
        while (val) {
            tmp[len++] = digits[val % (unsigned)base];
            val /= (unsigned)base;
        }
    }

    /* pad with fill character */
    int pad = (width > len) ? width - len : 0;
    for (int i = 0; i < pad && remain > 1; i++) {
        *buf++ = fill;
        remain--;
        written++;
    }

    /* write digits in reverse order */
    for (int i = len - 1; i >= 0 && remain > 1; i--) {
        *buf++ = tmp[i];
        remain--;
        written++;
    }

    return written;
}

/* Write a signed 64-bit integer in decimal */
static int fmt_int64(char *buf, size_t remain, int64_t val,
                     int width, char fill)
{
    int written = 0;
    uint64_t uval;

    if (val < 0) {
        if (remain > 1) {
            *buf++ = '-';
            remain--;
            written++;
        }
        uval = (uint64_t)(-(val + 1)) + 1;
        if (width > 0) width--;
    } else {
        uval = (uint64_t)val;
    }

    written += fmt_uint64(buf, remain, uval, 10, 0, width, fill);
    return written;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *out = buf;
    size_t remain = size;
    int total = 0;

    if (!buf || size == 0) {
        /* TODO: count-only mode not fully implemented */
        return 0;
    }

    while (*fmt && remain > 1) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            remain--;
            total++;
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags */
        char fill = ' ';
        if (*fmt == '0') {
            fill = '0';
            fmt++;
        }

        /* Parse width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Parse length modifier */
        int is_long = 0;    /* 1 = long, 2 = long long */
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        }

        /* Parse conversion specifier */
        int n = 0;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && remain > 1) {
                *out++ = *s++;
                remain--;
                total++;
            }
            break;
        }
        case 'd':
        case 'i': {
            int64_t val;
            if (is_long >= 2)
                val = (int64_t)va_arg(ap, long long);
            else if (is_long == 1)
                val = (int64_t)va_arg(ap, long);
            else
                val = (int64_t)va_arg(ap, int);
            n = fmt_int64(out, remain, val, width, fill);
            out += n;
            remain -= (size_t)n;
            total += n;
            break;
        }
        case 'u': {
            uint64_t val;
            if (is_long >= 2)
                val = (uint64_t)va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = (uint64_t)va_arg(ap, unsigned long);
            else
                val = (uint64_t)va_arg(ap, unsigned int);
            n = fmt_uint64(out, remain, val, 10, 0, width, fill);
            out += n;
            remain -= (size_t)n;
            total += n;
            break;
        }
        case 'x':
        case 'X': {
            uint64_t val;
            if (is_long >= 2)
                val = (uint64_t)va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = (uint64_t)va_arg(ap, unsigned long);
            else
                val = (uint64_t)va_arg(ap, unsigned int);
            n = fmt_uint64(out, remain, val, 16, (*fmt == 'X'), width, fill);
            out += n;
            remain -= (size_t)n;
            total += n;
            break;
        }
        case 'p': {
            uint64_t val = (uint64_t)(uintptr_t)va_arg(ap, void *);
            /* "0x" prefix */
            if (remain > 2) {
                *out++ = '0'; remain--; total++;
                *out++ = 'x'; remain--; total++;
            }
            n = fmt_uint64(out, remain, val, 16, 0, width, '0');
            out += n;
            remain -= (size_t)n;
            total += n;
            break;
        }
        case 'c': {
            int ch = va_arg(ap, int);
            *out++ = (char)ch;
            remain--;
            total++;
            break;
        }
        case '%':
            *out++ = '%';
            remain--;
            total++;
            break;
        default:
            /* Unknown specifier — just output it literally */
            *out++ = '%';
            remain--;
            total++;
            if (remain > 1) {
                *out++ = *fmt;
                remain--;
                total++;
            }
            break;
        }
        fmt++;
    }

    /* Null-terminate */
    if (size > 0)
        *out = '\0';

    return total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;
    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

/* ================================================================
 * qsort — simple shell sort (good enough for WAMR's small arrays)
 * ================================================================ */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;
    /* Simple insertion sort — O(n^2) but correct and tiny code */
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0) {
            char *a = arr + (j - 1) * size;
            char *b = arr + j * size;
            if (compar(a, b) <= 0)
                break;
            /* swap a and b */
            for (size_t k = 0; k < size; k++) {
                char tmp = a[k];
                a[k] = b[k];
                b[k] = tmp;
            }
            j--;
        }
    }
}

/* ================================================================
 * bsearch — binary search (standard C library)
 * ================================================================ */

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *elem = arr + mid * size;
        int cmp = compar(key, elem);
        if (cmp < 0)
            hi = mid;
        else if (cmp > 0)
            lo = mid + 1;
        else
            return (void *)elem;
    }
    return (void *)0;
}

/* ================================================================
 * Math stubs — for WASM floating-point opcodes
 *
 * The classic interpreter uses these for f32/f64 math operations.
 * Our Phase 2 test only uses integer add, so these are never called
 * at runtime.  We use GCC builtins where available for correctness
 * in case they are ever reached.
 *
 * 数学函数桩（Math stubs）——用于 WASM 的浮点操作码
 *
 * 经典解释器在执行 f32 / f64 数学运算时会调用这些函数。
 * 我们当前的 Phase 2 测试只使用整数加法，因此这些函数在运行时
 * 实际上不会被调用。
 *
 * 为了保证正确性（以防未来真的执行到这些路径），
 * 在可用的情况下我们使用 GCC 内建函数（builtins）来实现。
 * ================================================================ */

double fabs(double x)  { return x < 0 ? -x : x; }
float  fabsf(float x)  { return x < 0 ? -x : x; }

double ceil(double x)  { return __builtin_ceil(x); }
float  ceilf(float x)  { return __builtin_ceilf(x); }

double floor(double x) { return __builtin_floor(x); }
float  floorf(float x) { return __builtin_floorf(x); }

double trunc(double x) { return __builtin_trunc(x); }
float  truncf(float x) { return __builtin_truncf(x); }

double rint(double x)  { return __builtin_rint(x); }
float  rintf(float x)  { return __builtin_rintf(x); }

double sqrt(double x)  { return __builtin_sqrt(x); }
float  sqrtf(float x)  { return __builtin_sqrtf(x); }

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }
float  fminf(float x, float y) { return x < y ? x : y; }
float  fmaxf(float x, float y) { return x > y ? x : y; }

/* ================================================================
 * abort — fatal error handler
 * ================================================================ */

/* ========== Absolute value ========== */

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

/* ========== abort ========== */

void abort(void)
{
    pal_svsm_debug_print("[FATAL] abort() called\n");
    pal_svsm_exit(1);
    /* unreachable, but needed to satisfy __attribute__((noreturn)) */
    while (1) {}
}
