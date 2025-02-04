#ifndef AWS_IO_EVENT_LOOP_H
#define AWS_IO_EVENT_LOOP_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_event_loop;
struct aws_event_loop_group;
struct aws_shutdown_callback_options;
struct aws_task;

/**
 * @internal
 */
typedef void(aws_event_loop_on_event_fn)(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data);

/**
 * @internal
 */
struct aws_event_loop_vtable {
    void (*destroy)(struct aws_event_loop *event_loop);
    int (*run)(struct aws_event_loop *event_loop);
    int (*stop)(struct aws_event_loop *event_loop);
    int (*wait_for_stop_completion)(struct aws_event_loop *event_loop);
    void (*schedule_task_now)(struct aws_event_loop *event_loop, struct aws_task *task);
    void (*schedule_task_future)(struct aws_event_loop *event_loop, struct aws_task *task, uint64_t run_at_nanos);
    void (*cancel_task)(struct aws_event_loop *event_loop, struct aws_task *task);
    int (*connect_to_io_completion_port)(struct aws_event_loop *event_loop, struct aws_io_handle *handle);
    int (*subscribe_to_io_events)(
        struct aws_event_loop *event_loop,
        struct aws_io_handle *handle,
        int events,
        aws_event_loop_on_event_fn *on_event,
        void *user_data);
    int (*unsubscribe_from_io_events)(struct aws_event_loop *event_loop, struct aws_io_handle *handle);
    void (*free_io_event_resources)(void *user_data);
    bool (*is_on_callers_thread)(struct aws_event_loop *event_loop);
};

/**
 * Event loop group configuration options
 */
struct aws_event_loop_group_options {

    /**
     * How many event loops that event loop group should contain.  For most group types, this implies
     * the creation and management of an analagous amount of managed threads
     */
    uint16_t loop_count;

    /**
     * Optional callback to invoke when the event loop group finishes destruction.
     */
    const struct aws_shutdown_callback_options *shutdown_options;

    /**
     * Optional configuration to control how the event loop group's threads bind to CPU groups
     */
    const uint16_t *cpu_group;

    /**
     * Override for the clock function that event loops should use.  Defaults to the system's high resolution
     * timer.
     *
     * Do not bind this value to managed code; it is only used in timing-sensitive tests.
     */
    aws_io_clock_fn *clock_override;
};

AWS_EXTERN_C_BEGIN

/**
 * The event loop will schedule the task and run it on the event loop thread as soon as possible.
 * Note that cancelled tasks may execute outside the event loop thread.
 * This function may be called from outside or inside the event loop thread.
 *
 * The task should not be cleaned up or modified until its function is executed.
 */
AWS_IO_API
void aws_event_loop_schedule_task_now(struct aws_event_loop *event_loop, struct aws_task *task);

/**
 * The event loop will schedule the task and run it at the specified time.
 * Use aws_event_loop_current_clock_time() to query the current time in nanoseconds.
 * Note that cancelled tasks may execute outside the event loop thread.
 * This function may be called from outside or inside the event loop thread.
 *
 * The task should not be cleaned up or modified until its function is executed.
 */
AWS_IO_API
void aws_event_loop_schedule_task_future(
    struct aws_event_loop *event_loop,
    struct aws_task *task,
    uint64_t run_at_nanos);

/**
 * Cancels task.
 * This function must be called from the event loop's thread, and is only guaranteed
 * to work properly on tasks scheduled from within the event loop's thread.
 * The task will be executed with the AWS_TASK_STATUS_CANCELED status inside this call.
 */
AWS_IO_API
void aws_event_loop_cancel_task(struct aws_event_loop *event_loop, struct aws_task *task);

/**
 * Returns true if the event loop's thread is the same thread that called this function, otherwise false.
 */
AWS_IO_API
bool aws_event_loop_thread_is_callers_thread(struct aws_event_loop *event_loop);

/**
 * Gets the current timestamp for the event loop's clock, in nanoseconds. This function is thread-safe.
 */
AWS_IO_API
int aws_event_loop_current_clock_time(const struct aws_event_loop *event_loop, uint64_t *time_nanos);

/**
 * Creation function for event loop groups.
 */
AWS_IO_API
struct aws_event_loop_group *aws_event_loop_group_new(
    struct aws_allocator *allocator,
    const struct aws_event_loop_group_options *options);

/**
 * Increments the reference count on the event loop group, allowing the caller to take a reference to it.
 *
 * Returns the same event loop group passed in.
 */
AWS_IO_API
struct aws_event_loop_group *aws_event_loop_group_acquire(struct aws_event_loop_group *el_group);

/**
 * Decrements an event loop group's ref count.  When the ref count drops to zero, the event loop group will be
 * destroyed.
 */
AWS_IO_API
void aws_event_loop_group_release(struct aws_event_loop_group *el_group);

/**
 * Returns the event loop at a particular index.  If the index is out of bounds, null is returned.
 */
AWS_IO_API
struct aws_event_loop *aws_event_loop_group_get_loop_at(struct aws_event_loop_group *el_group, size_t index);

/**
 * Gets the number of event loops managed by an event loop group.
 */
AWS_IO_API
size_t aws_event_loop_group_get_loop_count(const struct aws_event_loop_group *el_group);

/**
 * Fetches the next loop for use. The purpose is to enable load balancing across loops. You should not depend on how
 * this load balancing is done as it is subject to change in the future. Currently it uses the "best-of-two" algorithm
 * based on the load factor of each loop.
 */
AWS_IO_API
struct aws_event_loop *aws_event_loop_group_get_next_loop(struct aws_event_loop_group *el_group);

/**
 * @deprecated - use aws_event_loop_group_new() instead
 */
AWS_IO_API
struct aws_event_loop_group *aws_event_loop_group_new_default(
    struct aws_allocator *alloc,
    uint16_t max_threads,
    const struct aws_shutdown_callback_options *shutdown_options);

/**
 * @deprecated - use aws_event_loop_group_new() instead
 */
AWS_IO_API
struct aws_event_loop_group *aws_event_loop_group_new_default_pinned_to_cpu_group(
    struct aws_allocator *alloc,
    uint16_t max_threads,
    uint16_t cpu_group,
    const struct aws_shutdown_callback_options *shutdown_options);

/**
 * @internal - Don't use outside of testing.
 *
 * Returns the opaque internal user data of an event loop.  Can be cast into a specific implementation by
 * privileged consumers.
 */
AWS_IO_API
void *aws_event_loop_get_impl(struct aws_event_loop *event_loop);

/**
 * @internal - Don't use outside of testing.
 *
 * Initializes the base structure used by all event loop implementations with test-oriented overrides.
 */
AWS_IO_API
struct aws_event_loop *aws_event_loop_new_base(
    struct aws_allocator *allocator,
    aws_io_clock_fn *clock,
    struct aws_event_loop_vtable *vtable,
    void *impl);

/**
 * @internal - Don't use outside of testing.
 *
 * Common cleanup code for all implementations.
 * This is only called from the *destroy() function of event loop implementations.
 */
AWS_IO_API
void aws_event_loop_clean_up_base(struct aws_event_loop *event_loop);

/**
 * @internal - Don't use outside of testing.
 *
 * Invokes the destroy() fn for the event loop implementation.
 * If the event loop is still in a running state, this function will block waiting on the event loop to shutdown.
 * If you do not want this function to block, call aws_event_loop_stop() manually first.
 * If the event loop is shared by multiple threads then destroy must be called by exactly one thread. All other threads
 * must ensure their API calls to the event loop happen-before the call to destroy.
 */
AWS_IO_API
void aws_event_loop_destroy(struct aws_event_loop *event_loop);

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_EVENT_LOOP_H */
