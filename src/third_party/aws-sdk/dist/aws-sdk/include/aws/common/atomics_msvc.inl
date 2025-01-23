#ifndef AWS_COMMON_ATOMICS_MSVC_INL
#define AWS_COMMON_ATOMICS_MSVC_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* These are implicitly included, but helps with editor highlighting */
#include <aws/common/atomics.h>
#include <aws/common/common.h>

/* This file generates level 4 compiler warnings in Visual Studio 2017 and older */
#pragma warning(push, 3)
#include <intrin.h>
#pragma warning(pop)

#include <stdint.h>
#include <stdlib.h>

AWS_EXTERN_C_BEGIN

#if !(defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM64))
#    error Atomics are not currently supported for non-x86 or ARM64 MSVC platforms

/*
 * In particular, it's not clear that seq_cst will work properly on non-x86
 * memory models. We may need to make use of platform-specific intrinsics.
 *
 * NOTE: Before removing this #error, please make use of the Interlocked*[Acquire|Release]
 * variants (if applicable for the new platform)! This will (hopefully) help ensure that
 * code breaks before people take too much of a dependency on it.
 */

#endif

/**
 * Some general notes:
 *
 * On x86/x86_64, by default, windows uses acquire/release semantics for volatile accesses;
 * however, this is not the case on ARM, and on x86/x86_64 it can be disabled using the
 * /volatile:iso compile flag.
 *
 * Interlocked* functions implicitly have acq_rel semantics; there are ones with weaker
 * semantics as well, but because windows is generally used on x86, where there's not a lot
 * of performance difference between different ordering modes anyway, we just use the stronger
 * forms for now. Further, on x86, they actually have seq_cst semantics as they use locked instructions.
 * It is unclear if Interlocked functions guarantee seq_cst on non-x86 platforms.
 *
 * Since all loads and stores are acq and/or rel already, we can do non-seq_cst loads and stores
 * as just volatile variable accesses, but add the appropriate barriers for good measure.
 *
 * For seq_cst accesses, we take advantage of the facts that (on x86):
 * 1. Loads are not reordered with other loads
 * 2. Stores are not reordered with other stores
 * 3. Locked instructions (including swaps) have a total order
 * 4. Non-locked accesses are not reordered with locked instructions
 *
 * Therefore, if we ensure that all seq_cst stores are locked, we can establish
 * a total order on stores, and the intervening ordinary loads will not violate that total
 * order.
 * See http://www.cs.cmu.edu/~410-f10/doc/Intel_Reordering_318147.pdf 2.7, which covers
 * this use case.
 */

/**
 * Some general notes about ARM environments:
 * ARM processors uses a weak memory model as opposed to the strong memory model used by Intel processors
 * This means more permissible memory ordering allowed between stores and loads.
 *
 * Thus ARM port will need more hardware fences/barriers to assure developer intent.
 * Memory barriers will prevent reordering stores and loads accross them depending on their type
 * (read write, write only, read only ...)
 *
 * For more information about ARM64 memory ordering,
 * see https://developer.arm.com/documentation/102336/0100/Memory-ordering
 * For more information about Memory barriers,
 * see https://developer.arm.com/documentation/102336/0100/Memory-barriers
 * For more information about Miscosoft Interensic ARM64 APIs,
 * see https://learn.microsoft.com/en-us/cpp/intrinsics/arm64-intrinsics?view=msvc-170
 * Note: wrt _Interlocked[Op]64 is the same for ARM64 and x64 processors
 */

#ifdef _M_IX86
#    define AWS_INTERLOCKED_INT(x) _Interlocked##x
typedef long aws_atomic_impl_int_t;
#else
#    define AWS_INTERLOCKED_INT(x) _Interlocked##x##64
typedef long long aws_atomic_impl_int_t;
#endif

#ifdef _M_ARM64
/* Hardware Read Write barrier, prevents all memory operations to cross the barrier in both directions */
#    define AWS_RW_BARRIER() __dmb(_ARM64_BARRIER_SY)
/* Hardware Read barrier, prevents all memory operations to cross the barrier upwards */
#    define AWS_R_BARRIER() __dmb(_ARM64_BARRIER_LD)
/* Hardware Write barrier, prevents all memory operations to cross the barrier downwards */
#    define AWS_W_BARRIER() __dmb(_ARM64_BARRIER_ST)
/* Software barrier, prevents the compiler from reodering the operations across the barrier */
#    define AWS_SW_BARRIER() _ReadWriteBarrier();
#else
/* hardware barriers, do nothing on x86 since it has a strong memory model
 * as described in the section above: some general notes
 */
#    define AWS_RW_BARRIER()
#    define AWS_R_BARRIER()
#    define AWS_W_BARRIER()
/*
 * x86: only a compiler barrier is required. For seq_cst, we must use some form of interlocked operation for
 * writes, but that's the caller's responsibility.
 *
 * Volatile ops may or may not imply this barrier, depending on the /volatile: switch, but adding an extra
 * barrier doesn't hurt.
 */
#    define AWS_SW_BARRIER() _ReadWriteBarrier(); /* software barrier */
#endif

static inline void aws_atomic_priv_check_order(enum aws_memory_order order) {
#ifndef NDEBUG
    switch (order) {
        case aws_memory_order_relaxed:
            return;
        case aws_memory_order_acquire:
            return;
        case aws_memory_order_release:
            return;
        case aws_memory_order_acq_rel:
            return;
        case aws_memory_order_seq_cst:
            return;
        default: /* Unknown memory order */
            abort();
    }
#endif
    (void)order;
}

enum aws_atomic_mode_priv { aws_atomic_priv_load, aws_atomic_priv_store };

static inline void aws_atomic_priv_barrier_before(enum aws_memory_order order, enum aws_atomic_mode_priv mode) {
    aws_atomic_priv_check_order(order);
    AWS_ASSERT(mode != aws_atomic_priv_load || order != aws_memory_order_release);

    if (order == aws_memory_order_relaxed) {
        /* no barriers required for relaxed mode */
        return;
    }

    if (order == aws_memory_order_acquire || mode == aws_atomic_priv_load) {
        /* for acquire, we need only use a barrier afterward */
        return;
    }

    AWS_RW_BARRIER();
    AWS_SW_BARRIER();
}

static inline void aws_atomic_priv_barrier_after(enum aws_memory_order order, enum aws_atomic_mode_priv mode) {
    aws_atomic_priv_check_order(order);
    AWS_ASSERT(mode != aws_atomic_priv_store || order != aws_memory_order_acquire);

    if (order == aws_memory_order_relaxed) {
        /* no barriers required for relaxed mode */
        return;
    }

    if (order == aws_memory_order_release || mode == aws_atomic_priv_store) {
        /* for release, we need only use a barrier before */
        return;
    }

    AWS_RW_BARRIER();
    AWS_SW_BARRIER();
}

/**
 * Initializes an atomic variable with an integer value. This operation should be done before any
 * other operations on this atomic variable, and must be done before attempting any parallel operations.
 */
AWS_STATIC_IMPL
void aws_atomic_init_int(volatile struct aws_atomic_var *var, size_t n) {
    AWS_ATOMIC_VAR_INTVAL(var) = (aws_atomic_impl_int_t)n;
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
    aws_atomic_priv_barrier_before(memory_order, aws_atomic_priv_load);
    size_t result = (size_t)AWS_ATOMIC_VAR_INTVAL(var);
    aws_atomic_priv_barrier_after(memory_order, aws_atomic_priv_load);
    return result;
}

/**
 * Reads an atomic var as an pointer, using the specified ordering, and returns the result.
 */
AWS_STATIC_IMPL
void *aws_atomic_load_ptr_explicit(volatile const struct aws_atomic_var *var, enum aws_memory_order memory_order) {
    aws_atomic_priv_barrier_before(memory_order, aws_atomic_priv_load);
    void *result = AWS_ATOMIC_VAR_PTRVAL(var);
    aws_atomic_priv_barrier_after(memory_order, aws_atomic_priv_load);
    return result;
}

/**
 * Stores an integer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_int_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order memory_order) {
    if (memory_order != aws_memory_order_seq_cst) {
        aws_atomic_priv_barrier_before(memory_order, aws_atomic_priv_store);
        AWS_ATOMIC_VAR_INTVAL(var) = (aws_atomic_impl_int_t)n;
        aws_atomic_priv_barrier_after(memory_order, aws_atomic_priv_store);
    } else {
        AWS_INTERLOCKED_INT(Exchange)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
    }
}

/**
 * Stores an pointer into an atomic var, using the specified ordering.
 */
AWS_STATIC_IMPL
void aws_atomic_store_ptr_explicit(volatile struct aws_atomic_var *var, void *p, enum aws_memory_order memory_order) {
    aws_atomic_priv_check_order(memory_order);
    if (memory_order != aws_memory_order_seq_cst) {
        aws_atomic_priv_barrier_before(memory_order, aws_atomic_priv_store);
        AWS_ATOMIC_VAR_PTRVAL(var) = p;
        aws_atomic_priv_barrier_after(memory_order, aws_atomic_priv_store);
    } else {
        _InterlockedExchangePointer(&AWS_ATOMIC_VAR_PTRVAL(var), p);
    }
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
    aws_atomic_priv_check_order(memory_order);
    return (size_t)AWS_INTERLOCKED_INT(Exchange)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
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
    aws_atomic_priv_check_order(memory_order);
    return _InterlockedExchangePointer(&AWS_ATOMIC_VAR_PTRVAL(var), p);
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
    aws_atomic_priv_check_order(order_success);
    aws_atomic_priv_check_order(order_failure);

    size_t oldval = (size_t)AWS_INTERLOCKED_INT(CompareExchange)(
        &AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)desired, (aws_atomic_impl_int_t)*expected);
    bool successful = oldval == *expected;
    *expected = oldval;

    return successful;
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
    aws_atomic_priv_check_order(order_success);
    aws_atomic_priv_check_order(order_failure);

    void *oldval = _InterlockedCompareExchangePointer(&AWS_ATOMIC_VAR_PTRVAL(var), desired, *expected);
    bool successful = oldval == *expected;
    *expected = oldval;

    return successful;
}

/**
 * Atomically adds n to *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_add_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    aws_atomic_priv_check_order(order);

    return (size_t)AWS_INTERLOCKED_INT(ExchangeAdd)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
}

/**
 * Atomically subtracts n from *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_sub_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    aws_atomic_priv_check_order(order);

    return (size_t)AWS_INTERLOCKED_INT(ExchangeAdd)(&AWS_ATOMIC_VAR_INTVAL(var), -(aws_atomic_impl_int_t)n);
}

/**
 * Atomically ORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_or_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    aws_atomic_priv_check_order(order);

    return (size_t)AWS_INTERLOCKED_INT(Or)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
}

/**
 * Atomically ANDs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_and_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    aws_atomic_priv_check_order(order);

    return (size_t)AWS_INTERLOCKED_INT(And)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
}

/**
 * Atomically XORs n with *var, and returns the previous value of *var.
 */
AWS_STATIC_IMPL
size_t aws_atomic_fetch_xor_explicit(volatile struct aws_atomic_var *var, size_t n, enum aws_memory_order order) {
    aws_atomic_priv_check_order(order);

    return (size_t)AWS_INTERLOCKED_INT(Xor)(&AWS_ATOMIC_VAR_INTVAL(var), (aws_atomic_impl_int_t)n);
}

/**
 * Provides the same reordering guarantees as an atomic operation with the specified memory order, without
 * needing to actually perform an atomic operation.
 */
AWS_STATIC_IMPL
void aws_atomic_thread_fence(enum aws_memory_order order) {
    volatile aws_atomic_impl_int_t x = 0;
    aws_atomic_priv_check_order(order);

    /* On x86: A compiler barrier is sufficient for anything short of seq_cst */

    switch (order) {
        case aws_memory_order_seq_cst:
            AWS_INTERLOCKED_INT(Exchange)(&x, 1);
            break;
        case aws_memory_order_release:
            AWS_W_BARRIER();
            AWS_SW_BARRIER();
            break;
        case aws_memory_order_acquire:
            AWS_R_BARRIER();
            AWS_SW_BARRIER();
            break;
        case aws_memory_order_acq_rel:
            AWS_RW_BARRIER();
            AWS_SW_BARRIER();
            break;
        case aws_memory_order_relaxed:
            /* no-op */
            break;
    }
}

/* prevent conflicts with other files that might pick the same names */
#undef AWS_RW_BARRIER
#undef AWS_R_BARRIER
#undef AWS_W_BARRIER
#undef AWS_SW_BARRIER

#define AWS_ATOMICS_HAVE_THREAD_FENCE
AWS_EXTERN_C_END
#endif
