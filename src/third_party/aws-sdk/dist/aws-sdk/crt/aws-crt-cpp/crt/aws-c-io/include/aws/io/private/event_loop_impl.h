#ifndef AWS_IO_EVENT_LOOP_IMPL_H
#define AWS_IO_EVENT_LOOP_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

#include <aws/common/atomics.h>
#include <aws/common/hash_table.h>
#include <aws/common/ref_count.h>
#include <aws/io/event_loop.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_event_loop;
struct aws_overlapped;

typedef void(aws_event_loop_on_completion_fn)(
    struct aws_event_loop *event_loop,
    struct aws_overlapped *overlapped,
    int status_code,
    size_t num_bytes_transferred);

/**
 * The aws_win32_OVERLAPPED struct is layout-compatible with OVERLAPPED as defined in <Windows.h>. It is used
 * here to avoid pulling in a dependency on <Windows.h> which would also bring along a lot of bad macros, such
 * as redefinitions of GetMessage and GetObject. Note that the OVERLAPPED struct layout in the Windows SDK can
 * never be altered without breaking binary compatibility for every existing third-party executable, so there
 * is no need to worry about keeping this definition in sync.
 */
struct aws_win32_OVERLAPPED {
    uintptr_t Internal;
    uintptr_t InternalHigh;
    union {
        struct {
            uint32_t Offset;
            uint32_t OffsetHigh;
        } s;
        void *Pointer;
    } u;
    void *hEvent;
};

/**
 * Use aws_overlapped when a handle connected to the event loop needs an OVERLAPPED struct.
 * OVERLAPPED structs are needed to make OS-level async I/O calls.
 * When the I/O completes, the assigned aws_event_loop_on_completion_fn is called from the event_loop's thread.
 * While the I/O is pending, it is not safe to modify or delete aws_overlapped.
 * Call aws_overlapped_init() before first use. If the aws_overlapped will be used multiple times, call
 * aws_overlapped_reset() or aws_overlapped_init() between uses.
 */
struct aws_overlapped {
    struct aws_win32_OVERLAPPED overlapped;
    aws_event_loop_on_completion_fn *on_completion;
    void *user_data;
};

enum aws_io_event_type {
    AWS_IO_EVENT_TYPE_READABLE = 1,
    AWS_IO_EVENT_TYPE_WRITABLE = 2,
    AWS_IO_EVENT_TYPE_REMOTE_HANG_UP = 4,
    AWS_IO_EVENT_TYPE_CLOSED = 8,
    AWS_IO_EVENT_TYPE_ERROR = 16,
};

struct aws_event_loop {
    struct aws_event_loop_vtable *vtable;
    struct aws_allocator *alloc;
    aws_io_clock_fn *clock;
    struct aws_hash_table local_data;
    struct aws_atomic_var current_load_factor;
    uint64_t latest_tick_start;
    size_t current_tick_latency_sum;
    struct aws_atomic_var next_flush_time;
    void *impl_data;
};

struct aws_event_loop_local_object;
typedef void(aws_event_loop_on_local_object_removed_fn)(struct aws_event_loop_local_object *);

struct aws_event_loop_local_object {
    const void *key;
    void *object;
    aws_event_loop_on_local_object_removed_fn *on_object_removed;
};

struct aws_event_loop_options {
    aws_io_clock_fn *clock;
    struct aws_thread_options *thread_options;
};

typedef struct aws_event_loop *(aws_new_event_loop_fn)(struct aws_allocator *alloc,
                                                       const struct aws_event_loop_options *options,
                                                       void *new_loop_user_data);

struct aws_event_loop_group {
    struct aws_allocator *allocator;
    struct aws_array_list event_loops;
    struct aws_ref_count ref_count;
    struct aws_shutdown_callback_options shutdown_options;
};

AWS_EXTERN_C_BEGIN

#ifdef AWS_USE_IO_COMPLETION_PORTS

/**
 * Prepares aws_overlapped for use, and sets a function to call when the overlapped operation completes.
 */
AWS_IO_API
void aws_overlapped_init(
    struct aws_overlapped *overlapped,
    aws_event_loop_on_completion_fn *on_completion,
    void *user_data);

/**
 * Prepares aws_overlapped for re-use without changing the assigned aws_event_loop_on_completion_fn.
 * Call aws_overlapped_init(), instead of aws_overlapped_reset(), to change the aws_event_loop_on_completion_fn.
 */
AWS_IO_API
void aws_overlapped_reset(struct aws_overlapped *overlapped);

/**
 * Casts an aws_overlapped pointer for use as a LPOVERLAPPED parameter to Windows API functions
 */
AWS_IO_API
struct _OVERLAPPED *aws_overlapped_to_windows_overlapped(struct aws_overlapped *overlapped);

/**
 * Associates an aws_io_handle with the event loop's I/O Completion Port.
 *
 * The handle must use aws_overlapped for all async operations requiring an OVERLAPPED struct.
 * When the operation completes, the aws_overlapped's completion function will run on the event loop thread.
 * Note that completion functions will not be invoked while the event loop is stopped. Users should wait for all async
 * operations on connected handles to complete before cleaning up or destroying the event loop.
 *
 * A handle may only be connected to one event loop in its lifetime.
 */
AWS_IO_API
int aws_event_loop_connect_handle_to_io_completion_port(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle);

#else

/**
 * Subscribes on_event to events on the event-loop for handle. events is a bitwise concatenation of the events that were
 * received. The definition for these values can be found in aws_io_event_type. Currently, only
 * AWS_IO_EVENT_TYPE_READABLE and AWS_IO_EVENT_TYPE_WRITABLE are honored. You always are registered for error conditions
 * and closure. This function may be called from outside or inside the event loop thread. However, the unsubscribe
 * function must be called inside the event-loop's thread.
 */
AWS_IO_API
int aws_event_loop_subscribe_to_io_events(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    aws_event_loop_on_event_fn *on_event,
    void *user_data);

#endif /* AWS_USE_IO_COMPLETION_PORTS */

/**
 * Creates an instance of the default event loop implementation for the current architecture and operating system.
 */
AWS_IO_API
struct aws_event_loop *aws_event_loop_new_default(struct aws_allocator *alloc, aws_io_clock_fn *clock);

/**
 * Creates an instance of the default event loop implementation for the current architecture and operating system using
 * extendable options.
 */
AWS_IO_API
struct aws_event_loop *aws_event_loop_new_default_with_options(
    struct aws_allocator *alloc,
    const struct aws_event_loop_options *options);

/**
 * Initializes common event-loop data structures.
 * This is only called from the *new() function of event loop implementations.
 */
AWS_IO_API
int aws_event_loop_init_base(struct aws_event_loop *event_loop, struct aws_allocator *alloc, aws_io_clock_fn *clock);

/**
 * Fetches an object from the event-loop's data store. Key will be taken as the memory address of the memory pointed to
 * by key. This function is not thread safe and should be called inside the event-loop's thread.
 */
AWS_IO_API
int aws_event_loop_fetch_local_object(
    struct aws_event_loop *event_loop,
    void *key,
    struct aws_event_loop_local_object *obj);

/**
 * Puts an item object the event-loop's data store. Key will be taken as the memory address of the memory pointed to by
 * key. The lifetime of item must live until remove or a put item overrides it. This function is not thread safe and
 * should be called inside the event-loop's thread.
 */
AWS_IO_API
int aws_event_loop_put_local_object(struct aws_event_loop *event_loop, struct aws_event_loop_local_object *obj);

/**
 * Removes an object from the event-loop's data store. Key will be taken as the memory address of the memory pointed to
 * by key. If removed_item is not null, the removed item will be moved to it if it exists. Otherwise, the default
 * deallocation strategy will be used. This function is not thread safe and should be called inside the event-loop's
 * thread.
 */
AWS_IO_API
int aws_event_loop_remove_local_object(
    struct aws_event_loop *event_loop,
    void *key,
    struct aws_event_loop_local_object *removed_obj);

/**
 * Triggers the running of the event loop. This function must not block. The event loop is not active until this
 * function is invoked. This function can be called again on an event loop after calling aws_event_loop_stop() and
 * aws_event_loop_wait_for_stop_completion().
 */
AWS_IO_API
int aws_event_loop_run(struct aws_event_loop *event_loop);

/**
 * Triggers the event loop to stop, but does not wait for the loop to stop completely.
 * This function may be called from outside or inside the event loop thread. It is safe to call multiple times.
 * This function is called from destroy().
 *
 * If you do not call destroy(), an event loop can be run again by calling stop(), wait_for_stop_completion(), run().
 */
AWS_IO_API
int aws_event_loop_stop(struct aws_event_loop *event_loop);

/**
 * For event-loop implementations to use for providing metrics info to the base event-loop. This enables the
 * event-loop load balancer to take into account load when vending another event-loop to a caller.
 *
 * Call this function at the beginning of your event-loop tick: after wake-up, but before processing any IO or tasks.
 */
AWS_IO_API
void aws_event_loop_register_tick_start(struct aws_event_loop *event_loop);

/**
 * For event-loop implementations to use for providing metrics info to the base event-loop. This enables the
 * event-loop load balancer to take into account load when vending another event-loop to a caller.
 *
 * Call this function at the end of your event-loop tick: after processing IO and tasks.
 */
AWS_IO_API
void aws_event_loop_register_tick_end(struct aws_event_loop *event_loop);

/**
 * Returns the current load factor (however that may be calculated). If the event-loop is not invoking
 * aws_event_loop_register_tick_start() and aws_event_loop_register_tick_end(), this value will always be 0.
 */
AWS_IO_API
size_t aws_event_loop_get_load_factor(struct aws_event_loop *event_loop);

/**
 * Blocks until the event loop stops completely.
 * If you want to call aws_event_loop_run() again, you must call this after aws_event_loop_stop().
 * It is not safe to call this function from inside the event loop thread.
 */
AWS_IO_API
int aws_event_loop_wait_for_stop_completion(struct aws_event_loop *event_loop);

/**
 * Unsubscribes handle from event-loop notifications.
 * This function is not thread safe and should be called inside the event-loop's thread.
 *
 * NOTE: if you are using io completion ports, this is a risky call. We use it in places, but only when we're certain
 * there's no pending events. If you want to use it, it's your job to make sure you don't have pending events before
 * calling it.
 */
AWS_IO_API
int aws_event_loop_unsubscribe_from_io_events(struct aws_event_loop *event_loop, struct aws_io_handle *handle);

/**
 * Cleans up resources (user_data) associated with the I/O eventing subsystem for a given handle. This should only
 * ever be necessary in the case where you are cleaning up an event loop during shutdown and its thread has already
 * been joined.
 */
AWS_IO_API
void aws_event_loop_free_io_event_resources(struct aws_event_loop *event_loop, struct aws_io_handle *handle);

AWS_IO_API
struct aws_event_loop_group *aws_event_loop_group_new_internal(
    struct aws_allocator *allocator,
    const struct aws_event_loop_group_options *options,
    aws_new_event_loop_fn *new_loop_fn,
    void *new_loop_user_data);

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_EVENT_LOOP_IMPL_H */
