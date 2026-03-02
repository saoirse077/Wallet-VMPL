# ============================================================================
# wamr-pal/CMakeLists.txt — Build bare-metal VMPL1 WAMR PAL ELF
#
# Produces a static ELF binary that runs directly in VMPL1 (Ring 3)
# with no OS, no libc, no dynamic linking.
#
# Usage:
#   cmake -B build && cmake --build build
#   cmake --build build --target deploy
# ============================================================================

cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(wamr_pal C ASM)

set(CMAKE_LINKER ld)

# ============================================================================
# Directory paths
# ============================================================================

set(DLMALLOC_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/../wasmlet/third_party/dlmalloc)
set(WAMR_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/../wasmlet/WAMR-wasmlet)
set(WASMLET_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../wasmlet/src)
set(WASMLET_INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../wasmlet/include)
set(PLATFORM_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/platform/vmpl1)
set(LDSCRIPT        ${CMAKE_CURRENT_SOURCE_DIR}/arch/x86_64/pal.lds)

# GCC built-in include directory (stdint.h, stddef.h, stdarg.h, etc.)
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
    OUTPUT_VARIABLE GCC_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "GCC built-in include: ${GCC_INCLUDE_DIR}")

# ============================================================================
# Common bare-metal compile flags
# ============================================================================

set(BARE_METAL_C_FLAGS
    -nostdlib -nostdinc -ffreestanding
    -isystem ${GCC_INCLUDE_DIR}
    -fno-stack-protector -fno-pic -fno-pie
    -mcmodel=large -mno-red-zone -m64
    -Wall -Wextra -O2
)

set(BARE_METAL_ASM_FLAGS
    -nostdlib -nostdinc -ffreestanding
    -fno-pic -fno-pie
    -mcmodel=large -mno-red-zone -m64
)

# ============================================================================
# WAMR compile-time feature macros
# ============================================================================

set(WAMR_DEFS
    ENABLE_MPK_ISOLATION=1
    WASM_ENABLE_INTERP=1
    WASM_ENABLE_FAST_INTERP=0
    BUILD_TARGET_X86_64
    BH_PLATFORM_VMPL1
    BH_MALLOC=wasm_runtime_malloc
    BH_FREE=wasm_runtime_free
    WASM_GLOBAL_HEAP_SIZE=10485760
    WASM_ENABLE_BULK_MEMORY=1
    WASM_ENABLE_BULK_MEMORY_OPT=1
    WASM_DISABLE_HW_BOUND_CHECK=1
    WASM_DISABLE_STACK_HW_BOUND_CHECK=1
    WASM_DISABLE_WAKEUP_BLOCKING_OP=1
    WASM_ENABLE_SHARED_MEMORY=0
    WASM_ENABLE_THREAD_MGR=0
    WASM_ENABLE_MULTI_MODULE=0
    WASM_ENABLE_MINI_LOADER=0
    WASM_ENABLE_LIBC_BUILTIN=0
    WASM_ENABLE_LIBC_WASI=0
    WASM_ENABLE_AOT=0
    WASM_ENABLE_JIT=0
    WASM_ENABLE_GC=0
    WASM_ENABLE_REF_TYPES=0
    WASM_ENABLE_TAIL_CALL=0
    WASM_ENABLE_SIMD=0
    WASM_ENABLE_QUICK_AOT_ENTRY=0
    WASM_ENABLE_AOT_INTRINSICS=0
    WASM_ENABLE_SHRUNK_MEMORY=1
    WASM_ENABLE_EXCE_HANDLING=0
    WASM_ENABLE_MEMORY64=0
    WASM_ENABLE_MULTI_MEMORY=0
    WASM_ENABLE_STRINGREF=0
    WASM_ENABLE_CALL_INDIRECT_OVERLONG=0
    WASM_ENABLE_EXTENDED_CONST_EXPR=0
    WASM_ENABLE_CUSTOM_NAME_SECTION=0
    WASM_ENABLE_DUMP_CALL_STACK=0
    WASM_DISABLE_WRITE_GS_BASE=1
)

# ============================================================================
# Include directory sets (reused across targets)
# ============================================================================

set(WAMR_INCLUDE_DIRS
    ${PLATFORM_DIR}
    ${WAMR_DIR}/core/shared/platform/include
    ${WAMR_DIR}/core/shared/utils
    ${WAMR_DIR}/core/shared/mem-alloc
    ${WAMR_DIR}/core/shared/mem-alloc/ems
    ${WAMR_DIR}/core/iwasm/include
    ${WAMR_DIR}/core/iwasm/common
    ${WAMR_DIR}/core/iwasm/interpreter
    ${WAMR_DIR}/core
)

set(WASMLET_INCLUDE_DIRS
    ${WASMLET_INC_DIR}
    ${WASMLET_SRC_DIR}/core
    ${DLMALLOC_DIR}
)

# ============================================================================
# Helper: apply bare-metal C flags + GCC built-in include to an OBJECT target
# ============================================================================

function(target_bare_metal target)
    target_compile_options(${target} PRIVATE ${BARE_METAL_C_FLAGS})
endfunction()

# ============================================================================
# OBJECT libraries (each group gets its own compile context)
# ============================================================================

# --- PAL core C sources ---
add_library(pal_core OBJECT
    pal_monitor_call.c
    pal_string.c
    pal_malloc.c
    platform_vmpl1.c
    config_vmpl1.c
    wamr_pal_main.c
)
target_bare_metal(pal_core)
target_compile_definitions(pal_core PRIVATE ${WAMR_DEFS})
target_include_directories(pal_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WASMLET_INCLUDE_DIRS}
    ${WAMR_INCLUDE_DIRS}
)

# --- WAMR VMPL1 platform adapter ---
add_library(wamr_platform OBJECT
    ${PLATFORM_DIR}/vmpl1_platform.c
    ${PLATFORM_DIR}/vmpl1_thread.c
    ${PLATFORM_DIR}/vmpl1_time.c
    ${PLATFORM_DIR}/vmpl1_mmap.c
)
target_bare_metal(wamr_platform)
target_compile_definitions(wamr_platform PRIVATE ${WAMR_DEFS})
target_include_directories(wamr_platform PRIVATE ${WAMR_INCLUDE_DIRS})

# --- dlmalloc (special defines + forced include) ---
add_library(dlmalloc_lib OBJECT ${DLMALLOC_DIR}/malloc.c)
target_bare_metal(dlmalloc_lib)
target_compile_options(dlmalloc_lib PRIVATE -Wno-expansion-to-defined)
target_compile_definitions(dlmalloc_lib PRIVATE
    ONLY_MSPACES=1
    HAVE_MMAP=0
    HAVE_MREMAP=0
    NO_MALLOC_STATS=1
    LACKS_UNISTD_H
    LACKS_SYS_PARAM_H
    LACKS_SYS_MMAN_H
    LACKS_FCNTL_H
    LACKS_ERRNO_H
    LACKS_SCHED_H
    LACKS_TIME_H
    LACKS_STDLIB_H
    LACKS_STRING_H
    LACKS_STRINGS_H
    LACKS_SYS_TYPES_H
    "malloc_getpagesize=4096"
    "ABORT=pal_svsm_exit(1)"
    "MALLOC_FAILURE_ACTION="
)
target_include_directories(dlmalloc_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_source_files_properties(${DLMALLOC_DIR}/malloc.c PROPERTIES
    COMPILE_FLAGS "-include ${CMAKE_CURRENT_SOURCE_DIR}/pal_dlmalloc_config.h"
)

# --- wasmlet library (compat shim headers as -isystem) ---
add_library(wasmlet_lib OBJECT
    ${WASMLET_SRC_DIR}/core/wasmlet.c
    ${WASMLET_SRC_DIR}/core/runtime_internal.c
    ${WASMLET_SRC_DIR}/core/module_internal.c
    ${WASMLET_SRC_DIR}/core/result_store.c
    ${WASMLET_SRC_DIR}/isolation/mpk_allocator.c
    ${WASMLET_SRC_DIR}/isolation/wamr_adaptor.c
    ${WASMLET_SRC_DIR}/threadpool/lockfree_queue.c
    ${WASMLET_SRC_DIR}/threadpool/thread_pool.c
    ${WASMLET_SRC_DIR}/utils/logging.c
)
target_bare_metal(wasmlet_lib)
target_compile_definitions(wasmlet_lib PRIVATE ${WAMR_DEFS} USE_MPK_ALLOCATOR)
target_include_directories(wasmlet_lib SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/wasmlet-compat
)
target_include_directories(wasmlet_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WASMLET_INCLUDE_DIRS}
    ${WAMR_INCLUDE_DIRS}
)

# --- WAMR runtime C sources (with warning suppressions) ---
add_library(wamr_lib OBJECT
    ${WAMR_DIR}/core/iwasm/interpreter/wasm_loader.c
    ${WAMR_DIR}/core/iwasm/interpreter/wasm_runtime.c
    ${WAMR_DIR}/core/iwasm/interpreter/wasm_interp_classic.c
    ${WAMR_DIR}/core/iwasm/common/wasm_runtime_common.c
    ${WAMR_DIR}/core/iwasm/common/wasm_native.c
    ${WAMR_DIR}/core/iwasm/common/wasm_exec_env.c
    ${WAMR_DIR}/core/iwasm/common/wasm_memory.c
    ${WAMR_DIR}/core/iwasm/common/wasm_loader_common.c
    ${WAMR_DIR}/core/iwasm/common/wasm_blocking_op.c
    ${WAMR_DIR}/core/shared/mem-alloc/mem_alloc.c
    ${WAMR_DIR}/core/shared/mem-alloc/ems/ems_kfc.c
    ${WAMR_DIR}/core/shared/mem-alloc/ems/ems_alloc.c
    ${WAMR_DIR}/core/shared/mem-alloc/ems/ems_hmu.c
    ${WAMR_DIR}/core/shared/utils/bh_assert.c
    ${WAMR_DIR}/core/shared/utils/bh_common.c
    ${WAMR_DIR}/core/shared/utils/bh_hashmap.c
    ${WAMR_DIR}/core/shared/utils/bh_leb128.c
    ${WAMR_DIR}/core/shared/utils/bh_list.c
    ${WAMR_DIR}/core/shared/utils/bh_log.c
    ${WAMR_DIR}/core/shared/utils/bh_queue.c
    ${WAMR_DIR}/core/shared/utils/bh_vector.c
    ${WAMR_DIR}/core/shared/utils/bh_bitmap.c
    ${WAMR_DIR}/core/shared/utils/runtime_timer.c
)
target_bare_metal(wamr_lib)
target_compile_options(wamr_lib PRIVATE
    -Wno-unused-parameter -Wno-sign-compare
    -Wno-missing-field-initializers -Wno-type-limits -Wno-unused-variable
)
target_compile_definitions(wamr_lib PRIVATE ${WAMR_DEFS})
target_include_directories(wamr_lib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include-shim
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${WAMR_INCLUDE_DIRS}
)

# ============================================================================
# Final ELF
# ============================================================================

# Assembly sources included directly (CMake needs at least one source file)
add_executable(wamr_pal
    arch/x86_64/pal_start.S
    ${WAMR_DIR}/core/iwasm/common/arch/invokeNative_em64.s
)
set_source_files_properties(
    arch/x86_64/pal_start.S
    ${WAMR_DIR}/core/iwasm/common/arch/invokeNative_em64.s
    PROPERTIES COMPILE_FLAGS
    "-nostdlib -nostdinc -ffreestanding -fno-pic -fno-pie -mcmodel=large -mno-red-zone -m64"
)

target_link_libraries(wamr_pal PRIVATE
    pal_core wamr_platform dlmalloc_lib wamr_lib wasmlet_lib
)

set_target_properties(wamr_pal PROPERTIES
    OUTPUT_NAME wamr_pal
    SUFFIX      .elf
    LINK_DEPENDS ${LDSCRIPT}
)

# Override link command: use ld directly with bare-metal linker script
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_LINKER> -static -nostdlib -T ${LDSCRIPT} -z max-page-size=4096 <OBJECTS> -o <TARGET>"
)

# Post-build: print size
add_custom_command(TARGET wamr_pal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Built: wamr_pal.elf"
)

# ============================================================================
# Deploy target
# ============================================================================

set(DEPLOY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../module/example)

add_custom_target(deploy
    DEPENDS wamr_pal
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:wamr_pal> ${DEPLOY_DIR}/wamr_pal.elf
    COMMAND ${CMAKE_COMMAND} -E echo "Deployed wamr_pal.elf to module/example/"
    COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/dummy.manifest
    COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/dummy.libos
)
