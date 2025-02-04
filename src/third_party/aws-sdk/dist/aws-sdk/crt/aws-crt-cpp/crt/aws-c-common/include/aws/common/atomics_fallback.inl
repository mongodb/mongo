#ifndef AWS_COMMON_ATOMICS_FALLBACK_INL
#define AWS_COMMON_ATOMICS_FALLBACK_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

AWS_EXTERN_C_BEGIN

#ifndef AWS_ATOMICS_HAVE_THREAD_FENCE

void aws_atomic_thread_fence(enum aws_memory_order order) {
    struct aws_atomic_var var;
    aws_atomic_int_t expected = 0;

    aws_atomic_store_int(&var, expected, aws_memory_order_relaxed);
    aws_atomic_compare_exchange_int(&var, &expected, 1, order, aws_memory_order_relaxed);
}

#endif /* AWS_ATOMICS_HAVE_THREAD_FENCE */

AWS_EXTERN_C_END
#endif /* AWS_COMMON_ATOMICS_FALLBACK_INL */
