/*
 * wamr_pal_main.c - VMPL1 entry point using wasmlet library
 *
 * Phase 3b execution model with wasmlet.h API:
 *   Phase A (early_invoke): heap_init + wasmlet_runtime_init + suspend
 *   Phase B (invoke_trustlet loop):
 *     Mode 1: wasmlet_init + wasmlet_run (load + invoke)
 *     Mode 2: wasmlet_run (invoke-only, reuse module)
 *     Mode 3: shutdown
 */

#include "pal_monitor_call.h"
#include "pal_malloc.h"
#include "pal_string.h"
#include "wasmlet.h"
#include "wasmlet_platform.h"

#define INPUT_CHANNEL_ADDR   0x28000000000ULL
#define OUTPUT_CHANNEL_ADDR  0x30000000000ULL
#define MAX_FUNC_NAME_LEN   128
#define INPUT_HEADER_SIZE    12

struct input_header {
    uint32_t wasm_size;
    uint32_t func_name_len;
    uint16_t argc;
    uint16_t reserved;
};

static inline uint32_t align4(uint32_t v) {
    return (v + 3) & ~(uint32_t)3;
}

static void write_output_channel(uint32_t status, uint32_t result) {
    int ret = pal_svsm_inflate_channel(1, 4096);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] WARNING: inflate output channel failed\n");
    }

    volatile uint32_t *output = (volatile uint32_t *)OUTPUT_CHANNEL_ADDR;
    output[0] = status;
    output[1] = result;
}

#if ENABLE_MPK_ISOLATION
static void print_pkru(const char *label) {
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

static uint32_t g_module_id = 0;
static int g_module_loaded = 0;

/* ---- Thread smoke test ---- */

static volatile uint64_t shared_result = 0;

static void *thread_worker(void *arg) {
    uint64_t val = (uint64_t)(uintptr_t)arg;
    pal_svsm_debug_print("[THREAD-TEST] Worker running, arg=");
    pal_svsm_debug_print_dec((int)val);
    pal_svsm_debug_print("\n");

    shared_result = val * val;

    pal_svsm_debug_print("[THREAD-TEST] Worker computed result=");
    pal_svsm_debug_print_dec((int)shared_result);
    pal_svsm_debug_print("\n");

    return (void *)(uintptr_t)(val + 1);
}

static void thread_smoke_test(void) {
    wasmlet_thread_t tid;
    int ret = wasmlet_thread_create(&tid, thread_worker, (void *)7, 0);
    if (ret != 0) {
        pal_svsm_debug_print("[THREAD-TEST] FAIL: thread_create returned error\n");
        pal_svsm_exit(1);
        return;
    }
    pal_svsm_debug_print("[THREAD-TEST] Created thread id=");
    pal_svsm_debug_print_dec((int)tid);
    pal_svsm_debug_print("\n");

    void *retval = (void *)0;
    ret = wasmlet_thread_join(tid, &retval);
    if (ret != 0) {
        pal_svsm_debug_print("[THREAD-TEST] FAIL: thread_join returned error\n");
        pal_svsm_exit(1);
        return;
    }

    pal_svsm_debug_print("[THREAD-TEST] Join returned, retval=");
    pal_svsm_debug_print_dec((int)(uintptr_t)retval);
    pal_svsm_debug_print("\n");

    if (shared_result != 49) {
        pal_svsm_debug_print("[THREAD-TEST] FAIL: shared_result != 49\n");
        pal_svsm_exit(1);
        return;
    }
    if ((uint64_t)(uintptr_t)retval != 8) {
        pal_svsm_debug_print("[THREAD-TEST] FAIL: retval != 8\n");
        pal_svsm_exit(1);
        return;
    }
    pal_svsm_debug_print("[THREAD-TEST] All assertions passed\n");
}

/* ---- End thread smoke test ---- */

void wamr_pal_main(void)
{
    int ret;

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] WAMR Runtime Starting (wasmlet)\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    PRINT_PKRU("startup");

    /* Phase A: Initialization */
    pal_svsm_debug_print("[WAMR-PAL] Initializing heap...\n");
    ret = pal_heap_init();
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: Heap init failed\n");
        pal_svsm_exit(1);
        while (1) {}
    }

    /* Create config: max_threads=0 means no thread pool (single-threaded) */
    wasmlet_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_threads = 0;
    config.thread_stack_size = 32 * 1024;
    config.max_heap_size = 32 * 1024;
    config.lf_queue_size = 64;
    config.pkey_pool_size = 15;
    config.mpk_module_heap_size = 4 * 1024 * 1024;
    config.mpk_exec_heap_size = 8 * 1024 * 1024;
    config.log_level = 1; /* INFO */

    pal_svsm_debug_print("[WAMR-PAL] Initializing wasmlet runtime...\n");
    ret = wasmlet_runtime_init(&config);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] FATAL: wasmlet_runtime_init failed\n");
        pal_svsm_exit(1);
        while (1) {}
    }
    pal_svsm_debug_print("[WAMR-PAL] Runtime initialized\n");
    PRINT_PKRU("after runtime_init");

    /* ---- Thread smoke test ---- */
    pal_svsm_debug_print("[THREAD-TEST] Starting thread test...\n");
    thread_smoke_test();
    pal_svsm_debug_print("[THREAD-TEST] Thread test passed!\n");
    /* ---- End thread test ---- */

    pal_svsm_debug_print("[WAMR-PAL] Initialization complete, suspending...\n");
    pal_svsm_exit(0);

    /* Phase B: Invocation loop */
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Entered invocation loop\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    for (;;) {
        volatile uint8_t *input = (volatile uint8_t *)INPUT_CHANNEL_ADDR;

        struct input_header hdr;
        hdr.wasm_size     = *(volatile uint32_t *)(input + 0);
        hdr.func_name_len = *(volatile uint32_t *)(input + 4);
        hdr.argc          = *(volatile uint16_t *)(input + 8);

        pal_svsm_debug_print("[WAMR-PAL] Input: wasm_size=");
        pal_svsm_debug_print_dec((int)hdr.wasm_size);
        pal_svsm_debug_print(", func_name_len=");
        pal_svsm_debug_print_dec((int)hdr.func_name_len);
        pal_svsm_debug_print(", argc=");
        pal_svsm_debug_print_dec((int)hdr.argc);
        pal_svsm_debug_print("\n");

        /* Mode 3: Shutdown */
        if (hdr.wasm_size == 0 && hdr.func_name_len == 0) {
            pal_svsm_debug_print("[WAMR-PAL] Received shutdown signal\n");
            break;
        }

        if (hdr.func_name_len == 0 || hdr.func_name_len > MAX_FUNC_NAME_LEN) {
            pal_svsm_debug_print("[WAMR-PAL] ERROR: invalid func_name_len\n");
            write_output_channel(1, 0);
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

        /* Read arguments */
        uint32_t argv[16];
        uint16_t argc = hdr.argc;
        if (argc > 16) argc = 16;
        for (uint16_t i = 0; i < argc; i++) {
            argv[i] = *(volatile uint32_t *)(input + offset);
            offset += 4;
        }

        /* Mode 1: Load new module + invoke */
        if (hdr.wasm_size > 0) {
            pal_svsm_debug_print("[WAMR-PAL] Mode 1: Load + invoke\n");

            if (g_module_loaded) {
                wasmlet_unload(g_module_id);
                g_module_loaded = 0;
            }

            /* Copy WASM bytecode to heap (WAMR modifies buffer in-place) */
            uint32_t wasm_size = hdr.wasm_size;
            uint8_t *wasm_buf = (uint8_t *)pal_malloc(wasm_size);
            if (!wasm_buf) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: malloc wasm buffer failed\n");
                write_output_channel(2, 0);
                pal_svsm_get_result();
                continue;
            }
            for (uint32_t i = 0; i < wasm_size; i++) {
                wasm_buf[i] = input[offset + i];
            }

            PRINT_PKRU("before wasmlet_init");
            ret = wasmlet_init(wasm_buf, wasm_size, &g_module_id);
            pal_free(wasm_buf);

            if (ret != 0) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: wasmlet_init failed\n");
                write_output_channel(3, 0);
                pal_svsm_get_result();
                continue;
            }
            g_module_loaded = 1;
            pal_svsm_debug_print("[WAMR-PAL] Module loaded OK\n");
            PRINT_PKRU("after wasmlet_init");
        } else {
            /* Mode 2: Invoke-only */
            pal_svsm_debug_print("[WAMR-PAL] Mode 2: Invoke-only\n");

            if (!g_module_loaded) {
                pal_svsm_debug_print("[WAMR-PAL] ERROR: No module loaded\n");
                write_output_channel(5, 0);
                pal_svsm_get_result();
                continue;
            }
        }

        /* Convert uint32 args to uint64 for wasmlet_run */
        uint64_t args64[16];
        for (uint16_t i = 0; i < argc; i++) {
            args64[i] = (uint64_t)argv[i];
        }

        pal_svsm_debug_print("[WAMR-PAL] Invoking ");
        pal_svsm_debug_print(func_name);
        pal_svsm_debug_print("...\n");

        execution_result_t result;
        ret = wasmlet_run(g_module_id, func_name, args64, (uint32_t)argc, &result);

        if (ret != 0 || result.status != EXEC_STATUS_SUCCESS) {
            pal_svsm_debug_print("[WAMR-PAL] ERROR: wasmlet_run failed\n");
            write_output_channel(4, 0);
        } else {
            pal_svsm_debug_print("[WAMR-PAL] Result: ");
            pal_svsm_debug_print_dec((int)result.return_value);
            pal_svsm_debug_print("\n");
            write_output_channel(0, (uint32_t)result.return_value);
        }
        PRINT_PKRU("after invoke");

        pal_svsm_debug_print("[WAMR-PAL] Calling pal_svsm_get_result()...\n");
        pal_svsm_get_result();
        pal_svsm_debug_print("[WAMR-PAL] Resumed for next invocation\n");
    }

    /* Cleanup */
    pal_svsm_debug_print("[WAMR-PAL] Shutting down...\n");
    if (g_module_loaded) {
        wasmlet_unload(g_module_id);
        g_module_loaded = 0;
    }
    wasmlet_runtime_destroy();
    PRINT_PKRU("after cleanup");

    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Complete. Exiting.\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    pal_svsm_exit(0);
    while (1) {}
}
