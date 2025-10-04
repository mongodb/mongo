#ifndef AWS_COMMON_ATOMICS_H
#define AWS_COMMON_ATOMICS_H

#include <aws/common/common.h>

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * struct aws_atomic_var represents an atomic variable - a value which can hold an integer or pointer
 * that can be manipulated atomically. struct aws_atomic_vars should normally only be manipulated
 * with atomics methods defined in this header.
 */
struct aws_atomic_var {
    void *value;
};
/* Helpers for extracting the integer and pointer values from aws_atomic_var. */
#define AWS_ATOMIC_VAR_PTRVAL(var) ((var)->value)
#define AWS_ATOMIC_VAR_INTVAL(var) (*(aws_atomic_impl_int_t *)(var))

/*
 * This enumeration specifies the memory ordering properties requested for a particular
 * atomic operation. The atomic operation may provide stricter ordering than requested.
 * Note that, within a single thread, all operations are still sequenced (that is, a thread
 * sees its own atomic writes and reads happening in program order, but other threads may
 * disagree on this ordering).
 *
 * The behavior of these memory orderings are the same as in the C11 atomics API; however,
 * we only implement a subset that can be portably implemented on the compilers we target.
 */

enum aws_memory_order {
    /**
     * No particular ordering constraints are guaranteed relative to other
     * operations at all; we merely ensure that the operation itself is atomic.
     */
    aws_memory_order_relaxed = 0,
    /* aws_memory_order_consume - not currently implemented */

    /**
     * Specifies acquire ordering. No reads or writes on the current thread can be
     * reordered to happen before this operation. This is typically paired with a release
     * ordering; any writes that happened on the releasing operation will be visible
     * after the paired acquire operation.
     *
     * Acquire ordering is only meaningful on load or load-store operations.
     */
    aws_memory_order_acquire = 2, /* leave a spot for consume if we ever add it */

    /**
     * Specifies release order. No reads or writes can be reordered to come after this
     * operation. Typically paired with an acquire operation.
     *
     * Release ordering is only meaningful on store or load-store operations.
     */
    aws_memory_order_release,

    /**
     * Specifies acquire-release order; if this operation acts as a load, it acts as an
     * acquire operation; if it acts as a store, it acts as a release operation; if it's
     * a load-store, it does both.
     */
    aws_memory_order_acq_rel,

    /*
     * Specifies sequentially consistent order. This behaves as acq_rel, but in addition,
     * all seq_cst operations appear to occur in some globally consistent order.
     *
     * TODO: Figure out how to correctly implement this in MSVC. It appears that interlocked
     * functions provide only acq_rel ordering.
     */
    aws_memory_order_seq_cst
};

/**
 * Statically initializes an aws_atomic_var to a given size_t value.
 */
#define AWS_ATOMIC_INIT_INT(x) {.value = (void *)(uintptr_t)(x)}

/**
 * Statically initializes an aws_atomic_var to a given void * value.
 */
#define AWS_ATOMIC_INIT_PTR(x) {.value = (void *)(x)}

AWS_EXTERN_C_BEGIN

/*
 * Note: We do not use the C11 atomics API; this is because we want to make sure the representation
 * (and behavior) of atomic values is consistent, regardless of what --std= flag you pass to your compiler.
 * Since C11 atomics can silently introduce locks, we run the risk of creating such ABI inconsistencies
 * if we decide based on compiler features which atomics API to use, and in practice we expect to have
 * either the GNU or MSVC atomics anyway.
 *
 * As future work, we could test to see if the C11 atomics API on this platform behaves consistently
 * with the other APIs and use it if it does.
 */

/**
 * Initializes an atomic variable with an integer value. This operation should be done before any
 * other operations on this atomic variable, and must be done before attempting any parallel operations.
 *
 * This operation does not imply a barrier. Ensure that you use an acquire-release barrier (or stronger)
 * when communicating the fact that initialization is complete to the other thread. Launching the thread
 * implies a sufficiently strong barrier.
 */
AWS_STATIC_IMPL
void aws_atomic_init_int(volatile struct aws_atomic_var *var, size_t n);

/**
 * Initializes an atomic variable with a pointer value. This operation should be done before any
 * other operations on this atomic variable, and must be done before attempting any parallel operations.
 *
 * This operation does not imply a barrier. Ensure that you use an acquire-release barrier (or stronger)
 * when communicating the fact that initialization is complete to the other thread. Launching the thread
 * implies a sufficiently strong barrier.
 */
AWS_STATIC_IMPL
void aws_atomic_init_ptr(volatile struct aws_atomic_var *var, void *p);

/**
 * Reads an atomic var as an integer, using the specified ordering, and returns the result.
 */
AWS_STATIC_IMPL
size_t aws_atomic_load_int_explicit(volatile const struct aws_atomic_var *var, enum aws_memory_order memory_order);

/**
 * Reads an atomic var as an integer, using sequentially consistent ordering, and returns the result.
 */
AWS_STATIC_IMPL
size_t aws_atomic_load_int(volatile const struct aws_atomic_var *var);
/**
 * Reads an atomic var as a pointer, using the specified ordering, and returns the result.
 */
AWS_STATIC_IMPL
void *aws_atomic_load_ptr_explicit(volatile const struct aws_atomic_var *var, enum aws_memory_order memory_order);

/**
 * Reads an atomic var as a pointer, using sequentially consistent ordering, and returns the result.
 */
AWS_STATIC_IMPL
void *aws_atomic_load_ptr(volatile const struct aws_atomic_var *var);

/**
 * Stores an integer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_int_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order memory_order);

/**
 * Stores an integer into an atomic var, using sequentially consistent ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_int(volatile struct aws_atomic_var *var, size_t n);

/**
 * Stores a pointer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_ptr_explicit(volatile struct aws_atomic_var *var, void *p, enum aws_memory_order memory_order);

/**
 * Stores a pointer into an atomic var, using sequentially consistent ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_ptr(volatile struct aws_atomic_var *var, void *p);

/**
 * Exchanges an integer with the value in an atomic_var, using the specified ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_exchange_int_explicit(
    volatile struct aws_atomic_var *var,
    size_t n,
    enum aws_memory_order memory_order);

/**
 * Exchanges an integer with the value in an atomic_var, using sequentially consistent ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_exchange_int(volatile struct aws_atomic_var *var, size_t n);

/**
 * Exchanges a pointer with the value in an atomic_var, using the specified ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
void *aws_atomic_exchange_ptr_explicit(
    volatile struct aws_atomic_var *var,
    void *p,
    enum aws_memory_order memory_order);

/**
 * Exchanges an integer with the value in an atomic_var, using sequentially consistent ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
void *aws_atomic_exchange_ptr(volatile struct aws_atomic_var *var, void *p);

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. On success, the memory ordering used was order_success; otherwise, it was order_failure.
 * order_failure must be no stronger than order_success, and must not be release or acq_rel.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_int_explicit(
    volatile struct aws_atomic_var *var,
    size_t *expected,
    size_t desired,
    enum aws_memory_order order_success,
    enum aws_memory_order order_failure);

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. Uses sequentially consistent memory ordering, regardless of success or failure.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_int(volatile struct aws_atomic_var *var, size_t *expected, size_t desired);

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. On success, the memory ordering used was order_success; otherwise, it was order_failure.
 * order_failure must be no stronger than order_success, and must not be release or acq_rel.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_ptr_explicit(
    volatile struct aws_atomic_var *var,
    void **expected,
    void *desired,
    enum aws_memory_order order_success,
    enum aws_memory_order order_failure);

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. Uses sequentially consistent memory ordering, regardless of success or failure.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_ptr(volatile struct aws_atomic_var *var, void **expected, void *desired);

/**
 * Atomically adds n to *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_add_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order);

/**
 * Atomically subtracts n from *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_sub_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order);

/**
 * Atomically ORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_or_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order);

/**
 * Atomically ANDs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_and_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order);

/**
 * Atomically XORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_xor_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order);

/**
 * Atomically adds n to *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_add(volatile struct aws_atomic_var *var, size_t n);

/**
 * Atomically subtracts n from *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_sub(volatile struct aws_atomic_var *var, size_t n);

/**
 * Atomically ands n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_and(volatile struct aws_atomic_var *var, size_t n);

/**
 * Atomically ors n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_or(volatile struct aws_atomic_var *var, size_t n);

/**
 * Atomically xors n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_xor(volatile struct aws_atomic_var *var, size_t n);

/**
 * Provides the same reordering guarantees as an atomic operation with the specified memory order, without
 * needing to actually perform an atomic operation.
 */
AWS_STATIC_IMPL
void aws_atomic_thread_fence(enum aws_memory_order order);

AWS_EXTERN_C_END

#ifndef AWS_NO_STATIC_IMPL
#    include <aws/common/atomics.inl>
#endif /* AWS_NO_STATIC_IMPL */

AWS_POP_SANE_WARNING_LEVEL

#endif
