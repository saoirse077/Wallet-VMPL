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
    uint16_t command;
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
static int g_workers_started = 0;
static uint64_t g_thread_capacity = 0;

static void handle_sync_invoke(struct input_header *hdr, volatile uint8_t *input);
static void handle_load_module(struct input_header *hdr, volatile uint8_t *input);
static void handle_submit_task(struct input_header *hdr, volatile uint8_t *input);
static void handle_get_result(struct input_header *hdr, volatile uint8_t *input);

static void ensure_workers_started(void)
{
    if (g_workers_started || g_thread_capacity == 0)
        return;
    pal_svsm_debug_print("[WAMR-PAL] Lazy-starting workers: ");
    pal_svsm_debug_print_dec((int)g_thread_capacity);
    pal_svsm_debug_print("\n");
    int ret = wasmlet_start_workers((uint32_t)g_thread_capacity);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] WARN: failed to start workers\n");
    }
    g_workers_started = 1;
}

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

    g_thread_capacity = pal_svsm_query_thread_capacity();
    pal_svsm_debug_print("[WAMR-PAL] Thread capacity: ");
    pal_svsm_debug_print_dec((int)g_thread_capacity);
    pal_svsm_debug_print("\n");

    wasmlet_config_t config;
    memset(&config, 0, sizeof(config));
    config.max_threads = 0; /* Phase A: no workers (CoW safety) */
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

    pal_svsm_debug_print("[WAMR-PAL] Initialization complete, suspending...\n");
    pal_svsm_exit(0);

    /* Phase B: Enter command loop. Workers start lazily on first async op. */
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");
    pal_svsm_debug_print("[WAMR-PAL] Phase B: command dispatch ready\n");
    pal_svsm_debug_print("[WAMR-PAL] ================================\n");

    int should_exit = 0;
    for (;;) {
        volatile uint8_t *input = (volatile uint8_t *)INPUT_CHANNEL_ADDR;

        struct input_header hdr;
        hdr.wasm_size     = *(volatile uint32_t *)(input + 0);
        hdr.func_name_len = *(volatile uint32_t *)(input + 4);
        hdr.argc          = *(volatile uint16_t *)(input + 8);
        hdr.command        = *(volatile uint16_t *)(input + 10);

        switch (hdr.command) {
        case 0:
            handle_sync_invoke(&hdr, input);
            break;
        case 1:
            handle_load_module(&hdr, input);
            break;
        case 2:
            handle_submit_task(&hdr, input);
            break;
        case 3:
            handle_get_result(&hdr, input);
            break;
        case 0xFF:
            pal_svsm_debug_print("[WAMR-PAL] DESTROY: stopping runtime...\n");
            if (g_module_loaded) {
                wasmlet_unload(g_module_id);
                g_module_loaded = 0;
            }
            if (g_workers_started) {
                pal_svsm_debug_print("[WAMR-PAL] DESTROY: joining workers...\n");
                wasmlet_runtime_destroy();
                g_workers_started = 0;
            } else {
                wasmlet_runtime_destroy();
            }
            write_output_channel(0, 0);
            should_exit = 1;
            break;
        default:
            pal_svsm_debug_print("[WAMR-PAL] Unknown command\n");
            write_output_channel(0xFF, 0);
            break;
        }

        pal_svsm_get_result();
        if (should_exit) {
            pal_svsm_exit(0);
            break;
        }
    }
    while (1) {}
}

/* ========== Command handlers ========== */

static void handle_sync_invoke(struct input_header *hdr, volatile uint8_t *input)
{
    int ret;

    /* Mode 3: Lightweight shutdown (legacy) — unload module, write output,
     * exit process. Does NOT call wasmlet_runtime_destroy() to avoid
     * potential deadlock from thread_join on workers that never ran. */
    if (hdr->wasm_size == 0 && hdr->func_name_len == 0) {
        pal_svsm_debug_print("[WAMR-PAL] Legacy shutdown (lightweight)\n");
        if (g_module_loaded) {
            wasmlet_unload(g_module_id);
            g_module_loaded = 0;
        }
        write_output_channel(0, 0);
        pal_svsm_get_result();
        pal_svsm_exit(0);
        return;
    }

    if (hdr->func_name_len == 0 || hdr->func_name_len > MAX_FUNC_NAME_LEN) {
        write_output_channel(1, 0);
        return;
    }

    char func_name[MAX_FUNC_NAME_LEN + 1];
    uint32_t offset = INPUT_HEADER_SIZE;
    for (uint32_t i = 0; i < hdr->func_name_len; i++) {
        func_name[i] = (char)input[offset + i];
    }
    func_name[hdr->func_name_len] = '\0';
    offset += hdr->func_name_len;
    offset = align4(offset);

    uint32_t argv[16];
    uint16_t argc = hdr->argc;
    if (argc > 16) argc = 16;
    for (uint16_t i = 0; i < argc; i++) {
        argv[i] = *(volatile uint32_t *)(input + offset);
        offset += 4;
    }

    if (hdr->wasm_size > 0) {
        if (g_module_loaded) {
            wasmlet_unload(g_module_id);
            g_module_loaded = 0;
        }
        uint32_t wasm_size = hdr->wasm_size;
        uint8_t *wasm_buf = (uint8_t *)pal_malloc(wasm_size);
        if (!wasm_buf) {
            write_output_channel(2, 0);
            return;
        }
        for (uint32_t i = 0; i < wasm_size; i++) {
            wasm_buf[i] = input[offset + i];
        }
        ret = wasmlet_init(wasm_buf, wasm_size, &g_module_id);
        pal_free(wasm_buf);
        if (ret != 0) {
            write_output_channel(3, 0);
            return;
        }
        g_module_loaded = 1;
    } else {
        if (!g_module_loaded) {
            write_output_channel(5, 0);
            return;
        }
    }

    uint64_t args64[16];
    for (uint16_t i = 0; i < argc; i++) {
        args64[i] = (uint64_t)argv[i];
    }

    execution_result_t result;
    ret = wasmlet_run(g_module_id, func_name, args64, (uint32_t)argc, &result);

    if (ret != 0 || result.status != EXEC_STATUS_SUCCESS) {
        write_output_channel(4, 0);
    } else {
        write_output_channel(0, (uint32_t)result.return_value);
    }
}

static void handle_load_module(struct input_header *hdr, volatile uint8_t *input)
{
    uint32_t wasm_size = hdr->wasm_size;
    if (wasm_size == 0) {
        write_output_channel(1, 0);
        return;
    }

    uint8_t *wasm_buf = (uint8_t *)pal_malloc(wasm_size);
    if (!wasm_buf) {
        write_output_channel(2, 0);
        return;
    }
    for (uint32_t i = 0; i < wasm_size; i++) {
        wasm_buf[i] = input[INPUT_HEADER_SIZE + i];
    }

    uint32_t module_id;
    int ret = wasmlet_init(wasm_buf, wasm_size, &module_id);
    pal_free(wasm_buf);

    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] LOAD_MODULE failed\n");
        write_output_channel(3, 0);
    } else {
        pal_svsm_debug_print("[WAMR-PAL] LOAD_MODULE ok, id=");
        pal_svsm_debug_print_dec((int)module_id);
        pal_svsm_debug_print("\n");
        write_output_channel(0, module_id);
    }
}

static void handle_submit_task(struct input_header *hdr, volatile uint8_t *input)
{
    ensure_workers_started();
    uint32_t offset = INPUT_HEADER_SIZE;
    uint32_t module_id = *(volatile uint32_t *)(input + offset);
    offset += 4;

    char func_name[256];
    uint32_t name_len = hdr->func_name_len;
    if (name_len == 0 || name_len > 255) {
        write_output_channel(1, 0);
        return;
    }
    for (uint32_t i = 0; i < name_len; i++) {
        func_name[i] = (char)input[offset + i];
    }
    func_name[name_len] = '\0';
    uint32_t name_padded = (name_len + 3) & ~(uint32_t)3;
    offset += name_padded;

    uint16_t argc = hdr->argc;
    uint64_t args64[32];
    for (uint16_t i = 0; i < argc && i < 32; i++) {
        args64[i] = (uint64_t)(*(volatile uint32_t *)(input + offset));
        offset += 4;
    }

    uint32_t request_id;
    int ret = wasmlet_run_async(module_id, func_name, args64, argc, &request_id);
    if (ret != 0) {
        pal_svsm_debug_print("[WAMR-PAL] SUBMIT_TASK failed\n");
        write_output_channel(4, 0);
    } else {
        pal_svsm_debug_print("[WAMR-PAL] SUBMIT_TASK ok, req_id=");
        pal_svsm_debug_print_dec((int)request_id);
        pal_svsm_debug_print("\n");
        write_output_channel(0, request_id);
    }
}

static void handle_get_result(struct input_header *hdr, volatile uint8_t *input)
{
    (void)hdr;
    uint32_t request_id = *(volatile uint32_t *)(input + INPUT_HEADER_SIZE);

    execution_result_t result;
    int ret = wasmlet_get_result(request_id, &result);

    if (ret == WASMLET_SUCCESS) {
        write_output_channel(0, (uint32_t)result.return_value);
    } else if (ret == WASMLET_ERROR_PENDING) {
        write_output_channel(0xFD, 0);
    } else {
        write_output_channel((uint32_t)(-ret), 0);
    }
}
