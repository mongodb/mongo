#ifndef AWS_COMMON_ATOMICS_GNU_OLD_INL
#define AWS_COMMON_ATOMICS_GNU_OLD_INL
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* These are implicitly included, but help with editor highlighting */
#include <aws/common/atomics.h>
#include <aws/common/common.h>

#include <stdint.h>
#include <stdlib.h>

AWS_EXTERN_C_BEGIN

#if defined(__GNUC__)
#    if (__GNUC__ < 4)
#        error GCC versions before 4.1.2 are not supported
#    elif (defined(__arm__) || defined(__ia64__)) && (__GNUC__ == 4 && __GNUC_MINOR__ < 4)
/* See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=36793 Itanium codegen */
/* https://bugs.launchpad.net/ubuntu/+source/gcc-4.4/+bug/491872 ARM codegen*/
/* https://gcc.gnu.org/bugzilla/show_bug.cgi?id=42263 ARM codegen */
#        error GCC versions before 4.4.0 are not supported on ARM or Itanium
#    elif (defined(__x86_64__) || defined(__i386__)) &&                                                                \
        (__GNUC__ == 4 && (__GNUC_MINOR__ < 1 || (__GNUC_MINOR__ == 1 && __GNUC_PATCHLEVEL__ < 2)))
/* 4.1.2 is the first gcc version with 100% working atomic intrinsics on Intel */
#        error GCC versions before 4.1.2 are not supported on x86/x64
#    endif
#endif

typedef size_t aws_atomic_impl_int_t;

static inline void aws_atomic_private_compiler_barrier(void) {
    __asm__ __volatile__("" : : : "memory");
}

static inline void aws_atomic_private_barrier_before(enum aws_memory_order order) {
    if (order == aws_memory_order_release || order == aws_memory_order_acq_rel || order == aws_memory_order_seq_cst) {
        __sync_synchronize();
    }

    aws_atomic_private_compiler_barrier();
}

static inline void aws_atomic_private_barrier_after(enum aws_memory_order order) {
    aws_atomic_private_compiler_barrier();

    if (order == aws_memory_order_acquire || order == aws_memory_order_acq_rel || order == aws_memory_order_seq_cst) {
        __sync_synchronize();
    }
}

/**
 * Initializes an atomic variable with an integer value. This operation should be done before any
 * other operations on this atomic variable, and must be done before attempting any parallel operations.
 */
AWS_STATIC_IMPL
void aws_atomic_init_int(volatile struct aws_atomic_var *var, size_t n) {
    AWS_ATOMIC_VAR_INTVAL(var) = n;
}

/**
 * Initializes an atomic variable with a pointer value. This operation should be done before any
 * other operations on this atomic variable, and must be done before attempting any parallel operations.
 */
AWS_STATIC_IMPL
void aws_atomic_init_ptr(volatile struct aws_atomic_var *var, void *p) {
    AWS_ATOMIC_VAR_PTRVAL(var) = p;
}

/**
 * Reads an atomic var as an integer, using the specified ordering, and returns the result.
 */
AWS_STATIC_IMPL
size_t aws_atomic_load_int_explicit(volatile const struct aws_atomic_var *var, enum aws_memory_order memory_order) {
    aws_atomic_private_barrier_before(memory_order);

    size_t retval = AWS_ATOMIC_VAR_INTVAL(var);

    /* Release barriers are not permitted for loads, so we just do a compiler barrier here */
    aws_atomic_private_compiler_barrier();

    return retval;
}

/**
 * Reads an atomic var as a pointer, using the specified ordering, and returns the result.
 */
AWS_STATIC_IMPL
void *aws_atomic_load_ptr_explicit(volatile const struct aws_atomic_var *var, enum aws_memory_order memory_order) {
    aws_atomic_private_barrier_before(memory_order);

    void *retval = AWS_ATOMIC_VAR_PTRVAL(var);

    /* Release barriers are not permitted for loads, so we just do a compiler barrier here */
    aws_atomic_private_compiler_barrier();

    return retval;
}

/**
 * Stores an integer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_int_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order memory_order) {
    /* Acquire barriers are not permitted for stores, so just do a compiler barrier before */
    aws_atomic_private_compiler_barrier();

    AWS_ATOMIC_VAR_INTVAL(var) = n;

    aws_atomic_private_barrier_after(memory_order);
}

/**
 * Stores a pointer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_ptr_explicit(volatile struct aws_atomic_var *var, void *p, enum aws_memory_order memory_order) {
    /* Acquire barriers are not permitted for stores, so just do a compiler barrier before */
    aws_atomic_private_compiler_barrier();

    AWS_ATOMIC_VAR_PTRVAL(var) = p;

    aws_atomic_private_barrier_after(memory_order);
}

/**
 * Exchanges an integer with the value in an atomic_var, using the specified ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_exchange_int_explicit(
    volatile struct aws_atomic_var *var,
    size_t n,
    enum aws_memory_order memory_order) {

    /*
     * GCC 4.6 and before have only __sync_lock_test_and_set as an exchange operation,
     * which may not support arbitrary values on all architectures. We simply emulate
     * with a CAS instead.
     */

    size_t oldval;
    do {
        oldval = AWS_ATOMIC_VAR_INTVAL(var);
    } while (!__sync_bool_compare_and_swap(&AWS_ATOMIC_VAR_INTVAL(var), oldval, n));

    /* __sync_bool_compare_and_swap implies a full barrier */

    return oldval;
}

/**
 * Exchanges a pointer with the value in an atomic_var, using the specified ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
void *aws_atomic_exchange_ptr_explicit(
    volatile struct aws_atomic_var *var,
    void *p,
    enum aws_memory_order memory_order) {

    /*
     * GCC 4.6 and before have only __sync_lock_test_and_set as an exchange operation,
     * which may not support arbitrary values on all architectures. We simply emulate
     * with a CAS instead.
     */
    void *oldval;
    do {
        oldval = AWS_ATOMIC_VAR_PTRVAL(var);
    } while (!__sync_bool_compare_and_swap(&AWS_ATOMIC_VAR_PTRVAL(var), oldval, p));

    /* __sync_bool_compare_and_swap implies a full barrier */

    return oldval;
}

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. On success, the memory ordering used was order_success; otherwise, it was order_failure.
 * order_failure must be no stronger than order_success, and must not be release or acq_rel.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_int_explicit(
    volatile struct aws_atomic_var *var,
    size_t *expected,
    size_t desired,
    enum aws_memory_order order_success,
    enum aws_memory_order order_failure) {

    bool result = __sync_bool_compare_and_swap(&AWS_ATOMIC_VAR_INTVAL(var), *expected, desired);
    if (!result) {
        *expected = AWS_ATOMIC_VAR_INTVAL(var);
    }

    return result;
}

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. On success, the memory ordering used was order_success; otherwise, it was order_failure.
 * order_failure must be no stronger than order_success, and must not be release or acq_rel.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_ptr_explicit(
    volatile struct aws_atomic_var *var,
    void **expected,
    void *desired,
    enum aws_memory_order order_success,
    enum aws_memory_order order_failure) {

    bool result = __sync_bool_compare_and_swap(&AWS_ATOMIC_VAR_PTRVAL(var), *expected, desired);
    if (!result) {
        *expected = AWS_ATOMIC_VAR_PTRVAL(var);
    }

    return result;
}

/**
 * Atomically adds n to *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_add_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    return __sync_fetch_and_add(&AWS_ATOMIC_VAR_INTVAL(var), n);
}

/**
 * Atomically subtracts n from *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_sub_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    return __sync_fetch_and_sub(&AWS_ATOMIC_VAR_INTVAL(var), n);
}

/**
 * Atomically ORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_or_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    return __sync_fetch_and_or(&AWS_ATOMIC_VAR_INTVAL(var), n);
}

/**
 * Atomically ANDs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_and_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    return __sync_fetch_and_and(&AWS_ATOMIC_VAR_INTVAL(var), n);
}

/**
 * Atomically XORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_xor_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    return __sync_fetch_and_xor(&AWS_ATOMIC_VAR_INTVAL(var), n);
}

/**
 * Provides the same reordering guarantees as an atomic operation with the specified memory order, without
 * needing to actually perform an atomic operation.
 */
AWS_STATIC_IMPL
void aws_atomic_thread_fence(enum aws_memory_order order) {
    /* On old versions of GCC we only have this one big hammer... */
    __sync_synchronize();
}

#define AWS_ATOMICS_HAVE_THREAD_FENCE

AWS_EXTERN_C_END
#endif /* AWS_COMMON_ATOMICS_GNU_OLD_INL */
