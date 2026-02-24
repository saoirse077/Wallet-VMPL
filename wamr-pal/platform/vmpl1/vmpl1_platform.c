/*
 * vmpl1_platform.c - WAMR platform core for bare-metal VMPL1
 *
 * Implements the "Section 1" APIs from platform_api_vmcore.h:
 *   - bh_platform_init / bh_platform_destroy
 *   - os_malloc / os_realloc / os_free
 *   - os_printf / os_vprintf
 *   - os_set_print_function
 *   - os_dumps_proc_mem_info
 *   - os_dcache_flush / os_icache_flush
 *   - os_is_handle_valid / putchar / puts
 *
 * All memory allocation is delegated to pal_malloc (dlmalloc mspace).
 * All output goes through pal_svsm_debug_print (CPUID trap to VMPL0).
 */
/*
 * vmpl1_platform.c - 面向裸机 VMPL1 的 WAMR 平台核心实现
 *
 * 实现了 platform_api_vmcore.h 中定义的“Section 1”接口：
 *   - bh_platform_init / bh_platform_destroy
 *   - os_malloc / os_realloc / os_free
 *   - os_printf / os_vprintf
 *   - os_set_print_function
 *   - os_dumps_proc_mem_info
 *   - os_dcache_flush / os_icache_flush
 *   - os_is_handle_valid / putchar / puts
 *
 * 所有内存分配都委托给 pal_malloc（基于 dlmalloc 的 mspace）。
 * 所有输出都通过 pal_svsm_debug_print 完成
 * （通过 CPUID trap 切换到 VMPL0 执行）。
 */
 
#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#include "../../pal_monitor_call.h"

/* ================================================================
 * Print function callback
 * ================================================================ */

static os_print_function_t print_function = NULL;

void
os_set_print_function(os_print_function_t pf)
{
    print_function = pf;
}

/* ================================================================
 * Platform init / destroy
 * ================================================================ */

int
bh_platform_init(void)
{
    return BHT_OK;
}

void
bh_platform_destroy(void)
{
}

/* ================================================================
 * Memory allocator — delegates to pal_malloc (dlmalloc mspace)
 *
 * Note: WAMR uses BH_MALLOC / BH_FREE macros (defined as
 * wasm_runtime_malloc / wasm_runtime_free) for most allocations.
 * os_malloc/os_free are only used during early init and by the
 * internal allocator bootstrap.
 * ================================================================ */

void *
os_malloc(unsigned size)
{
    return pal_malloc((size_t)size);
}

void *
os_realloc(void *ptr, unsigned size)
{
    return pal_realloc(ptr, (size_t)size);
}

void
os_free(void *ptr)
{
    pal_free(ptr);
}

/* ================================================================
 * Formatted output
 *
 * os_printf / os_vprintf format the string into a stack buffer,
 * then output via:
 *   1. User-registered print_function callback (if set), OR
 *   2. pal_svsm_debug_print (CPUID trap to VMPL0 serial console)
 * ================================================================ */

#define FIXED_BUFFER_SIZE 512

int
os_printf(const char *format, ...)
{
    char buf[FIXED_BUFFER_SIZE];
    va_list ap;
    int n;

    va_start(ap, format);
    n = vsnprintf(buf, FIXED_BUFFER_SIZE, format, ap);
    va_end(ap);

    if (print_function != NULL) {
        print_function(buf);
    }
    else {
        pal_svsm_debug_print(buf);
    }

    return n;
}

int
os_vprintf(const char *format, va_list ap)
{
    char buf[FIXED_BUFFER_SIZE];
    int n;

    n = vsnprintf(buf, FIXED_BUFFER_SIZE, format, ap);

    if (print_function != NULL) {
        print_function(buf);
    }
    else {
        pal_svsm_debug_print(buf);
    }

    return n;
}

/* ================================================================
 * Misc stubs
 * ================================================================ */

int
os_dumps_proc_mem_info(char *out, unsigned int size)
{
    (void)out;
    (void)size;
    return -1;
}

int
putchar(int c)
{
    char s[2] = { (char)c, '\0' };
    pal_svsm_debug_print(s);
    return c;
}

int
puts(const char *s)
{
    pal_svsm_debug_print(s);
    pal_svsm_debug_print("\n");
    return 0;
}

/* File handle check — bare-metal has no files */
bool
os_is_handle_valid(os_file_handle *handle)
{
    if (!handle)
        return false;
    return *handle > -1;
}

/* Cache flush — no-op on x86-64 (coherent caches) */
void
os_dcache_flush(void)
{
}

void
os_icache_flush(void *start, size_t len)
{
    (void)start;
    (void)len;
}

/* ================================================================
 * WASM C API stubs
 *
 * wasm_runtime_common.c unconditionally includes wasm_c_api_internal.h
 * and calls wasm_trap_delete() in the C-API native call path.
 * Since we don't use the C API, we provide a minimal stub to satisfy
 * the linker.
 * ================================================================ */

/* wasm_trap_t is an opaque struct from wasm_c_api.h */
typedef struct wasm_trap_t wasm_trap_t;

void
wasm_trap_delete(wasm_trap_t *trap)
{
    (void)trap;
    /* No-op: we don't use the WASM C API */
}
