/*
 * assert.h shim for freestanding VMPL1 environment.
 *
 * Some WAMR headers (e.g. wasm_c_api.h) unconditionally include <assert.h>.
 * Our platform_internal.h already defines assert() as a macro, so this
 * shim just needs to exist to satisfy the #include.
 *
 * If platform_internal.h hasn't been included yet, we provide a fallback.
 */
 /*
 * 为 freestanding VMPL1 环境提供的 assert.h 兼容层（shim）。
 *
 * 一些 WAMR 头文件（例如 wasm_c_api.h）会无条件包含 <assert.h>。
 * 我们的 platform_internal.h 已经将 assert() 定义为一个宏，
 * 因此这个兼容层只需要“存在”，以满足 #include 的要求即可。
 *
 * 如果 platform_internal.h 尚未被包含，我们则提供一个后备实现（fallback）。
 */
 
#ifndef _SHIM_ASSERT_H
#define _SHIM_ASSERT_H

#ifndef assert
extern void abort(void);
extern void pal_svsm_debug_print(const char *msg);
#define assert(expr)                                              \
    do {                                                          \
        if (!(expr)) {                                            \
            pal_svsm_debug_print("[ASSERT FAIL] " #expr "\n");   \
            abort();                                              \
        }                                                         \
    } while (0)
#endif

/* C11 static_assert — GCC supports _Static_assert natively */
#ifndef static_assert
#define static_assert _Static_assert
#endif

#endif /* _SHIM_ASSERT_H */
