/*
 * wamr_pal_main.c - WAMR PAL entry point for VMPL1 (Phase 2)
 *
 * This is the main function of the bare-metal WAMR Runtime ELF
 * running in VMPL1. Called by pal_start.S after stack setup.
 *
 * Execution environment:
 *   - VMPL1, Ring 3 (CPL=3)
 *   - Stack at TP_STACK_START_VADDR + 8*4096 = 0x80_0000_0000 + 0x8000
 *   - No libc, no OS
 *   - Only CPUID traps to VMPL0 Monitor for services
 *   - Input channel at 0x280_0000_0000 (data from VMPL0/guest)
 *   - Output channel at 0x300_0000_0000 (results to VMPL0/guest)
 *
 * Phase 2 flow:
 *   1. pal_heap_init()           → 初始化全局堆 (dlmalloc mspace, 16 MB)
 *   2. wasmlet_runtime_init()    → 初始化 WAMR runtime (+ MPK 分配器)
 *   3. wasmlet_load_module()     → 加载嵌入的 add.wasm 模块
 *   4. wasmlet_invoke("add",…)   → 实例化 + 执行 add(3,5) + 销毁实例
 *   5. 输出结果到 output channel
 *   6. wasmlet_unload_module()   → 卸载模块
 *   7. wasmlet_runtime_destroy() → 销毁 WAMR runtime
 *   8. pal_svsm_exit(0)          → 退出 VMPL1
 */

#include "pal_monitor_call.h"
#include "pal_malloc.h"
#include "pal_string.h"
#include "wasmlet_vmpl1.h"

/* Memory channel addresses (set by SVSM in process_cow.rs early_init) */
#define INPUT_CHANNEL_ADDR   0x28000000000ULL
#define OUTPUT_CHANNEL_ADDR  0x30000000000ULL

/* Output channel data format:
 *   [0..3]  uint32_t status   // 0 = success, non-zero = error
 *   [4..7]  uint32_t result   // function return value
 */
#define OUTPUT_STATUS_OFFSET  0
#define OUTPUT_RESULT_OFFSET  4

/*
 * ============================================================
 * Embedded WASM test module: add.wasm (84 bytes)
 *
 * Source: wasmlet/tests/wasm-modules/add.wasm
 * Exports: add(i32, i32) -> i32
 *          multiply(i32, i32) -> i32
 *          get_answer() -> i32
 *
 * Compiled from:
 *   (module
 *     (func (export "add") (param i32 i32) (result i32)
 *       local.get 0  local.get 1  i32.add)
 *     (func (export "multiply") (param i32 i32) (result i32)
 *       local.get 0  local.get 1  i32.mul)
 *     (func (export "get_answer") (result i32)
 *       i32.const 42))
 * ============================================================
 */
static const uint8_t embedded_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0b, 0x02, 0x60,
    0x02, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x03, 0x04, 0x03,
    0x00, 0x00, 0x01, 0x07, 0x1f, 0x03, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
    0x08, 0x6d, 0x75, 0x6c, 0x74, 0x69, 0x70, 0x6c, 0x79, 0x00, 0x01, 0x0a,
    0x67, 0x65, 0x74, 0x5f, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x00, 0x02,
    0x0a, 0x16, 0x03, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b, 0x07,
    0x00, 0x20, 0x00, 0x20, 0x01, 0x6c, 0x0b, 0x04, 0x00, 0x41, 0x2a, 0x0b
};
static const uint32_t embedded_wasm_size = sizeof(embedded_wasm);

/*
 * write_output_channel - Write execution result to the output channel
 *
 * The output channel is at a fixed virtual address (0x300_0000_0000).
 * During invoke_trustlet, SVSM inflates the output channel pages.
 * During early_invoke (create_zygote), we inflate it ourselves.
 *
 * Format:
 *   [0..3]  uint32_t status   // 0 = success, 1 = error
 *   [4..7]  uint32_t result   // function return value
 */
static void write_output_channel(uint32_t status, uint32_t result)
{
    /*
     * Inflate the output channel (1 page = 4096 bytes).
     * This asks VMPL0 to allocate physical pages and map them
     * at OUTPUT_CHANNEL_ADDR in our page table.
     *
     * select=1 means output channel.
     */
    int ret = pal_svsm_inflate_channel(1, 4096);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] WARNING: inflate output channel failed, error=");
        pal_svsm_debug_print_dec(ret);
        pal_svsm_debug_print("\n");
        /* Continue anyway - the pages might already be mapped */
    }

    volatile uint32_t *output = (volatile uint32_t *)OUTPUT_CHANNEL_ADDR;
    output[OUTPUT_STATUS_OFFSET / sizeof(uint32_t)] = status;
    output[OUTPUT_RESULT_OFFSET / sizeof(uint32_t)] = result;

    pal_svsm_debug_print("[WAMR-PAL] Output channel written: status=");
    pal_svsm_debug_print_dec((int)status);
    pal_svsm_debug_print(", result=");
    pal_svsm_debug_print_dec((int)result);
    pal_svsm_debug_print("\n");
}

/*
 * wamr_pal_main - VMPL1 entry point (Phase 2)
 *
 * Called by pal_start.S. Must not return (calls pal_svsm_exit).
 *
 * This function orchestrates the complete WAMR lifecycle:
 *   heap init → runtime init → load module → invoke → unload → destroy → exit
 */
void wamr_pal_main(void)
{
    uint32_t result = 0;
    int ret;

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] WAMR Runtime Phase 2 Starting\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    /* ===== Step 1: Initialize heap (dlmalloc mspace, 16 MB) ===== */
    pal_svsm_debug_print("[WAMR-PAL] Initializing heap...\n");
    ret = pal_heap_init();
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: Heap init failed\n");
        write_output_channel(1, 0);
        pal_svsm_exit(1);
    }
    pal_svsm_debug_print("[WAMR-PAL] Heap initialized (16 MB)\n");

    /* ===== Step 2: Initialize WAMR runtime ===== */
    pal_svsm_debug_print("[WAMR-PAL] Initializing WAMR runtime...\n");
    ret = wasmlet_runtime_init();
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: WAMR runtime init failed\n");
        write_output_channel(1, 0);
        pal_svsm_exit(1);
    }
    pal_svsm_debug_print("[WAMR-PAL] WAMR runtime initialized\n");

    /* ===== Step 3: Load WASM module (embedded add.wasm) ===== */
    pal_svsm_debug_print("[WAMR-PAL] Loading embedded add.wasm (");
    pal_svsm_debug_print_dec((int)embedded_wasm_size);
    pal_svsm_debug_print(" bytes)...\n");

    /*
     * IMPORTANT: WAMR's wasm_runtime_load() modifies the buffer in-place
     * (e.g. byte-order swaps, internal patching). The embedded_wasm array
     * lives in .rodata (read-only segment), so we must copy it to a
     * writable heap buffer before passing it to WAMR.
     */
    uint8_t *wasm_buf = (uint8_t *)pal_malloc(embedded_wasm_size);
    if (!wasm_buf) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: Failed to allocate WASM buffer\n");
        write_output_channel(1, 0);
        wasmlet_runtime_destroy();
        pal_svsm_exit(1);
    }
    memcpy(wasm_buf, embedded_wasm, embedded_wasm_size);

    ret = wasmlet_load_module(wasm_buf, embedded_wasm_size);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: Module load failed\n");
        pal_free(wasm_buf);
        write_output_channel(1, 0);
        wasmlet_runtime_destroy();
        pal_svsm_exit(1);
    }
    pal_svsm_debug_print("[WAMR-PAL] Module loaded OK\n");

    /* ===== Step 4: Invoke add(3, 5) ===== */
    {
        uint32_t argv[2] = { 3, 5 };

        pal_svsm_debug_print("[WAMR-PAL] Calling add(3, 5)...\n");

        ret = wasmlet_invoke("add", 2, argv, &result);
        if (ret != 0) {
            pal_svsm_debug_print("[WAMR-PAL] FATAL: Invoke failed\n");
            write_output_channel(1, 0);
            wasmlet_unload_module();
            wasmlet_runtime_destroy();
            pal_svsm_exit(1);
        }

        pal_svsm_debug_print("[WAMR-PAL] Result: ");
        pal_svsm_debug_print_dec((int)result);
        pal_svsm_debug_print("\n");

        /* Verify result */
        if (result == 8) {
            pal_svsm_debug_print("[WAMR-PAL] *** PASS: add(3, 5) == 8 ***\n");
        } else {
            pal_svsm_debug_print("[WAMR-PAL] *** FAIL: expected 8, got ");
            pal_svsm_debug_print_dec((int)result);
            pal_svsm_debug_print(" ***\n");
        }
    }

    /* ===== Step 4b: Bonus test - invoke multiply(4, 7) ===== */
    {
        uint32_t argv2[2] = { 4, 7 };
        uint32_t result2 = 0;

        pal_svsm_debug_print("[WAMR-PAL] Calling multiply(4, 7)...\n");

        ret = wasmlet_invoke("multiply", 2, argv2, &result2);
        if (ret == 0) {
            pal_svsm_debug_print("[WAMR-PAL] multiply result: ");
            pal_svsm_debug_print_dec((int)result2);
            pal_svsm_debug_print("\n");
            if (result2 == 28) {
                pal_svsm_debug_print("[WAMR-PAL] *** PASS: multiply(4, 7) == 28 ***\n");
            } else {
                pal_svsm_debug_print("[WAMR-PAL] *** FAIL: expected 28 ***\n");
            }
        } else {
            pal_svsm_debug_print("[WAMR-PAL] multiply invoke failed\n");
        }
    }

    /* ===== Step 4c: Bonus test - invoke get_answer() ===== */
    {
        uint32_t argv3[1] = { 0 };  /* need buffer for return value */
        uint32_t result3 = 0;

        pal_svsm_debug_print("[WAMR-PAL] Calling get_answer()...\n");

        ret = wasmlet_invoke("get_answer", 0, argv3, &result3);
        if (ret == 0) {
            pal_svsm_debug_print("[WAMR-PAL] get_answer result: ");
            pal_svsm_debug_print_dec((int)result3);
            pal_svsm_debug_print("\n");
            if (result3 == 42) {
                pal_svsm_debug_print("[WAMR-PAL] *** PASS: get_answer() == 42 ***\n");
            } else {
                pal_svsm_debug_print("[WAMR-PAL] *** FAIL: expected 42 ***\n");
            }
        } else {
            pal_svsm_debug_print("[WAMR-PAL] get_answer invoke failed\n");
        }
    }

    /* ===== Step 5: Write result to output channel ===== */
    write_output_channel(0, result);

    /* ===== Step 6: Cleanup ===== */
    pal_svsm_debug_print("[WAMR-PAL] Unloading module...\n");
    wasmlet_unload_module();
    pal_free(wasm_buf);  /* Free the heap-allocated WASM buffer after unload */

    pal_svsm_debug_print("[WAMR-PAL] Destroying runtime...\n");
    wasmlet_runtime_destroy();

    /* ===== Step 7: Exit ===== */
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Phase 2 complete. All tests done.\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    pal_svsm_exit(0);

    /* Should never reach here */
    while (1) {}
}
