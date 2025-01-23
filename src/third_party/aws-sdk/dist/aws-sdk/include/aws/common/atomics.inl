#ifndef AWS_COMMON_ATOMICS_INL
#define AWS_COMMON_ATOMICS_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>
#include <aws/common/common.h>

AWS_EXTERN_C_BEGIN

/**
 * Reads an atomic var as an integer, using sequentially consistent ordering, and returns the result.
 */
AWS_STATIC_IMPL
size_t aws_atomic_load_int(volatile const struct aws_atomic_var *var) {
    return aws_atomic_load_int_explicit(var, aws_memory_order_seq_cst);
}

/**
 * Reads an atomic var as a pointer, using sequentially consistent ordering, and returns the result.
 */
AWS_STATIC_IMPL
void *aws_atomic_load_ptr(volatile const struct aws_atomic_var *var) {
    return aws_atomic_load_ptr_explicit(var, aws_memory_order_seq_cst);
}

/**
 * Stores an integer into an atomic var, using sequentially consistent ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_int(volatile struct aws_atomic_var *var, size_t n) {
    aws_atomic_store_int_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Stores a pointer into an atomic var, using sequentially consistent ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_ptr(volatile struct aws_atomic_var *var, void *p) {
    aws_atomic_store_ptr_explicit(var, p, aws_memory_order_seq_cst);
}

/**
 * Exchanges an integer with the value in an atomic_var, using sequentially consistent ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_exchange_int(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_exchange_int_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Exchanges an integer with the value in an atomic_var, using sequentially consistent ordering.
 * Returns the value that was previously in the atomic_var.
 */
AWS_STATIC_IMPL
void *aws_atomic_exchange_ptr(volatile struct aws_atomic_var *var, void *p) {
    return aws_atomic_exchange_ptr_explicit(var, p, aws_memory_order_seq_cst);
}

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. Uses sequentially consistent memory ordering, regardless of success or failure.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_int(volatile struct aws_atomic_var *var, size_t *expected, size_t desired) {
    return aws_atomic_compare_exchange_int_explicit(
        var, expected, desired, aws_memory_order_seq_cst, aws_memory_order_seq_cst);
}

/**
 * Atomically compares *var to *expected; if they are equal, atomically sets *var = desired. Otherwise, *expected is set
 * to the value in *var. Uses sequentially consistent memory ordering, regardless of success or failure.
 * Returns true if the compare was successful and the variable updated to desired.
 */
AWS_STATIC_IMPL
bool aws_atomic_compare_exchange_ptr(volatile struct aws_atomic_var *var, void **expected, void *desired) {
    return aws_atomic_compare_exchange_ptr_explicit(
        var, expected, desired, aws_memory_order_seq_cst, aws_memory_order_seq_cst);
}

/**
 * Atomically adds n to *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_add(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_fetch_add_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Atomically subtracts n from *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_sub(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_fetch_sub_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Atomically ands n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_and(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_fetch_and_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Atomically ors n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_or(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_fetch_or_explicit(var, n, aws_memory_order_seq_cst);
}

/**
 * Atomically xors n into *var, and returns the previous value of *var.
 * Uses sequentially consistent ordering.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_xor(volatile struct aws_atomic_var *var, size_t n) {
    return aws_atomic_fetch_xor_explicit(var, n, aws_memory_order_seq_cst);
}
AWS_EXTERN_C_END

/* Include the backend implementation now, because we'll use its typedefs and #defines below */
#if defined(__GNUC__) || defined(__clang__)
#    if defined(__ATOMIC_RELAXED)
#        include <aws/common/atomics_gnu.inl>
#    else
#        include <aws/common/atomics_gnu_old.inl>
#    endif /* __ATOMIC_RELAXED */
#elif defined(_MSC_VER)
#    include <aws/common/atomics_msvc.inl>
#else
#    error No atomics implementation for your compiler is available
#endif

#include <aws/common/atomics_fallback.inl>

#endif /* AWS_COMMON_ATOMICS_INL */
