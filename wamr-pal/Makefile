# ============================================================================
# wamr-pal/Makefile - Build bare-metal VMPL1 WAMR PAL ELF
#
# This produces a static ELF binary that runs directly in VMPL1 (Ring 3)
# with no OS, no libc, no dynamic linking. All services are provided by
# CPUID traps to the VMPL0 Monitor.
#
# Usage:
#   make              # Build wamr_pal.elf
#   make clean        # Remove build artifacts
#   make info         # Print ELF info (readelf)
# ============================================================================

# Toolchain
CC      := gcc
AS      := gcc
LD      := ld
OBJCOPY := objcopy

# Output
TARGET  := wamr_pal.elf

# ---- Directory paths ----
DLMALLOC_DIR := ../wasmlet/third_party/dlmalloc
WAMR_DIR     := ../wasmlet/WAMR-wasmlet
PLATFORM_DIR := platform/vmpl1
BUILD_DIR    := build/wamr

# ============================================================================
# Source files — PAL core
# ============================================================================
PAL_C_SRCS := pal_monitor_call.c pal_string.c pal_malloc.c \
              platform_vmpl1.c config_vmpl1.c wamr_pal_main.c

PAL_S_SRCS := arch/x86_64/pal_start.S

# ---- WAMR VMPL1 platform layer ----
PLATFORM_SRCS := $(PLATFORM_DIR)/vmpl1_platform.c \
                 $(PLATFORM_DIR)/vmpl1_thread.c \
                 $(PLATFORM_DIR)/vmpl1_time.c \
                 $(PLATFORM_DIR)/vmpl1_mmap.c

# ---- wasmlet library sources (from wasmlet submodule) ----
WASMLET_SRC_DIR := ../wasmlet/src
WASMLET_INC_DIR := ../wasmlet/include
WASMLET_C_SRCS := \
    $(WASMLET_SRC_DIR)/core/wasmlet.c \
    $(WASMLET_SRC_DIR)/core/runtime_internal.c \
    $(WASMLET_SRC_DIR)/core/module_internal.c \
    $(WASMLET_SRC_DIR)/core/result_store.c \
    $(WASMLET_SRC_DIR)/isolation/mpk_allocator.c \
    $(WASMLET_SRC_DIR)/isolation/wamr_adaptor.c \
    $(WASMLET_SRC_DIR)/threadpool/lockfree_queue.c \
    $(WASMLET_SRC_DIR)/threadpool/thread_pool.c \
    $(WASMLET_SRC_DIR)/utils/logging.c

WASMLET_BUILD_DIR := build/wasmlet

# ---- dlmalloc ----
DLMALLOC_SRC := $(DLMALLOC_DIR)/malloc.c
DLMALLOC_OBJ := dlmalloc.o

# ============================================================================
# WAMR source files (from WAMR source tree)
# ============================================================================

# Interpreter core
WAMR_INTERP_SRCS := \
    $(WAMR_DIR)/core/iwasm/interpreter/wasm_loader.c \
    $(WAMR_DIR)/core/iwasm/interpreter/wasm_runtime.c \
    $(WAMR_DIR)/core/iwasm/interpreter/wasm_interp_classic.c

# Common layer (excluding wasm_application.c, wasm_c_api.c, wasm_shared_memory.c)
WAMR_COMMON_SRCS := \
    $(WAMR_DIR)/core/iwasm/common/wasm_runtime_common.c \
    $(WAMR_DIR)/core/iwasm/common/wasm_native.c \
    $(WAMR_DIR)/core/iwasm/common/wasm_exec_env.c \
    $(WAMR_DIR)/core/iwasm/common/wasm_memory.c \
    $(WAMR_DIR)/core/iwasm/common/wasm_loader_common.c \
    $(WAMR_DIR)/core/iwasm/common/wasm_blocking_op.c

# x86_64 native call bridge (assembly)
WAMR_ASM_SRCS := \
    $(WAMR_DIR)/core/iwasm/common/arch/invokeNative_em64.s

# Memory allocator (EMS, excluding ems_gc.c)
WAMR_MEM_SRCS := \
    $(WAMR_DIR)/core/shared/mem-alloc/mem_alloc.c \
    $(WAMR_DIR)/core/shared/mem-alloc/ems/ems_kfc.c \
    $(WAMR_DIR)/core/shared/mem-alloc/ems/ems_alloc.c \
    $(WAMR_DIR)/core/shared/mem-alloc/ems/ems_hmu.c

# Utility library
WAMR_UTILS_SRCS := \
    $(WAMR_DIR)/core/shared/utils/bh_assert.c \
    $(WAMR_DIR)/core/shared/utils/bh_common.c \
    $(WAMR_DIR)/core/shared/utils/bh_hashmap.c \
    $(WAMR_DIR)/core/shared/utils/bh_leb128.c \
    $(WAMR_DIR)/core/shared/utils/bh_list.c \
    $(WAMR_DIR)/core/shared/utils/bh_log.c \
    $(WAMR_DIR)/core/shared/utils/bh_queue.c \
    $(WAMR_DIR)/core/shared/utils/bh_vector.c \
    $(WAMR_DIR)/core/shared/utils/bh_bitmap.c \
    $(WAMR_DIR)/core/shared/utils/runtime_timer.c

# All WAMR C sources
WAMR_C_SRCS := $(WAMR_INTERP_SRCS) $(WAMR_COMMON_SRCS) $(WAMR_MEM_SRCS) $(WAMR_UTILS_SRCS)

# Linker script
LDSCRIPT := arch/x86_64/pal.lds

# ============================================================================
# Object files
# ============================================================================

PAL_C_OBJS     := $(PAL_C_SRCS:.c=.o)
PAL_S_OBJS     := $(PAL_S_SRCS:.S=.o)
PLATFORM_OBJS  := $(PLATFORM_SRCS:.c=.o)

WAMR_C_OBJS    := $(patsubst $(WAMR_DIR)/%.c,$(BUILD_DIR)/%.o,$(WAMR_C_SRCS))
WAMR_ASM_OBJS  := $(patsubst $(WAMR_DIR)/%.s,$(BUILD_DIR)/%.o,$(WAMR_ASM_SRCS))

WASMLET_OBJS   := $(patsubst $(WASMLET_SRC_DIR)/%.c,$(WASMLET_BUILD_DIR)/%.o,$(WASMLET_C_SRCS))

ALL_OBJS := $(PAL_S_OBJS) $(PAL_C_OBJS) $(PLATFORM_OBJS) $(DLMALLOC_OBJ) \
            $(WAMR_C_OBJS) $(WAMR_ASM_OBJS) $(WASMLET_OBJS)

# ============================================================================
# Compiler flags
# ============================================================================

GCC_INCLUDE := $(shell $(CC) -print-file-name=include)

# Include paths for WAMR and wasmlet headers
WAMR_INCLUDES := \
    -I$(PLATFORM_DIR) \
    -I$(WAMR_DIR)/core/shared/platform/include \
    -I$(WAMR_DIR)/core/shared/utils \
    -I$(WAMR_DIR)/core/shared/mem-alloc \
    -I$(WAMR_DIR)/core/shared/mem-alloc/ems \
    -I$(WAMR_DIR)/core/iwasm/include \
    -I$(WAMR_DIR)/core/iwasm/common \
    -I$(WAMR_DIR)/core/iwasm/interpreter \
    -I$(WAMR_DIR)/core

# wasmlet include paths (public + internal headers)
WASMLET_INCLUDES := \
    -I$(WASMLET_INC_DIR) \
    -I$(WASMLET_SRC_DIR)/core \
    -I$(DLMALLOC_DIR)

# Base CFLAGS (bare-metal, no libc)
CFLAGS := -nostdlib -nostdinc -ffreestanding \
          -isystem $(GCC_INCLUDE) \
          -fno-stack-protector \
          -fno-pic -fno-pie \
          -mcmodel=large \
          -mno-red-zone \
          -m64 \
          -Wall -Wextra \
          -O2

ASFLAGS := -nostdlib -nostdinc -ffreestanding \
           -fno-pic -fno-pie \
           -mcmodel=large \
           -mno-red-zone \
           -m64

# ---- MPK isolation switch ----
# Phase 2a: ENABLE_MPK_ISOLATION=0 (default, use pal_malloc for all allocations)
# Phase 2b: ENABLE_MPK_ISOLATION=1 (enable MPK pkey domain isolation)
ENABLE_MPK_ISOLATION ?= 1

# ---- WAMR compile-time feature macros ----
# These control which WAMR features are enabled/disabled.
# We only need the classic interpreter, no AOT/JIT/WASI/GC/threads.
WAMR_DEFS := \
    -DENABLE_MPK_ISOLATION=$(ENABLE_MPK_ISOLATION) \
    -DWASM_ENABLE_INTERP=1 \
    -DWASM_ENABLE_FAST_INTERP=0 \
    -DBUILD_TARGET_X86_64 \
    -DBH_PLATFORM_VMPL1 \
    -DBH_MALLOC=wasm_runtime_malloc \
    -DBH_FREE=wasm_runtime_free \
    -DWASM_GLOBAL_HEAP_SIZE=10485760 \
    -DWASM_ENABLE_BULK_MEMORY=1 \
    -DWASM_ENABLE_BULK_MEMORY_OPT=1 \
    -DWASM_DISABLE_HW_BOUND_CHECK=1 \
    -DWASM_DISABLE_STACK_HW_BOUND_CHECK=1 \
    -DWASM_DISABLE_WAKEUP_BLOCKING_OP=1 \
    -DWASM_ENABLE_SHARED_MEMORY=0 \
    -DWASM_ENABLE_THREAD_MGR=0 \
    -DWASM_ENABLE_MULTI_MODULE=0 \
    -DWASM_ENABLE_MINI_LOADER=0 \
    -DWASM_ENABLE_LIBC_BUILTIN=0 \
    -DWASM_ENABLE_LIBC_WASI=0 \
    -DWASM_ENABLE_AOT=0 \
    -DWASM_ENABLE_JIT=0 \
    -DWASM_ENABLE_GC=0 \
    -DWASM_ENABLE_REF_TYPES=0 \
    -DWASM_ENABLE_TAIL_CALL=0 \
    -DWASM_ENABLE_SIMD=0 \
    -DWASM_ENABLE_QUICK_AOT_ENTRY=0 \
    -DWASM_ENABLE_AOT_INTRINSICS=0 \
    -DWASM_ENABLE_SHRUNK_MEMORY=1 \
    -DWASM_ENABLE_EXCE_HANDLING=0 \
    -DWASM_ENABLE_MEMORY64=0 \
    -DWASM_ENABLE_MULTI_MEMORY=0 \
    -DWASM_ENABLE_STRINGREF=0 \
    -DWASM_ENABLE_CALL_INDIRECT_OVERLONG=0 \
    -DWASM_ENABLE_EXTENDED_CONST_EXPR=0 \
    -DWASM_ENABLE_CUSTOM_NAME_SECTION=0 \
    -DWASM_ENABLE_DUMP_CALL_STACK=0 \
    -DWASM_DISABLE_WRITE_GS_BASE=1

# ---- dlmalloc compile flags ----
DLMALLOC_DEFS := \
    -DONLY_MSPACES=1 \
    -DHAVE_MMAP=0 \
    -DHAVE_MREMAP=0 \
    -DNO_MALLOC_STATS=1 \
    -DLACKS_UNISTD_H \
    -DLACKS_SYS_PARAM_H \
    -DLACKS_SYS_MMAN_H \
    -DLACKS_FCNTL_H \
    -DLACKS_ERRNO_H \
    -DLACKS_SCHED_H \
    -DLACKS_TIME_H \
    -DLACKS_STDLIB_H \
    -DLACKS_STRING_H \
    -DLACKS_STRINGS_H \
    -DLACKS_SYS_TYPES_H \
    -Dmalloc_getpagesize=4096 \
    '-DABORT=pal_svsm_exit(1)' \
    '-DMALLOC_FAILURE_ACTION=' \
    -DUSE_LOCKS=1

# ---------- Linker flags ----------
LDFLAGS := -static -nostdlib \
           -T $(LDSCRIPT) \
           -z max-page-size=4096

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean info deploy

all: $(TARGET)

$(TARGET): $(ALL_OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(ALL_OBJS)
	@echo ""
	@echo "============================================"
	@echo " Built: $(TARGET)"
	@echo " Size:  $$(stat -c %s $(TARGET)) bytes"
	@echo "============================================"

# ============================================================================
# Compile rules
# ============================================================================

# PAL core C sources (needs WAMR + wasmlet includes)
%.o: %.c
	$(CC) $(CFLAGS) $(WASMLET_INCLUDES) $(WAMR_INCLUDES) $(WAMR_DEFS) -I. -c -o $@ $<

# WAMR platform C sources (needs WAMR + wasmlet include paths)
$(PLATFORM_DIR)/%.o: $(PLATFORM_DIR)/%.c $(PLATFORM_DIR)/platform_internal.h
	$(CC) $(CFLAGS) $(WASMLET_INCLUDES) $(WAMR_INCLUDES) $(WAMR_DEFS) -c -o $@ $<

# PAL assembly sources
arch/x86_64/%.o: arch/x86_64/%.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# dlmalloc with special flags
$(DLMALLOC_OBJ): $(DLMALLOC_SRC) pal_dlmalloc_config.h
	$(CC) $(CFLAGS) $(DLMALLOC_DEFS) -I. -include pal_dlmalloc_config.h -Wno-expansion-to-defined -c -o $@ $<

# wasmlet library sources → build/wasmlet/*.o
# -isystem wasmlet-compat prioritises compat shim headers over system headers.
$(WASMLET_BUILD_DIR)/%.o: $(WASMLET_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -isystem wasmlet-compat $(WASMLET_INCLUDES) $(WAMR_INCLUDES) $(WAMR_DEFS) -DUSE_MPK_ALLOCATOR -I. -c -o $@ $<

# WAMR C sources → build/wamr/*.o
# -Iinclude-shim provides string.h/assert.h shims for WAMR headers that
# unconditionally include them (e.g. wasm_c_api.h).
# -I. is needed so the shim's #include "pal_string.h" resolves correctly.
$(BUILD_DIR)/%.o: $(WAMR_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude-shim -I. $(WAMR_INCLUDES) $(WAMR_DEFS) -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Wno-type-limits -Wno-unused-variable -c -o $@ $<

# WAMR assembly sources → build/wamr/*.o
$(BUILD_DIR)/%.o: $(WAMR_DIR)/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c -o $@ $<

# ============================================================================
# Utility targets
# ============================================================================

clean:
	rm -f $(PAL_C_OBJS) $(PAL_S_OBJS) $(DLMALLOC_OBJ) $(TARGET)
	rm -f $(PLATFORM_DIR)/*.o
	rm -rf $(BUILD_DIR) $(WASMLET_BUILD_DIR)

# Deploy the ELF and WASM test module to module/example/ for guest access via 9p mount.
deploy: $(TARGET)
	cp $(TARGET) ../module/example/
	@# Compile add.wat → add.wasm if wat2wasm is available, else use existing add.wasm
	@if command -v wat2wasm >/dev/null 2>&1; then \
		wat2wasm add.wat -o add.wasm 2>/dev/null || true; \
	fi
	@if [ -f add.wasm ]; then \
		cp add.wasm ../module/example/; \
		echo "Deployed $(TARGET) + add.wasm to ../module/example/"; \
	else \
		echo "WARNING: add.wasm not found. Compile add.wat first:"; \
		echo "  wat2wasm add.wat -o add.wasm"; \
		echo "Deployed $(TARGET) to ../module/example/"; \
	fi
	@# Ensure dummy files exist for create_zygote/create_trustlet
	@touch ../module/example/dummy.manifest ../module/example/dummy.libos
	@echo "dummy" > ../module/example/dummy_function.txt 2>/dev/null || true

# Print ELF information for verification
info: $(TARGET)
	@echo "===== ELF Header ====="
	readelf -h $(TARGET)
	@echo ""
	@echo "===== Program Headers (PT_LOAD segments) ====="
	readelf -l $(TARGET)
	@echo ""
	@echo "===== Section Headers ====="
	readelf -S $(TARGET)
	@echo ""
	@echo "===== Symbol Table (PAL + WAMR key symbols) ====="
	readelf -s $(TARGET) | grep -E "pal_start|wamr_pal_main|monitor_call|pal_svsm|wasmlet_|wasm_runtime"
