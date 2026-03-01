/*
 * pal_monitor_call.h - VMPL1 ↔ VMPL0 CPUID trap interface
 *
 * Standalone version for WAMR PAL (no Gramine dependencies).
 * Based on gramine-svsm/pal/src/host/svsm/pal_monitor_call.h
 *
 * All communication with VMPL0 Monitor is done via CPUID instruction traps.
 * The Monitor intercepts the CPUID and reads/writes VMSA registers.
 */

#ifndef WAMR_PAL_MONITOR_CALL_H_
#define WAMR_PAL_MONITOR_CALL_H_

#include <stdint.h>
#include <stddef.h>

/* ========== CPUID trap data structure ========== */

struct monitor_call_data {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t r8;
    uint64_t r9;
};

/* ========== Low-level CPUID trap functions ========== */

/* Basic CPUID trap (uses rax, rbx, rcx, rdx) */
void monitor_call(struct monitor_call_data* data);

/* Extended CPUID trap (additionally uses r8, r9) */
void extended_monitor_call(struct monitor_call_data* data);

/* ========== Basic PAL services ========== */

/*
 * Debug output - single character to SVSM serial console
 * Call number: 0x4FFFFFFD
 * Send '\0' to flush the line buffer in the Monitor.
 */
void pal_svsm_debug_putc(char c);

/*
 * Debug output - null-terminated string
 * Sends each character via debug_putc, then sends '\0' to flush.
 */
void pal_svsm_debug_print(const char* str);

/* Debug output - print uint64 as hex (e.g. "0x0000000012345678") */
void pal_svsm_debug_print_hex(uint64_t val);

/* Debug output - print int as decimal */
void pal_svsm_debug_print_dec(int val);

/*
 * Exit VMPL1 execution
 * Call number: 0x4FFFFFFE
 * This causes handle_process_request() to return false, breaking the
 * ap_create loop in early_invoke()/invoke_trustlet().
 */
void pal_svsm_exit(int exitcode);

/*
 * Report error and exit
 * Call number: 0x4FFFFFFF
 */
void pal_svsm_fail(const char* err, int err_no);

/* ========== Memory management ========== */

/*
 * Allocate virtual memory (backed by physical pages)
 * Call number: 0x4FFFFFFC
 * Parameters:
 *   addr  - virtual address (must be page-aligned)
 *   size  - allocation size in bytes (must be page-aligned)
 *   flags - protection flags (0x7 = RWX)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_virt_alloc(void* addr, uint64_t size, uint64_t flags);

/*
 * Free virtual memory
 * Call number: 0x4FFFFFF6
 * Parameters:
 *   addr - virtual address (must match previous alloc)
 *   size - size in bytes (must match previous alloc)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_free(void* addr, uint64_t size);

/* ========== MPK six interfaces (VMPL0 managed) ========== */

/*
 * Interface 1: Allocate a protection key (pkey)
 * Call number: 0x4FFFFFF1
 * Returns: pkey (1-15) on success, negative error code on failure
 */
int pal_svsm_mpk_pkey_alloc(void);

/*
 * Interface 2: Allocate memory with pkey tag
 * Call number: 0x4FFFFFF3
 * Parameters:
 *   addr - virtual address (must be page-aligned)
 *   size - allocation size (must be page-aligned)
 *   pkey - protection key (1-15, from pkey_alloc)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_mpk_alloc(void* addr, uint64_t size, uint32_t pkey);

/*
 * Interface 3: Enter secure domain (enable pkey read/write in PKRU)
 * Call number: 0x4FFFFFF0
 * Parameters:
 *   pkey - protection key to enable (1-15)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_mpk_enter_domain(uint32_t pkey);

/*
 * Interface 4: Exit secure domain (disable pkey in PKRU)
 * Call number: 0x4FFFFFEF
 * Parameters:
 *   pkey - protection key to disable (1-15)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_mpk_exit_domain(uint32_t pkey);

/*
 * Interface 5: Free memory (without freeing pkey)
 * Call number: 0x4FFFFFF2
 * Parameters:
 *   addr - virtual address
 *   size - allocation size
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_mpk_free(void* addr, uint64_t size);

/*
 * Interface 6: Free pkey (optionally free memory too)
 * Call number: 0x4FFFFFEE
 * Parameters:
 *   pkey - protection key to free (1-15)
 *   addr - virtual address (0 = don't free memory)
 *   size - size (0 = don't free memory)
 * Returns: 0 on success, non-zero on error
 */
int pal_svsm_mpk_free_pkey(uint32_t pkey, void* addr, uint64_t size);

/*
 * Helper: Query current VMSA.pkru value (for debugging)
 * Call number: 0x4FFFFFED
 * Returns: current PKRU value
 */
uint32_t pal_svsm_mpk_query_pkru(void);

/* ========== Trustlet result notification ========== */

/*
 * Notify SVSM that results are ready in the output channel.
 * Call number: 0x4FFFFFF8
 *
 * When VMPL1 calls this during invoke_trustlet, SVSM will:
 *   1. Copy the output channel data to the Guest's return buffer
 *      (via copy_out to result_addr in guest page table)
 *   2. Set return_value = GETRESULT (1)
 *   3. Break the ap_create loop (return false)
 *   4. The Guest's ioctl returns invocationGetValue (1)
 *
 * After this call returns, VMPL1 is suspended until the next
 * invoke_trustlet from the Guest.
 */
void pal_svsm_get_result(void);

/* ========== Thread management (VMPL0 managed) ========== */

/*
 * Create a thread (allocate VMSA, execution deferred until join)
 * Call number: 0x4FFFFFEC
 * Parameters:
 *   entry_rip  - thread entry function address
 *   stack_top  - top of the pre-allocated stack (grows downward)
 *   gs_base    - GS segment base for TLS (TCB address)
 *   arg        - single uint64_t argument passed in RDI
 * Returns: thread id (0..7), or UINT64_MAX on error
 */
uint64_t pal_svsm_thread_create(uint64_t entry_rip, uint64_t stack_top,
                                 uint64_t gs_base, uint64_t arg);

/*
 * Join a thread (blocks until thread calls thread_exit)
 * Call number: 0x4FFFFFEB
 * Parameters:
 *   tid - thread id from thread_create
 * Returns: exit code from the thread
 */
uint64_t pal_svsm_thread_join(uint64_t tid);

/*
 * Exit current thread (called from within a thread)
 * Call number: 0x4FFFFFEA
 * Parameters:
 *   exit_code - value returned to the joiner
 * Does not return.
 */
void pal_svsm_thread_exit(uint64_t exit_code);

/* ========== Thread capacity query ========== */

/*
 * Query available thread runner capacity (number of APs for threads)
 * Call number: 0x4FFFFFE9
 * Returns: number of available thread runners (CPU_COUNT - 1)
 */
uint64_t pal_svsm_query_thread_capacity(void);

/* ========== Memory Channel management ========== */

/*
 * Inflate (expand) a memory channel
 * Call number: 0x4FFFFFA3
 *
 * This asks VMPL0 to allocate physical pages and map them into
 * the VMPL1 page table at the channel's fixed virtual address.
 *
 * Parameters:
 *   select - 0 = inflate input channel (0x280_0000_0000)
 *            1 = inflate output channel (0x300_0000_0000)
 *   size   - size in bytes (will be rounded up to page boundary)
 * Returns: 0 on success (SVSM always returns true for this call)
 */
int pal_svsm_inflate_channel(int select, uint64_t size);

#endif /* WAMR_PAL_MONITOR_CALL_H_ */
