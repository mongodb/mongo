/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/ref_count.h>

#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>

void aws_ref_count_init(struct aws_ref_count *ref_count, void *object, aws_simple_completion_callback *on_zero_fn) {
    aws_atomic_init_int(&ref_count->ref_count, 1);
    ref_count->object = object;
    ref_count->on_zero_fn = on_zero_fn;
}

void *aws_ref_count_acquire(struct aws_ref_count *ref_count) {
    size_t old_value = aws_atomic_fetch_add(&ref_count->ref_count, 1);
    AWS_ASSERT(old_value > 0 && "refcount has been zero, it's invalid to use it again.");
    (void)old_value;

    return ref_count->object;
}

size_t aws_ref_count_release(struct aws_ref_count *ref_count) {
    size_t old_value = aws_atomic_fetch_sub(&ref_count->ref_count, 1);
    AWS_ASSERT(old_value > 0 && "refcount has gone negative");
    if (old_value == 1) {
        ref_count->on_zero_fn(ref_count->object);
    }

    return old_value - 1;
}
