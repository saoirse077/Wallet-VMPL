#ifndef ATTEST_MICROBENCHMARK_H_
#define ATTEST_MICROBENCHMARK_H_

/* Attestation measurements API */

void measure_monitor_cold();
void measure_monitor_hot();
void prepare_measure_wamr_runtime_cold(const uint64_t process_id);
void measure_wamr_runtime_cold(const uint64_t process_id);
void measure_wamr_runtime_hot(const uint64_t process_id);
void measure_wasm_module_cold(const uint64_t process_id, const uint64_t module_id);
void measure_wasm_module_hot(const uint64_t process_id, const uint64_t module_id);
void measure_function(const uint64_t process_id, const uint64_t module_id,
                        const char* input, const uint64_t input_len, 
                        const char* output, const uint64_t output_len);

/* End of Attestation measurements API */

#endif // ATTEST_MICROBENCHMARK_H_
