#include "attest.h"
#include "attest_microbenchmark.h"
#include "vmpl.h"

#define PAGE_SIZE 4096

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

void measure_monitor_cold() {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate monitor attestation report buffer\n");
      return;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = monitorAttestationCold;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;
  free(att_buffer);
  
  return;
}

void measure_monitor_hot() {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate monitor attestation report buffer\n");
      return;
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
  free(att_buffer);
  
  return;
}

void prepare_measure_wamr_runtime_cold(const uint64_t process_id) {
  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = prepareWamrRuntimeAttestationCold;
  call.monitor_attestation.process_id = process_id;
  uint64_t ret = ioctl(con, VMPL_WR, &call);
  
  return;
}

void measure_wamr_runtime_cold(const uint64_t process_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WAMR runtime attestation report buffer\n");
      return;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = wamrRuntimeAttestationCold;
  call.monitor_attestation.process_id = process_id;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;
  free(att_buffer);
  
  return;
}

void measure_wamr_runtime_hot(const uint64_t process_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WAMR runtime attestation report buffer\n");
      return;
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
  free(att_buffer);
  
  return;
}

void measure_wasm_module_cold(const uint64_t process_id, const uint64_t module_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WASM module attestation report buffer\n");
      return;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = wasmModuleAttestationCold;
  call.monitor_attestation.process_id = process_id;
  call.monitor_attestation.module_id = module_id;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;
  free(att_buffer);
  
  return;
}

void measure_wasm_module_hot(const uint64_t process_id, const uint64_t module_id) {
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
      printf("Can't allocate WASM module attestation report buffer\n");
      return;
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
  free(att_buffer);
  
  return;
}

void measure_function(const uint64_t process_id, const uint64_t module_id,
                        const char* input, const uint64_t input_len, 
                        const char* output, const uint64_t output_len)
{
  uint8_t* att_buffer = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
  if (att_buffer == NULL) {
    printf("Can't allocate function attestation report buffer\n");
    return;
  }
  for(int i = 0; i < PAGE_SIZE; i++){
      att_buffer[i] = 0;
  }

  function_data* function_data_ptr;
  allocate_function_struct(&function_data_ptr);
  function_data_ptr->trustletId = process_id;
  function_data_ptr->moduleId = module_id;
  function_data_ptr->fnInput = (void*)input;
  function_data_ptr->fnInputSize = input_len;
  function_data_ptr->fnOutput = (void*)output;
  function_data_ptr->fnOutputSize = output_len;

  struct monitor_call call;
  call.type = attest;
  call.monitor_attestation.type = functionAttestation;
  call.monitor_attestation.process_id = process_id;
  call.monitor_attestation.function_data_ptr = (void*)function_data_ptr;
  call.attestation_target = att_buffer;
  uint64_t ret = ioctl(con, VMPL_WR, &call);

  struct attestation_report* report = (struct attestation_report*)att_buffer;
  free(att_buffer);
  free(function_data_ptr);

  return;
}
