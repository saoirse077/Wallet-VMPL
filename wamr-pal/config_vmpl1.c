/*
 * config_vmpl1.c - Minimal config stubs for wasmlet on VMPL1
 *
 * On VMPL1, config is always provided by wamr_pal_main.c (stack-allocated).
 * We only need config_free() (called by wasmlet_runtime_destroy).
 */

#include "config.h"
#include "pal_malloc.h"
#include "pal_string.h"

wasmlet_config_t *config_create_default(void) {
    wasmlet_config_t *config = (wasmlet_config_t *)pal_calloc(1, sizeof(wasmlet_config_t));
    if (!config) return (void *)0;

    config->max_threads = 0;
    config->thread_stack_size = 32 * 1024;
    config->module_memory_pages = 256;
    config->module_max_size = 10 * 1024 * 1024;
    config->timeout_ms = 5000;
    config->max_stack_depth = 1024;
    config->max_heap_size = 32 * 1024;
    config->max_total_memory = 16 * 1024 * 1024;
    config->log_level = 1; /* INFO */
    config->lf_queue_size = 64;
    config->pkey_pool_size = 15;
    config->mpk_module_heap_size = 4 * 1024 * 1024;
    config->mpk_exec_heap_size = 8 * 1024 * 1024;

    return config;
}

int config_load_from_file(const char *file_path, wasmlet_config_t *config) {
    (void)file_path; (void)config;
    return -1;
}

int config_validate(const wasmlet_config_t *config) {
    if (!config) return -1;
    return 0;
}

void config_free(wasmlet_config_t *config) {
    if (config) pal_free(config);
}

void config_print(const wasmlet_config_t *config) {
    (void)config;
}
