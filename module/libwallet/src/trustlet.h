#ifndef TRUSTLET_H_
#define TRUSTLET_H_

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "monitor.h"

struct data {
    void* ptr;
    uint64_t size;
};

struct trustlet_invokation {
    uint64_t invokation_type;
    struct data function_arg;
    struct data result;
    struct data guest_request_args;
};

struct guest_request_args {
    union {
        struct {
            char path[256];
            uint64_t size;
            uint32_t mode;
            int32_t ret;
        } fileattr;
        struct {
            char path[256];
            uint32_t fd;
        } open;
        struct {
            char buf[1024];
            uint64_t count;
            uint64_t offset;
            uint32_t fd;
        } read;
        struct {
            uint64_t fd; // use uint64_t so that the monitor
                         // can access all fields using u64
                         // sice
            uint64_t offset;
            uint64_t size;
            uint64_t addr_offset;
            uint64_t buf_addr;
        } mmap;
    };
};

enum invocation_type {
    normalInvocation = 0,
    requestFileattr,
    requestOpen,
    requestRead,
    requestMmap,
};

enum invocation_return_type {
    invocationExit = 0,
    invocationGetValue = 1,
    invocationError = 2,
    guestRequestFileattr = 3,
    guestRequestOpen = 4,
    guestRequestRead = 5,
    guestRequestMmap = 6,
};


int create_trustlet(const int zygote_id, char* func);
char* invoke_trustlet(const int trustlet_id, char* args, uint64_t output_size);
char* invoke_trustlet_bin(const int trustlet_id, void* args, uint64_t args_size, uint64_t output_size);
int delete_trustlet(const int trustlet_id);
void create_channel(const int trustlet_id_1, const int trustlet_id_2);

#define TRUSTLET_CMD_SYNC_INVOKE  0
#define TRUSTLET_CMD_LOAD_MODULE  1
#define TRUSTLET_CMD_SUBMIT_TASK  2
#define TRUSTLET_CMD_GET_RESULT   3
#define TRUSTLET_CMD_DESTROY      0xFF

#define TRUSTLET_RESULT_PENDING   0xFD

int trustlet_load_module(int trustlet_id,
                         const void *wasm_data, uint64_t wasm_size,
                         uint32_t *module_id);

int trustlet_submit_task(int trustlet_id, uint32_t module_id,
                         const char *func_name,
                         const uint32_t *argv, uint16_t argc,
                         uint32_t *request_id);

int trustlet_get_result(int trustlet_id, uint32_t request_id,
                        uint32_t *out_status, uint32_t *out_value);

int trustlet_destroy_runtime(int trustlet_id);

#endif // TRUSTLET_H_
