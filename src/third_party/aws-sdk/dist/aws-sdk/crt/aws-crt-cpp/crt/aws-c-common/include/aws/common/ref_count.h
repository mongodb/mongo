#ifndef AWS_COMMON_REF_COUNT_H
#define AWS_COMMON_REF_COUNT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

#include <aws/common/atomics.h>
#include <aws/common/shutdown_types.h>

AWS_PUSH_SANE_WARNING_LEVEL

/*
 * A utility type for making ref-counted types, reminiscent of std::shared_ptr in C++
 */
struct aws_ref_count {
    struct aws_atomic_var ref_count;
    void *object;
    aws_simple_completion_callback *on_zero_fn;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes a ref-counter structure.  After initialization, the ref count will be 1.
 *
 * @param ref_count ref-counter to initialize
 * @param object object being ref counted
 * @param on_zero_fn function to invoke when the ref count reaches zero
 */
AWS_COMMON_API void aws_ref_count_init(
    struct aws_ref_count *ref_count,
    void *object,
    aws_simple_completion_callback *on_zero_fn);

/**
 * Increments a ref-counter's ref count
 *
 * @param ref_count ref-counter to increment the count for
 * @return the object being ref-counted
 */
AWS_COMMON_API void *aws_ref_count_acquire(struct aws_ref_count *ref_count);

/**
 * Decrements a ref-counter's ref count.  Invokes the on_zero callback if the ref count drops to zero
 * @param ref_count ref-counter to decrement the count for
 * @return the value of the decremented ref count
 */
AWS_COMMON_API size_t aws_ref_count_release(struct aws_ref_count *ref_count);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_REF_COUNT_H */
