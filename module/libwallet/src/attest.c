#include "attest.h"
#include "vmpl.h"

#define PAGE_SIZE 4096

/* Attestation helper functions */

// invoked function counter for report dumping
// Note: Used only in debugging mode
#ifndef NODEBUG
static int attested_function_cnt = 0;
#endif

// Function to print the buffer as hex
static void print_buffer_hex(FILE *file, const uint8_t *buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        fprintf(file, "%02X", buffer[i]);
        if ((i + 1) % 16 == 0) fprintf(file, "\n"); // Line break every 16 bytes
    }
    if (size % 16 != 0) fprintf(file, "\n"); // Final line break if not multiple of 16
}

// Function to parse and print the attestation report
static void print_attestation_report(const uint8_t *att_buffer, FILE *file) {
    size_t field_count = sizeof(fields) / sizeof(fields[0]);
    for (size_t i = 0; i < field_count; i++) {
        const Field *field = &fields[i];
        fprintf(file, "%s (Offset: 0x%02lX, Size: %lu bytes):\n",
                field->name, field->offset, field->size);
        print_buffer_hex(file, att_buffer + field->offset, field->size);
        fprintf(file, "\n");
    }
}

// Function to allocate the struct for the function attestation arguments
static void allocate_function_struct(function_data** fn){
    uint8_t* buf = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (buf == NULL) {
        printf("Can't allocate function struct for the attestation\n");
        *fn = NULL;
        return;
    }
    for(int i = 0; i < PAGE_SIZE; i++) {
        buf[i] = 0;
    }
    *fn = (void*)buf;
}

/* End of Attestation helper functions */

/**
 * Performs monitor attestation.
 * Warning: leaking the report buffer memory (or the output file pathname in debug mode).
 * @return Pointer to the attestation report buffer (release) or report path (in debug mode)
 */
char* attest_monitor() {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate monitor attestation report buffer\n");
      return NULL;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = monitorAttestation;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;

  #ifndef NODEBUG
  // Open a file for writing the report
  FILE *output_file = fopen(MONITOR_ATTESTATION_REPORT_PATH, "w");
  if (output_file == NULL) {
      perror("Error opening file");
      return NULL;
  }
  print_attestation_report(att_buffer, output_file);
  free(att_buffer);
  // Close the output file
  fclose(output_file);
  return MONITOR_ATTESTATION_REPORT_PATH;
  #else
  return (char*)report;
  #endif
}

/**
 * Phase 4: Performs WAMR runtime attestation.
 * @param process_id ID of the process (Zygote/Trustlet) to attest.
 * @return Pointer to the attestation report buffer (release) or report path (in debug mode)
 */
char* attest_wamr_runtime(const uint64_t process_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WAMR runtime attestation report buffer\n");
      return NULL;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = wamrRuntimeAttestation;
  call.monitor_attestation.process_id = process_id;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;

  #ifndef NODEBUG
  FILE *output_file = fopen(WAMR_RUNTIME_ATTESTATION_REPORT_PATH, "w");
  if (output_file == NULL) {
      perror("Error opening file");
      return NULL;
  }
  print_attestation_report(att_buffer, output_file);
  free(att_buffer);
  fclose(output_file);
  return WAMR_RUNTIME_ATTESTATION_REPORT_PATH;
  #else
  return (char*)report;
  #endif
}

/**
 * Phase 4: Performs WASM module attestation.
 * @param process_id ID of the process (Trustlet) to attest.
 * @param module_id Index of the WASM module to attest.
 * @return Pointer to the attestation report buffer (release) or report path (in debug mode)
 */
char* attest_wasm_module(const uint64_t process_id, const uint64_t module_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WASM module attestation report buffer\n");
      return NULL;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = wasmModuleAttestation;
  call.monitor_attestation.process_id = process_id;
  call.monitor_attestation.module_id = module_id;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;

  #ifndef NODEBUG
  FILE *output_file = fopen(WASM_MODULE_ATTESTATION_REPORT_PATH, "w");
  if (output_file == NULL) {
      perror("Error opening file");
      return NULL;
  }
  print_attestation_report(att_buffer, output_file);
  free(att_buffer);
  fclose(output_file);
  return WASM_MODULE_ATTESTATION_REPORT_PATH;
  #else
  return (char*)report;
  #endif
}

/**
 * Performs function execution attestation.
 * Warning: leaking the report buffer memory (or the output file pathname in debug mode).
 * @param trusted_process_id ID of the trusted process to attest.
 * @param module_id Index of the WASM module (Phase 4 新增).
 * @param input Pointer to function input data.
 * @param input_len Size of the input data.
 * @param output Pointer to function output data.
 * @param output_len Size of the output data.
 * @return Pointer to the attestation report buffer (release) or report path (in debug mode)
 */
char* attest_execution(const uint64_t trusted_process_id, const uint64_t module_id,
                        const char* input, const uint64_t input_len, 
                        const char* output, const uint64_t output_len)
{
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
    printf("Can't allocate trustlet attestation report buffer\n");
    return NULL;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  function_data* function_data_ptr;
  allocate_function_struct(&function_data_ptr);
  function_data_ptr->trustletId = trusted_process_id;
  function_data_ptr->moduleId = module_id;
  function_data_ptr->fnInput = (void*)input;
  function_data_ptr->fnInputSize = input_len;
  function_data_ptr->fnOutput = (void*)output;
  function_data_ptr->fnOutputSize = output_len;

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = functionAttestation;
  call.monitor_attestation.process_id = trusted_process_id;
  call.monitor_attestation.function_data_ptr = (void*)function_data_ptr;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;
  
  #ifndef NODEBUG
  /* Warning: the out_filename memory is leaked! (debugging purposes)*/
  char* out_filename = (char*)malloc(MAX_PATH_SIZE * sizeof(char));
  snprintf(out_filename, MAX_PATH_SIZE, "%s%d", FUNCTION_ATTESTATION_REPORT_PATH, attested_function_cnt);
  attested_function_cnt++;
  FILE *output_file = fopen(out_filename, "w");
  if (output_file == NULL) {
    perror("Error opening file");
    return NULL;
  }
  print_attestation_report(att_buffer, output_file);
  free(att_buffer);
  // Close the output file
  fclose(output_file);
  return out_filename;
  #else
  return (char*)report;
  #endif
}
