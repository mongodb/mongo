#ifndef AWS_COMMON_CROSS_PROCESS_LOCK_H
#define AWS_COMMON_CROSS_PROCESS_LOCK_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

struct aws_cross_process_lock;
AWS_EXTERN_C_BEGIN

/**
 * Attempts to acquire a system-wide (not per process or per user) lock scoped by instance_nonce.
 * For any given unique nonce, a lock will be returned by the first caller. Subsequent calls will
 * return NULL and raise AWS_ERROR_MUTEX_CALLER_NOT_OWNER
 * until the either the process owning the lock exits or the program owning the lock
 * calls aws_cross_process_lock_release() explicitly.
 *
 * If the process exits before the lock is released, the kernel will unlock it for the next consumer.
 */
AWS_COMMON_API
struct aws_cross_process_lock *aws_cross_process_lock_try_acquire(
    struct aws_allocator *allocator,
    struct aws_byte_cursor instance_nonce);

/**
 * Releases the lock so the next caller (may be another process) can get an instance of the lock.
 */
AWS_COMMON_API
void aws_cross_process_lock_release(struct aws_cross_process_lock *instance_lock);

AWS_EXTERN_C_END

#endif /* AWS_COMMON_CROSS_PROCESS_LOCK_H */
