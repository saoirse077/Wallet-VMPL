/*
 * stdatomic.h compat shim for wasmlet on VMPL1 (freestanding)
 *
 * Maps C11 atomic types and operations to GCC __atomic builtins.
 */

#ifndef _WASMLET_COMPAT_STDATOMIC_H
#define _WASMLET_COMPAT_STDATOMIC_H

#include <stdint.h>
#include <stdbool.h>

/* Memory orders */
typedef enum {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

/* Atomic types */
typedef volatile _Bool        atomic_bool;
typedef volatile unsigned int atomic_uint;
typedef volatile uint_fast32_t atomic_uint_fast32_t;
typedef volatile uint_fast64_t atomic_uint_fast64_t;

/* Load / Store */
#define atomic_load(ptr)  __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
#define atomic_store(ptr, val)  __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_load_explicit(ptr, order)  __atomic_load_n(ptr, order)
#define atomic_store_explicit(ptr, val, order)  __atomic_store_n(ptr, val, order)

/* Fetch-add / Fetch-sub */
#define atomic_fetch_add(ptr, val)  __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub(ptr, val)  __atomic_fetch_sub(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_add_explicit(ptr, val, order)  __atomic_fetch_add(ptr, val, order)
#define atomic_fetch_sub_explicit(ptr, val, order)  __atomic_fetch_sub(ptr, val, order)

/* Compare-and-swap */
#define atomic_compare_exchange_weak(ptr, expected, desired) \
    __atomic_compare_exchange_n(ptr, expected, desired, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_strong(ptr, expected, desired) \
    __atomic_compare_exchange_n(ptr, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_weak_explicit(ptr, expected, desired, succ, fail) \
    __atomic_compare_exchange_n(ptr, expected, desired, 1, succ, fail)

#define atomic_compare_exchange_strong_explicit(ptr, expected, desired, succ, fail) \
    __atomic_compare_exchange_n(ptr, expected, desired, 0, succ, fail)

#endif /* _WASMLET_COMPAT_STDATOMIC_H */
