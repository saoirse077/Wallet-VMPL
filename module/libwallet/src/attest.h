#ifndef ATTEST_H_
#define ATTEST_H_

#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <stdlib.h>

#include "monitor.h"

#define  PACKED __attribute__((__packed__)) 

// this struct is allocated with alignment requirements!
typedef struct PACKED _function_data {
  uint64_t trustletId; // 8 bytes
  uint64_t moduleId;   // 8 bytes — Phase 4 新增
  uint64_t fnInputSize; // 8 bytes
  void* fnInput; // 8 bytes
  uint64_t fnOutputSize; // 8 bytes
  void* fnOutput; // 8 bytes
  void* reportOutput;
} function_data;

/* Attestation API */

char* attest_monitor();
char* attest_wamr_runtime(const uint64_t process_id);
char* attest_wasm_module(const uint64_t process_id, const uint64_t module_id);
char* attest_execution(const uint64_t trusted_process_id, const uint64_t module_id,
                        const char* input, const uint64_t input_len, 
                        const char* output, const uint64_t output_len);

/* End of Attestation API */

#endif // ATTEST_H_
