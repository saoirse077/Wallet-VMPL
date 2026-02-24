/*
 * string.h shim for freestanding VMPL1 environment.
 *
 * Some WAMR headers (e.g. wasm_c_api.h) unconditionally include <string.h>.
 * This shim redirects to our pal_string.h which provides all the necessary
 * string/memory functions.
 */
 /*
 * 为 freestanding VMPL1 环境提供的 string.h 兼容层（shim）。
 *
 * 一些 WAMR 头文件（例如 wasm_c_api.h）会无条件包含 <string.h>。
 * 这个兼容层会将其重定向到我们自己的 pal_string.h，
 * 而该头文件已经实现了所有必要的字符串和内存操作函数。
 */
 
#ifndef _SHIM_STRING_H
#define _SHIM_STRING_H

/* Use path relative to wamr-pal/ root, since -I. is on the include path */
#include "pal_string.h"

#endif /* _SHIM_STRING_H */
