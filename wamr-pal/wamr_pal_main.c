/*
 * wamr_pal_main.c - WAMR PAL entry point for VMPL1 (Phase 3b)
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
 * Phase 3b execution model (two-phase lifecycle with module reuse):
 *
 *   Phase A — early_invoke (triggered by create_zygote from Guest):
 *     1. pal_heap_init()           → Initialize global heap (dlmalloc mspace)
 *     2. wasmlet_runtime_init()    → Initialize WAMR runtime (+ MPK allocator)
 *     3. pal_svsm_exit(0)          → Suspend VMPL1, return to SVSM
 *        (VMSA.RIP now points to instruction after cpuid in pal_svsm_exit)
 *
 *   Phase B — invoke_trustlet (triggered by invoke_trustlet_bin from Guest):
 *     (pal_svsm_exit returns normally because SVSM's ap_create resumes VMPL1)
 *     4. Read input channel header
 *     5. Three modes based on wasm_size and func_name_len:
 *
 *        Mode 1: wasm_size > 0, func_name_len > 0
 *          → Load new module + invoke function (first call or module switch)
 *          → If a module is already loaded, unload it first
 *
 *        Mode 2: wasm_size == 0, func_name_len > 0
 *          → Invoke-only (reuse already loaded module, no WASM bytecode)
 *          → Only function name + arguments are in the input channel
 *
 *        Mode 3: wasm_size == 0, func_name_len == 0
 *          → Shutdown signal: unload module + destroy runtime + exit
 *
 *     6. Write result to output channel
 *     7. pal_svsm_get_result()     → Notify SVSM results are ready
 *     8. Loop back to step 4 for next invocation
 *
 *   Cleanup (Mode 3 shutdown):
 *     9. wasmlet_unload_module()
 *     10. wasmlet_runtime_destroy()
 *     11. pal_svsm_exit(0)         → Final exit
 *
 * Input channel protocol (at 0x280_0000_0000):
 *
 *   Mode 1 (load + invoke):
 *     [0..3]   uint32_t wasm_size      // WASM bytecode size (> 0)
 *     [4..7]   uint32_t func_name_len  // Length of function name (> 0)
 *     [8..9]   uint16_t argc           // Number of i32 arguments
 *     [10..11] uint16_t reserved       // Reserved (padding)
 *     [12..12+func_name_len-1] char func_name[]
 *     [aligned to 4 bytes]
 *     [arg_offset..arg_offset+argc*4-1] uint32_t argv[]
 *     [argv_end..argv_end+wasm_size-1]  uint8_t wasm_bytes[]
 *
 *   Mode 2 (invoke-only, reuse module):
 *     [0..3]   uint32_t wasm_size = 0  // No WASM bytecode
 *     [4..7]   uint32_t func_name_len  // Length of function name (> 0)
 *     [8..9]   uint16_t argc           // Number of i32 arguments
 *     [10..11] uint16_t reserved       // Reserved (padding)
 *     [12..12+func_name_len-1] char func_name[]
 *     [aligned to 4 bytes]
 *     [arg_offset..arg_offset+argc*4-1] uint32_t argv[]
 *     // No wasm_bytes
 *
 *   Mode 3 (shutdown):
 *     [0..3]   uint32_t wasm_size = 0
 *     [4..7]   uint32_t func_name_len = 0
 *     // No further data
 *
 * Output channel protocol (at 0x300_0000_0000):
 *   [0..3]   uint32_t status   // 0 = success, non-zero = error code
 *   [4..7]   uint32_t result   // Function return value (i32)
 *   [8..71]  uint8_t  env_hash[64]  // Phase 4: SHA-512 of instantiation environment
 *                                    // Written by wasmlet_invoke() before function call
 */

#include "pal_monitor_call.h"
#include "pal_malloc.h"
#include "pal_string.h"
#include "wasmlet_vmpl1.h"

/* Memory channel addresses (set by SVSM in process_cow.rs early_init) */
#define INPUT_CHANNEL_ADDR   0x28000000000ULL
#define OUTPUT_CHANNEL_ADDR  0x30000000000ULL

/* Output channel data format offsets (in bytes) */
#define OUTPUT_STATUS_OFFSET  0
#define OUTPUT_RESULT_OFFSET  4

/* Maximum function name length we support */
#define MAX_FUNC_NAME_LEN  128

/* ============================================================
 * Input channel protocol helpers
 * ============================================================ */

/* Input channel header layout */
struct input_header {
    uint32_t wasm_size;       /* [0..3]   */
    uint32_t func_name_len;   /* [4..7]   */
    uint16_t argc;            /* [8..9]   */
    uint16_t reserved;        /* [10..11] */
    /* Followed by: func_name, then argv, then wasm_bytes (if wasm_size > 0) */
};

#define INPUT_HEADER_SIZE  12  /* sizeof(struct input_header) */

/*
 * Align offset up to 4-byte boundary.
 */
static inline uint32_t align4(uint32_t v)
{
    return (v + 3) & ~(uint32_t)3;
}

/*
 * write_output_channel - Write execution result to the output channel
 *
 * The output channel is at a fixed virtual address (0x300_0000_0000).
 * During invoke_trustlet, SVSM inflates the output channel pages.
 *
 * Format:
 *   [0..3]  uint32_t status       // 0 = success, 1 = error
 *   [4..7]  uint32_t result       // function return value
 *   [8..71] uint8_t  env_hash[64] // Phase 4: written by wasmlet_invoke()
 *
 * Note: This function only writes status and result (offsets 0-7).
 * The env_hash at offset 8 is written by wasmlet_invoke() before
 * this function is called, and is preserved.
 */
static void write_output_channel(uint32_t status, uint32_t result)
{
    /*
     * Inflate the output channel (1 page = 4096 bytes).
     * This asks VMPL0 to allocate physical pages and map them
     * at OUTPUT_CHANNEL_ADDR in our page table.
     *
     * select=1 means output channel.
     *
     * Note: During invoke_trustlet, SVSM already calls inflate_output()
     * before resuming VMPL1. But we call inflate here as well for safety
     * (in case the output channel was not yet inflated).
     */
    int ret = pal_svsm_inflate_channel(1, 4096);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] WARNING: inflate output channel failed\n");
        /* Continue anyway - the pages might already be mapped */
    }

    volatile uint32_t *output = (volatile uint32_t *)OUTPUT_CHANNEL_ADDR;
    output[OUTPUT_STATUS_OFFSET / sizeof(uint32_t)] = status;
    output[OUTPUT_RESULT_OFFSET / sizeof(uint32_t)] = result;
}

/* ============================================================
 * PKRU debug helpers (MPK mode only)
 * ============================================================ */
#if ENABLE_MPK_ISOLATION
static void print_pkru(const char *label)
{
    uint32_t pkru = pal_svsm_mpk_query_pkru();
    pal_svsm_debug_print("[PKRU] ");
    pal_svsm_debug_print(label);
    pal_svsm_debug_print(": PKRU=");
    pal_svsm_debug_print_hex((uint64_t)pkru);
    pal_svsm_debug_print("\n");
}
#define PRINT_PKRU(label) print_pkru(label)
#else
#define PRINT_PKRU(label) ((void)0)
#endif

/* ============================================================
 * Module state tracking for Phase 3b module reuse
 * ============================================================ */

/* Track the WASM bytecode buffer so we can free it when unloading */
static uint8_t *g_wasm_buf = NULL;

/* ============================================================
 * wamr_pal_main - VMPL1 entry point (Phase 3b)
 *
 * Called by pal_start.S. Must not return (calls pal_svsm_exit).
 * ============================================================ */
void wamr_pal_main(void)
{
    int ret;

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] WAMR Runtime Phase 3b Starting (MPK="
#if ENABLE_MPK_ISOLATION
        "ON"
#else
        "OFF"
#endif
        ")\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    PRINT_PKRU("startup");

    /* =============================================================
     * Phase A: Initialization (runs during early_invoke / create_zygote)
     * ============================================================= */

    /* Step 1: Initialize heap (dlmalloc mspace, 16 MB) */
    pal_svsm_debug_print("[WAMR-PAL] Initializing heap...\n");
    ret = pal_heap_init();
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: Heap init failed\n");
        pal_svsm_exit(1);
        while (1) {}
    }
    pal_svsm_debug_print("[WAMR-PAL] Heap initialized (16 MB)\n");

    /* Step 2: Initialize WAMR runtime */
    pal_svsm_debug_print("[WAMR-PAL] Initializing WAMR runtime...\n");
    ret = wasmlet_runtime_init();
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: WAMR runtime init failed\n");
        pal_svsm_exit(1);
        while (1) {}
    }
    pal_svsm_debug_print("[WAMR-PAL] WAMR runtime initialized\n");
    PRINT_PKRU("after runtime_init");

    /* Step 3: Suspend VMPL1 — signal to SVSM that initialization is done.
     *
     * This triggers pal_svsm_exit(0) → CPUID 0x4FFFFFFE → SVSM sets
     * return_value = EXIT(0) → handle_process_request returns false →
     * early_invoke loop breaks → create_zygote returns to Guest.
     *
     * VMSA.RIP now points to the instruction after the CPUID in
     * monitor_call(). When invoke_trustlet calls ap_create to resume
     * VMPL1, execution continues right here — pal_svsm_exit returns
     * normally, and we fall through to Phase B below.
     */
    pal_svsm_debug_print("[WAMR-PAL] Initialization complete, suspending...\n");
    pal_svsm_exit(0);

    /* =============================================================
     * Phase B: Invocation loop (runs during invoke_trustlet)
     *
     * Phase 3b supports three modes:
     *   Mode 1: wasm_size > 0 → load new module + invoke function
     *   Mode 2: wasm_size == 0, func_name_len > 0 → invoke-only (reuse module)
     *   Mode 3: wasm_size == 0, func_name_len == 0 → shutdown
     * ============================================================= */

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Entered invocation loop (Phase 3b)\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    for (;;) {
        /* ---- Read input channel header ---- */
        volatile uint8_t *input = (volatile uint8_t *)INPUT_CHANNEL_ADDR;

        struct input_header hdr;
        hdr.wasm_size     = *(volatile uint32_t *)(input + 0);
        hdr.func_name_len = *(volatile uint32_t *)(input + 4);
        hdr.argc          = *(volatile uint16_t *)(input + 8);
        hdr.reserved      = 0;

        pal_svsm_debug_print("[WAMR-PAL] Input: wasm_size=");
        pal_svsm_debug_print_dec((int)hdr.wasm_size);
        pal_svsm_debug_print(", func_name_len=");
        pal_svsm_debug_print_dec((int)hdr.func_name_len);
        pal_svsm_debug_print(", argc=");
        pal_svsm_debug_print_dec((int)hdr.argc);
        pal_svsm_debug_print("\n");

        /* ---- Mode 3: Shutdown signal ---- */
        if (hdr.wasm_size == 0 && hdr.func_name_len == 0) {
            pal_svsm_debug_print("[WAMR-PAL] Received shutdown signal (wasm_size=0, func_name_len=0)\n");
            break;
        }

        /* Validate func_name_len (required for Mode 1 and Mode 2) */
        if (hdr.func_name_len == 0 || hdr.func_name_len > MAX_FUNC_NAME_LEN) {
            pal_svsm_debug_print("[WAMR-PAL] ERROR: invalid func_name_len\n");
            write_output_channel(1, 0);
            pal_svsm_get_result();
            continue;
        }

        /* ---- Mode 1: Load new module + invoke ---- */
        if (hdr.wasm_size > 0) {
            pal_svsm_debug_print("[WAMR-PAL] Mode 1: Load new module + invoke\n");

            /* If a module is already loaded, unload it first */
            if (g_wasm_buf) {
                pal_svsm_debug_print("[WAMR-PAL] Unloading previous module...\n");
                wasmlet_unload_module();
                pal_free(g_wasm_buf);
                g_wasm_buf = NULL;
            }

            /* Read function name */
            char func_name[MAX_FUNC_NAME_LEN + 1];
            uint32_t offset = INPUT_HEADER_SIZE;
            for (uint32_t i = 0; i < hdr.func_name_len; i++) {
                func_name[i] = (char)input[offset + i];
            }
            func_name[hdr.func_name_len] = '\0';
            offset += hdr.func_name_len;
            offset = align4(offset);

            pal_svsm_debug_print("[WAMR-PAL] Function: ");
            pal_svsm_debug_print(func_name);
            pal_svsm_debug_print("\n");

            /* Read arguments */
            uint32_t argv[16];
            uint16_t argc = hdr.argc;
            if (argc > 16) argc = 16;
            for (uint16_t i = 0; i < argc; i++) {
                argv[i] = *(volatile uint32_t *)(input + offset);
                offset += 4;
            }

            /* Read WASM bytecode */
            uint32_t wasm_offset = offset;
            uint32_t wasm_size = hdr.wasm_size;

            pal_svsm_debug_print("[WAMR-PAL] WASM bytecode at input offset ");
            pal_svsm_debug_print_dec((int)wasm_offset);
            pal_svsm_debug_print(", size=");
            pal_svsm_debug_print_dec((int)wasm_size);
            pal_svsm_debug_print("\n");

            /*
             * Copy WASM bytecode to writable heap buffer.
             * WAMR's wasm_runtime_load() modifies the buffer in-place
             * (byte-order swaps, internal patching), so we cannot use
             * the input channel memory directly.
             */
            g_wasm_buf = (uint8_t *)pal_malloc(wasm_size);
            if (!g_wasm_buf) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: Failed to allocate WASM buffer\n");
                write_output_channel(2, 0);
                pal_svsm_get_result();
                continue;
            }
            for (uint32_t i = 0; i < wasm_size; i++) {
                g_wasm_buf[i] = input[wasm_offset + i];
            }

            /* Load WASM module */
            pal_svsm_debug_print("[WAMR-PAL] Loading WASM module...\n");
            PRINT_PKRU("before module_load");

            ret = wasmlet_load_module(g_wasm_buf, wasm_size);
            if (ret != 0) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: Module load failed\n");
                pal_free(g_wasm_buf);
                g_wasm_buf = NULL;
                write_output_channel(3, 0);
                pal_svsm_get_result();
                continue;
            }
            pal_svsm_debug_print("[WAMR-PAL] Module loaded OK\n");
            PRINT_PKRU("after module_load");

            /* Invoke function */
            uint32_t result = 0;

            pal_svsm_debug_print("[WAMR-PAL] Invoking ");
            pal_svsm_debug_print(func_name);
            pal_svsm_debug_print("(");
            for (uint16_t i = 0; i < argc; i++) {
                if (i > 0) pal_svsm_debug_print(", ");
                pal_svsm_debug_print_dec((int)argv[i]);
            }
            pal_svsm_debug_print(")...\n");

            ret = wasmlet_invoke(func_name, (int)argc, argv, &result);
            if (ret != 0) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: Invoke failed\n");
                /* Keep module loaded — caller can retry or send shutdown */
                write_output_channel(4, 0);
                pal_svsm_get_result();
                continue;
            }

            pal_svsm_debug_print("[WAMR-PAL] Result: ");
            pal_svsm_debug_print_dec((int)result);
            pal_svsm_debug_print("\n");
            PRINT_PKRU("after invoke");

            /* Write result to output channel */
            write_output_channel(0, result);

            pal_svsm_debug_print("[WAMR-PAL] Output written: status=0, result=");
            pal_svsm_debug_print_dec((int)result);
            pal_svsm_debug_print("\n");

        } else {
            /* ---- Mode 2: Invoke-only (reuse loaded module) ---- */
            pal_svsm_debug_print("[WAMR-PAL] Mode 2: Invoke-only (reuse module)\n");

            /* Check that a module is loaded */
            if (!g_wasm_buf) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: No module loaded for invoke-only\n");
                write_output_channel(5, 0);  /* status=5: no module loaded */
                pal_svsm_get_result();
                continue;
            }

            /* Read function name */
            char func_name[MAX_FUNC_NAME_LEN + 1];
            uint32_t offset = INPUT_HEADER_SIZE;
            for (uint32_t i = 0; i < hdr.func_name_len; i++) {
                func_name[i] = (char)input[offset + i];
            }
            func_name[hdr.func_name_len] = '\0';
            offset += hdr.func_name_len;
            offset = align4(offset);

            pal_svsm_debug_print("[WAMR-PAL] Function: ");
            pal_svsm_debug_print(func_name);
            pal_svsm_debug_print("\n");

            /* Read arguments */
            uint32_t argv[16];
            uint16_t argc = hdr.argc;
            if (argc > 16) argc = 16;
            for (uint16_t i = 0; i < argc; i++) {
                argv[i] = *(volatile uint32_t *)(input + offset);
                offset += 4;
            }

            /* Invoke function on already-loaded module */
            uint32_t result = 0;

            pal_svsm_debug_print("[WAMR-PAL] Invoking ");
            pal_svsm_debug_print(func_name);
            pal_svsm_debug_print("(");
            for (uint16_t i = 0; i < argc; i++) {
                if (i > 0) pal_svsm_debug_print(", ");
                pal_svsm_debug_print_dec((int)argv[i]);
            }
            pal_svsm_debug_print(")...\n");

            ret = wasmlet_invoke(func_name, (int)argc, argv, &result);
            if (ret != 0) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: Invoke failed\n");
                write_output_channel(4, 0);
                pal_svsm_get_result();
                continue;
            }

            pal_svsm_debug_print("[WAMR-PAL] Result: ");
            pal_svsm_debug_print_dec((int)result);
            pal_svsm_debug_print("\n");
            PRINT_PKRU("after invoke");

            /* Write result to output channel */
            write_output_channel(0, result);

            pal_svsm_debug_print("[WAMR-PAL] Output written: status=0, result=");
            pal_svsm_debug_print_dec((int)result);
            pal_svsm_debug_print("\n");
        }

        /* ---- Notify SVSM results are ready ---- */
        pal_svsm_debug_print("[WAMR-PAL] Calling pal_svsm_get_result()...\n");
        pal_svsm_get_result();

        /*
         * pal_svsm_get_result() triggers CPUID 0x4FFFFFF8.
         * SVSM's handler:
         *   1. copy_out(result_addr, guest_page_table, result_size)
         *      → copies output channel to Guest's return_buffer
         *   2. return_value = GETRESULT (1)
         *   3. returns false → invoke_trustlet loop breaks
         *   4. Guest ioctl returns 1 (invocationGetValue)
         *   5. Guest C code returns return_buffer pointer
         *   6. Python gets bytes
         *
         * When the next invoke_trustlet_bin is called from Guest,
         * SVSM resumes VMPL1 via ap_create, and pal_svsm_get_result()
         * returns here. We loop back to read the next input.
         */
        pal_svsm_debug_print("[WAMR-PAL] Resumed for next invocation\n");
    }

    /* =============================================================
     * Cleanup and final exit (reached on Mode 3 shutdown)
     * ============================================================= */
    pal_svsm_debug_print("[WAMR-PAL] Shutting down...\n");

    /* Unload module if still loaded */
    if (g_wasm_buf) {
        wasmlet_unload_module();
        pal_free(g_wasm_buf);
        g_wasm_buf = NULL;
    }

    pal_svsm_debug_print("[WAMR-PAL] Destroying WAMR runtime...\n");
    wasmlet_runtime_destroy();

    PRINT_PKRU("after cleanup");

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Phase 3b complete. Exiting.\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    pal_svsm_exit(0);

    /* Should never reach here */
    while (1) {}
}
