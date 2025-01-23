#ifndef AWS_COMMON_THREAD_SCHEDULER_H
#define AWS_COMMON_THREAD_SCHEDULER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_thread_scheduler;
struct aws_thread_options;
struct aws_task;

AWS_EXTERN_C_BEGIN

/**
 * Creates a new instance of a thread scheduler. This object receives scheduled tasks and executes them inside a
 * background thread. On success, this function returns an instance with a ref-count of 1. On failure it returns NULL.
 *
 * thread_options are optional.
 *
 * The semantics of this interface conform to the semantics of aws_task_scheduler.
 */
AWS_COMMON_API
struct aws_thread_scheduler *aws_thread_scheduler_new(
    struct aws_allocator *allocator,
    const struct aws_thread_options *thread_options);

/**
 * Acquire a reference to the scheduler.
 */
AWS_COMMON_API void aws_thread_scheduler_acquire(struct aws_thread_scheduler *scheduler);

/**
 * Release a reference to the scheduler.
 */
AWS_COMMON_API void aws_thread_scheduler_release(const struct aws_thread_scheduler *scheduler);

/**
 * Schedules a task to run in the future. time_to_run is the absolute time from the system hw_clock.
 */
AWS_COMMON_API void aws_thread_scheduler_schedule_future(
    struct aws_thread_scheduler *scheduler,
    struct aws_task *task,
    uint64_t time_to_run);

/**
 * Schedules a task to run as soon as possible.
 */
AWS_COMMON_API void aws_thread_scheduler_schedule_now(struct aws_thread_scheduler *scheduler, struct aws_task *task);

/**
 * Cancel a task that has been scheduled. The cancellation callback will be invoked in the background thread.
 * This function is slow, so please don't do it in the hot path for your code.
 */
AWS_COMMON_API void aws_thread_scheduler_cancel_task(struct aws_thread_scheduler *scheduler, struct aws_task *task);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_THREAD_SCHEDULER_H */
