# Multi-Threaded WASM Execution — Design Document

## 1. Architecture Overview

### System Layering

```
┌──────────────────────────────────────────────────┐
│  Guest OS (Linux)                                │
│    Python test → libwallet → ioctl               │
├──────────────────────────────────────────────────┤
│  VMPL1 Trustlet                                  │
│    wamr-pal  (entry point, command dispatch)     │
│    wasmlet   (runtime, thread pool, MPK domains) │
│    WAMR      (WebAssembly interpreter engine)    │
├──────────────────────────────────────────────────┤
│  VMPL0 SVSM Monitor                             │
│    runtime.rs  (monitor calls, thread mgmt)      │
│    mpk_memory.rs (MPK page table operations)     │
└──────────────────────────────────────────────────┘
```

### Multi-vCPU Model

- **BSP (Bootstrap Processor)**: Runs the main Trustlet logic — initialization,
  command dispatch, module loading, and result retrieval.
- **AP (Application Processors)**: Each AP is a dedicated worker vCPU that runs
  `thread_runner_idle()` in the SVSM monitor, picking up thread tasks from
  `THREAD_SLOTS`.
- **1:1 binding**: Each AP monitors exactly one slot (`apic_id - base`). There
  is no scheduler — an AP runs a single thread to completion before accepting
  the next one.

### Zygote / Trustlet Two-Phase Model

- **Phase A (early_invoke)**: The BSP performs `heap_init` →
  `wasmlet_runtime_init` → `pal_svsm_exit(0)`. Workers are NOT created during
  this phase, ensuring the page table state is safe for Copy-on-Write (CoW)
  fork in SVSM.
- **Phase B (invoke_trustlet loop)**: The BSP enters the command dispatch loop.
  Workers are started lazily on the first `submit_task` command via
  `wasmlet_start_workers()`.

---

## 2. Thread Management

### SVSM Monitor Call Interface

| Call                | Code         | Arguments                           | Returns          |
|---------------------|--------------|-------------------------------------|------------------|
| `thread_create`     | `0x4FFFFFEC` | rbx=rip, rcx=rsp, rdx=gs, r8=arg   | rax=slot_id      |
| `thread_join`       | `0x4FFFFFEB` | rbx=slot_id                         | rax=exit_code    |
| `thread_exit`       | `0x4FFFFFEA` | rbx=exit_code                       | (does not return)|
| `query_capacity`    | `0x4FFFFFE9` | (none)                              | rax=num_runners  |

### VMSA Lifecycle

1. **Allocate**: BSP allocates a physical page, copies its own VMSA as template.
2. **Configure**: Sets `rip`, `rsp`, `rbp`, `gs.base`, `rdi` (argument), clears
   residual exit state (`guest_exit_code`, etc.).
3. **RMP Adjust**: Mark page as VMPL1 VMSA via `rmp_set_guest_vmsa`.
4. **Post to slot**: Store `vmsa_paddr`, `process_id`, transition state to
   `Pending`.
5. **AP pickup**: AP sees `Pending`, transitions to `Running`, calls `ap_create`
   in a loop.
6. **Exit**: Thread calls `thread_exit` (intercepted before `handle_process_request`),
   AP sets `exit_code` and transitions to `Done`.
7. **Join + Free**: BSP spins on `Done`, reads `exit_code`, frees VMSA page,
   resets slot to `Free`.

### Slot State Machine

```
Free ──(CAS by BSP)──► Reserved ──(BSP setup done)──► Pending
                                                          │
                                                     (AP pickup)
                                                          ▼
                                                       Running
                                                          │
                                                    (thread_exit)
                                                          ▼
                                                        Done ──(BSP join)──► Free
```

The `Reserved` state is introduced to prevent race conditions: `find_free_thread_slot`
uses `compare_exchange(Free → Reserved)` to atomically claim a slot. The BSP then
sets up VMSA data and transitions to `Pending`. The AP only picks up `Pending` slots,
ensuring all data is visible before execution begins.

### TLS Implementation

Each worker thread has its own Thread Control Block (TCB) accessed via `GS.base`.
When creating a new VMSA, the BSP sets `new_vmsa.gs.base = gs_base` (passed by the
VMPL1 `thread_create` caller). The wasmlet platform layer uses `GS.base` for
thread-local storage (TLS) — each worker's exec heap mspace and WAMR thread
environment are stored in TLS.

---

## 3. Async Execution Pipeline

### Command Dispatch

The BSP enters a command loop reading from the input channel at `0x28000000000`:

| Command | Name           | Description                                 |
|---------|----------------|---------------------------------------------|
| `0`     | sync_invoke    | Load + run (or run-only) synchronously      |
| `1`     | load_module    | Load WASM module, return module_id          |
| `2`     | submit_task    | Async submit: enqueue to thread pool        |
| `3`     | get_result     | Poll for async result by request_id         |
| `0xFF`  | destroy        | Unload module, join workers, shutdown       |

### Wasmlet Thread Pool

```
submit_task ──► lockfree_queue ──► worker_thread_func ──► wasm_executor
                                                              │
                                                         result_store
                                                              │
get_result  ◄────────────────────────────────────────────────-┘
```

- **lockfree_queue**: Multi-producer multi-consumer ring buffer sized by
  `config.lf_queue_size` (default 64).
- **worker_thread_func**: Pops tasks, invokes the registered executor, stores
  results. Uses `wasmlet_usleep(1000)` when idle.
- **result_store**: Thread-safe map from `request_id` to `execution_result_t`.
  Supports `PENDING` → `SUCCESS`/`ERROR` state transitions.

### Lazy Worker Startup

Workers are not created during Phase A (`max_threads=0`). On the first
`submit_task` command, `ensure_workers_started()` calls
`wasmlet_start_workers(thread_capacity)`, which:

1. Updates `config.max_threads`
2. Creates the thread pool
3. Registers the WASM executor
4. Sets worker init/cleanup callbacks
5. Starts worker threads (each triggers `pal_svsm_thread_create`)

---

## 4. MPK Isolation Model

### Per-vCPU PKRU Independence

PKRU is stored in each vCPU's VMSA. When the SVSM monitor modifies
`vmsa.pkru` for domain enter/exit, only that vCPU is affected. Multiple
workers can safely enter the same MPK domain concurrently — each modifies
its own VMSA's PKRU field.

### Module Heap (pkey > 0)

- Allocated via `mpk_alloc_memory` with a specific pkey (1–15).
- Access controlled by `domain_enter` (clear PKRU bits) / `domain_exit`
  (set PKRU AD bit).
- Shared `mspace` per module (`domain->module_msp`) protected by
  `dlmalloc USE_LOCKS=1`.

### Execution Heap (pkey = 0)

- Per-worker TLS mspace (`mpk_thread_exec_init`).
- Uses pkey=0 (always accessible) because SVSM cannot re-key already-mapped
  pages.
- Thread isolation is achieved through TLS — each worker only accesses its
  own exec heap.

### SVSM MPK Interfaces

| Interface       | Code         | Description                      |
|-----------------|--------------|----------------------------------|
| pkey_alloc      | `0x4FFFFFF1` | Allocate a pkey from bitmap      |
| mpk_alloc       | `0x4FFFFFF3` | Map pages with pkey in PTE       |
| enter_domain    | `0x4FFFFFF0` | Clear PKRU bits for pkey         |
| exit_domain     | `0x4FFFFFEF` | Set PKRU AD bit for pkey         |
| mpk_free        | `0x4FFFFFF2` | Unmap pages, clear PTE pkey      |
| free_pkey       | `0x4FFFFFEE` | Release pkey (optionally + free) |

### Known Limitation: Exec Heap pkey=0

The execution heap uses `pkey=0` (always accessible), meaning hardware MPK
isolation does not protect exec heap memory. This is a known design limitation:

- SVSM's `add_pages` creates new page table entries; there is no API to change
  the pkey on an already-mapped page.
- Mitigation: each worker has an independent TLS exec heap. Workers do not
  access each other's exec heaps.
- All workers execute code from the same Trustlet, so the trust domain is
  identical.

---

## 5. Concurrency Safety

### Page Table Modifications

All page table modifications go through `PAGE_TABLE_LOCK` (a `SpinLock` in
`runtime.rs`):

- `pal_svsm_virt_alloc` / `pal_svsm_virt_free`: held during `add_pages` /
  `remove_pages`
- `pal_svsm_mpk_alloc` / `pal_svsm_mpk_free` / `pal_svsm_mpk_free_pkey`:
  held during `mpk_alloc_memory` / `mpk_free_memory` / `mpk_free_pkey`

This prevents corruption when BSP (module loading) and APs (worker init) modify
the shared CR3 page table concurrently.

### Shared Allocator Safety

- **dlmalloc** (`USE_LOCKS=1`): The module heap mspace uses dlmalloc's internal
  mutex for thread-safe allocation/deallocation.
- **Bump allocator** (`vmpl1_mmap.c`): `mmap_next_addr` is a simple bump
  pointer. In the current design, `os_mmap` is only called from BSP (module
  loading) or within `init_mutex`-protected worker init, so no atomic is needed.
  For future multi-BSP scenarios, this should be made atomic.

### Thread Slot Allocation

`find_free_thread_slot` uses `compare_exchange(Free → Reserved)` to atomically
claim a slot, preventing duplicate allocation when concurrent `thread_create`
calls race for the same slot.

### Recursive Mutex

`korp_mutex` in `platform_internal.h` supports recursion via an `owner` field
and `count`. This satisfies WAMR's internal locking requirements where the same
thread may re-acquire a mutex (e.g., during nested runtime calls).

---

## 6. Data Flow Diagram

```
Guest Python → libwallet → ioctl → SVSM → VMPL1 Trustlet
                                              │
                                    ┌─────────┴──────────┐
                                    BSP                   AP (worker)
                                    │                     │
                              cmd dispatch           thread_runner_idle
                              load_module             ↓
                              submit_task → queue → worker_thread_func
                              get_result ← store ← wasm_executor
```

### Detailed Async Flow

1. Guest sends `load_module` (cmd=1) with WASM binary → BSP loads module →
   returns `module_id`.
2. Guest sends `submit_task` (cmd=2) with module_id + function name + args →
   BSP enqueues to lockfree queue, ensures workers are started →
   returns `request_id`.
3. AP worker pops task from queue → enters MPK domain → instantiates WASM
   module → calls function → stores result in result_store.
4. Guest sends `get_result` (cmd=3) with request_id → BSP polls result_store →
   returns status + return value (or PENDING=0xFD).
5. Guest sends `destroy` (cmd=0xFF) → BSP unloads module, joins all workers,
   destroys runtime.
