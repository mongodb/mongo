#ifndef AWS_IO_CHANNEL_H
#define AWS_IO_CHANNEL_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/io.h>

#include <aws/common/statistics.h>
#include <aws/common/task_scheduler.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_channel_direction {
    AWS_CHANNEL_DIR_READ,
    AWS_CHANNEL_DIR_WRITE,
};

struct aws_channel;
struct aws_channel_slot;
struct aws_channel_handler;
struct aws_event_loop;
struct aws_event_loop_local_object;

typedef void(aws_channel_on_setup_completed_fn)(struct aws_channel *channel, int error_code, void *user_data);

/* Callback called when a channel is completely shutdown. error_code refers to the reason the channel was closed. */
typedef void(aws_channel_on_shutdown_completed_fn)(struct aws_channel *channel, int error_code, void *user_data);

struct aws_channel_slot {
    struct aws_allocator *alloc;
    struct aws_channel *channel;
    struct aws_channel_slot *adj_left;
    struct aws_channel_slot *adj_right;
    struct aws_channel_handler *handler;
    size_t window_size;
    size_t upstream_message_overhead;
    size_t current_window_update_batch_size;
};

struct aws_channel_task;
typedef void(aws_channel_task_fn)(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status);

struct aws_channel_task {
    struct aws_task wrapper_task;
    aws_channel_task_fn *task_fn;
    void *arg;
    const char *type_tag;
    struct aws_linked_list_node node;
};

struct aws_channel_handler_vtable {
    /**
     * Called by the channel when a message is available for processing in the read direction. It is your
     * responsibility to call aws_mem_release(message->allocator, message); on message when you are finished with it.
     * You must only call `aws_mem_release(message->allocator, message);` if the `process_read_message`
     * returns AWS_OP_SUCCESS. In case of an error, you must not clean up the message and should just raise the error.
     *
     * Also keep in mind that your slot's internal window has been decremented. You'll want to call
     * aws_channel_slot_increment_read_window() at some point in the future if you want to keep receiving data.
     */
    int (*process_read_message)(
        struct aws_channel_handler *handler,
        struct aws_channel_slot *slot,
        struct aws_io_message *message);
    /**
     * Called by the channel when a message is available for processing in the write direction. It is your
     * responsibility to call aws_mem_release(message->allocator, message); on message when you are finished with it.
     * You must only call `aws_mem_release(message->allocator, message);` if the `process_read_message`
     * returns AWS_OP_SUCCESS. In case of an error, you must not clean up the message and should just raise the error.
     */
    int (*process_write_message)(
        struct aws_channel_handler *handler,
        struct aws_channel_slot *slot,
        struct aws_io_message *message);
    /**
     * Called by the channel when a downstream handler has issued a window increment. You'll want to update your
     * internal state and likely propagate a window increment message of your own by calling
     * 'aws_channel_slot_increment_read_window()'
     */
    int (*increment_read_window)(struct aws_channel_handler *handler, struct aws_channel_slot *slot, size_t size);

    /**
     * The channel calls shutdown on all handlers twice, once to shut down reading, and once to shut down writing.
     * Shutdown always begins with the left-most handler, and proceeds to the right with dir set to
     * AWS_CHANNEL_DIR_READ. Then shutdown is called on handlers from right to left with dir set to
     * AWS_CHANNEL_DIR_WRITE.
     *
     * The shutdown process does not need to complete immediately and may rely on scheduled tasks.
     * The handler must call aws_channel_slot_on_handler_shutdown_complete() when it is finished,
     * which propagates shutdown to the next handler.  If 'free_scarce_resources_immediately' is true,
     * then resources vulnerable to denial-of-service attacks (such as sockets and file handles)
     * must be closed immediately before the shutdown() call returns.
     */
    int (*shutdown)(
        struct aws_channel_handler *handler,
        struct aws_channel_slot *slot,
        enum aws_channel_direction dir,
        int error_code,
        bool free_scarce_resources_immediately);

    /**
     * Called by the channel when the handler is added to a slot, to get the initial window size.
     */
    size_t (*initial_window_size)(struct aws_channel_handler *handler);

    /** Called by the channel anytime a handler is added or removed, provides a hint for downstream
     * handlers to avoid message fragmentation due to message overhead. */
    size_t (*message_overhead)(struct aws_channel_handler *handler);

    /**
     * Clean up any resources and deallocate yourself. The shutdown process will already be completed before this
     * function is called.
     */
    void (*destroy)(struct aws_channel_handler *handler);

    /**
     * Directs the channel handler to reset all of the internal statistics it tracks about itself.
     */
    void (*reset_statistics)(struct aws_channel_handler *handler);

    /**
     * Adds a pointer to the handler's internal statistics (if they exist) to a list of statistics structures
     * associated with the channel's handler chain.
     */
    void (*gather_statistics)(struct aws_channel_handler *handler, struct aws_array_list *stats_list);

    /*
     * If this handler represents a source of data (like the socket_handler), then this will trigger a read
     * from the data source.
     */
    void (*trigger_read)(struct aws_channel_handler *handler);
};

struct aws_channel_handler {
    struct aws_channel_handler_vtable *vtable;
    struct aws_allocator *alloc;
    struct aws_channel_slot *slot;
    void *impl;
};

/**
 * Args for creating a new channel.
 *  event_loop to use for IO and tasks. on_setup_completed will be invoked when
 *  the setup process is finished It will be executed in the event loop's thread.
 *  on_shutdown_completed will be executed upon channel shutdown.
 *
 *  enable_read_back_pressure toggles whether or not back pressure will be applied in the channel.
 *  Leave this option off unless you're using something like reactive-streams, since it is a slight throughput
 *  penalty.
 *
 *  Unless otherwise
 *  specified all functions for channels and channel slots must be executed within that channel's event-loop's thread.
 **/
struct aws_channel_options {
    struct aws_event_loop *event_loop;
    aws_channel_on_setup_completed_fn *on_setup_completed;
    aws_channel_on_shutdown_completed_fn *on_shutdown_completed;
    void *setup_user_data;
    void *shutdown_user_data;
    bool enable_read_back_pressure;
};

AWS_EXTERN_C_BEGIN

extern AWS_IO_API size_t g_aws_channel_max_fragment_size;

/**
 * Initializes channel_task for use.
 */
AWS_IO_API
void aws_channel_task_init(
    struct aws_channel_task *channel_task,
    aws_channel_task_fn *task_fn,
    void *arg,
    const char *type_tag);

/**
 * Allocates new channel, Unless otherwise specified all functions for channels and channel slots must be executed
 * within that channel's event-loop's thread. channel_options are copied.
 */
AWS_IO_API
struct aws_channel *aws_channel_new(struct aws_allocator *allocator, const struct aws_channel_options *creation_args);

/**
 * Mark the channel, along with all slots and handlers, for destruction.
 * Must be called after shutdown has completed.
 * Can be called from any thread assuming 'aws_channel_shutdown()' has completed.
 * Note that memory will not be freed until all users which acquired holds on the channel via
 * aws_channel_acquire_hold(), release them via aws_channel_release_hold().
 */
AWS_IO_API
void aws_channel_destroy(struct aws_channel *channel);

/**
 * Initiates shutdown of the channel. Shutdown will begin with the left-most slot. Each handler will invoke
 * 'aws_channel_slot_on_handler_shutdown_complete' once they've finished their shutdown process for the read direction.
 * Once the right-most slot has shutdown in the read direction, the process will start shutting down starting on the
 * right-most slot. Once the left-most slot has shutdown in the write direction, 'callbacks->shutdown_completed' will be
 * invoked in the event loop's thread.
 *
 * This function can be called from any thread.
 */
AWS_IO_API
int aws_channel_shutdown(struct aws_channel *channel, int error_code);

/**
 * Prevent a channel's memory from being freed.
 * Any number of users may acquire a hold to prevent a channel and its handlers from being unexpectedly freed.
 * Any user which acquires a hold must release it via aws_channel_release_hold().
 * Memory will be freed once all holds are released and aws_channel_destroy() has been called.
 */
AWS_IO_API
void aws_channel_acquire_hold(struct aws_channel *channel);

/**
 * Release a hold on the channel's memory, allowing it to be freed.
 * This may be called before or after aws_channel_destroy().
 */
AWS_IO_API
void aws_channel_release_hold(struct aws_channel *channel);

/**
 * Allocates and initializes a new slot for use with the channel. If this is the first slot in the channel, it will
 * automatically be added to the channel as the first slot. For all subsequent calls on a given channel, the slot will
 * need to be added to the channel via. the aws_channel_slot_insert_right(), aws_channel_slot_insert_end(), and
 * aws_channel_slot_insert_left() APIs.
 */
AWS_IO_API
struct aws_channel_slot *aws_channel_slot_new(struct aws_channel *channel);

/**
 * Fetches the event loop the channel is a part of.
 */
AWS_IO_API
struct aws_event_loop *aws_channel_get_event_loop(struct aws_channel *channel);

/**
 * Fetches the current timestamp from the event-loop's clock, in nanoseconds.
 */
AWS_IO_API
int aws_channel_current_clock_time(struct aws_channel *channel, uint64_t *time_nanos);

/**
 * Retrieves an object by key from the event loop's local storage.
 */
AWS_IO_API
int aws_channel_fetch_local_object(
    struct aws_channel *channel,
    const void *key,
    struct aws_event_loop_local_object *obj);

/**
 * Stores an object by key in the event loop's local storage.
 */
AWS_IO_API
int aws_channel_put_local_object(
    struct aws_channel *channel,
    const void *key,
    const struct aws_event_loop_local_object *obj);

/**
 * Removes an object by key from the event loop's local storage.
 */
AWS_IO_API
int aws_channel_remove_local_object(
    struct aws_channel *channel,
    const void *key,
    struct aws_event_loop_local_object *removed_obj);

/**
 * Acquires a message from the event loop's message pool. size_hint is merely a hint, it may be smaller than you
 * requested and you are responsible for checking the bounds of it. If the returned message is not large enough, you
 * must send multiple messages. This cannot fail, it never returns NULL.
 */
AWS_IO_API
struct aws_io_message *aws_channel_acquire_message_from_pool(
    struct aws_channel *channel,
    enum aws_io_message_type message_type,
    size_t size_hint);

/**
 * Schedules a task to run on the event loop as soon as possible.
 * This is the ideal way to move a task into the correct thread. It's also handy for context switches.
 * This function is safe to call from any thread.
 *
 * If called from the channel's event loop, the task will get directly added to the run-now list.
 * If called from outside the channel's event loop, the task will go into a cross-thread task queue.
 *
 * If tasks must be serialized relative to some source synchronization, you may not want to use this API
 * because tasks submitted from the event loop thread can "jump ahead" of tasks submitted from external threads
 * due to this optimization.  If this is a problem, you can either refactor your submission logic or use
 * the aws_channel_schedule_task_now_serialized variant which does not perform this optimization.
 *
 * The task should not be cleaned up or modified until its function is executed.
 */
AWS_IO_API
void aws_channel_schedule_task_now(struct aws_channel *channel, struct aws_channel_task *task);

/**
 * Schedules a task to run on the event loop as soon as possible.
 *
 * This variant always uses the cross thread queue rather than conditionally skipping it when already in
 * the destination event loop.  While not "optimal", this allows us to serialize task execution no matter where
 * the task was submitted from: if you are submitting tasks from a critical section, the serialized order that you
 * submit is guaranteed to be the order that they execute on the event loop.
 *
 * The task should not be cleaned up or modified until its function is executed.
 */
AWS_IO_API
void aws_channel_schedule_task_now_serialized(struct aws_channel *channel, struct aws_channel_task *task);

/**
 * Schedules a task to run on the event loop at the specified time.
 * This is the ideal way to move a task into the correct thread. It's also handy for context switches.
 * Use aws_channel_current_clock_time() to get the current time in nanoseconds.
 * This function is safe to call from any thread.
 *
 * The task should not be cleaned up or modified until its function is executed.
 */
AWS_IO_API
void aws_channel_schedule_task_future(
    struct aws_channel *channel,
    struct aws_channel_task *task,
    uint64_t run_at_nanos);

/**
 * Instrument a channel with a statistics handler.  While instrumented with a statistics handler, the channel
 * will periodically report per-channel-handler-specific statistics about handler performance and state.
 *
 * Assigning a statistics handler to a channel is a transfer of ownership -- the channel will clean up
 * the handler appropriately.  Statistics handlers may be changed dynamically (for example, the upgrade
 * from a vanilla http channel to a websocket channel), but this function may only be called from the
 * event loop thread that the channel is a part of.
 *
 * The first possible hook to set a statistics handler is the channel's creation callback.
 */
AWS_IO_API
int aws_channel_set_statistics_handler(struct aws_channel *channel, struct aws_crt_statistics_handler *handler);

/**
 * Returns true if the caller is on the event loop's thread. If false, you likely need to use
 * aws_channel_schedule_task(). This function is safe to call from any thread.
 */
AWS_IO_API
bool aws_channel_thread_is_callers_thread(struct aws_channel *channel);

/**
 * Sets the handler for a slot, the slot will also call get_current_window_size() and propagate a window update
 * upstream.
 */
AWS_IO_API
int aws_channel_slot_set_handler(struct aws_channel_slot *slot, struct aws_channel_handler *handler);

/**
 * Removes slot from the channel and deallocates the slot and its handler.
 */
AWS_IO_API
int aws_channel_slot_remove(struct aws_channel_slot *slot);

/**
 * Replaces remove with new_slot. Deallocates remove and its handler.
 */
AWS_IO_API
int aws_channel_slot_replace(struct aws_channel_slot *remove, struct aws_channel_slot *new_slot);

/**
 * inserts 'to_add' to the position immediately to the right of slot. Note that the first call to
 * aws_channel_slot_new() adds it to the channel implicitly.
 */
AWS_IO_API
int aws_channel_slot_insert_right(struct aws_channel_slot *slot, struct aws_channel_slot *to_add);

/**
 * Inserts to 'to_add' the end of the channel. Note that the first call to
 * aws_channel_slot_new() adds it to the channel implicitly.
 */
AWS_IO_API
int aws_channel_slot_insert_end(struct aws_channel *channel, struct aws_channel_slot *to_add);

/**
 * inserts 'to_add' to the position immediately to the left of slot. Note that the first call to
 * aws_channel_slot_new() adds it to the channel implicitly.
 */
AWS_IO_API
int aws_channel_slot_insert_left(struct aws_channel_slot *slot, struct aws_channel_slot *to_add);

/**
 * Sends a message to the adjacent slot in the channel based on dir. Also does window size checking.
 *
 * NOTE: if this function returns an error code, it is the caller's responsibility to release message
 * back to the pool. If this function returns AWS_OP_SUCCESS, the recipient of the message has taken
 * ownership of the message. So, for example, don't release a message to the pool and then return an error.
 * If you encounter an error condition in this case, shutdown the channel with the appropriate error code.
 */
AWS_IO_API
int aws_channel_slot_send_message(
    struct aws_channel_slot *slot,
    struct aws_io_message *message,
    enum aws_channel_direction dir);

/**
 * Convenience function that invokes aws_channel_acquire_message_from_pool(),
 * asking for the largest reasonable DATA message that can be sent in the write direction,
 * with upstream overhead accounted for. This cannot fail, it never returns NULL.
 */
AWS_IO_API
struct aws_io_message *aws_channel_slot_acquire_max_message_for_write(struct aws_channel_slot *slot);

/**
 * Issues a window update notification upstream (to the left.)
 */
AWS_IO_API
int aws_channel_slot_increment_read_window(struct aws_channel_slot *slot, size_t window);

/**
 * Called by handlers once they have finished their shutdown in the 'dir' direction. Propagates the shutdown process
 * to the next handler in the channel.
 */
AWS_IO_API
int aws_channel_slot_on_handler_shutdown_complete(
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int err_code,
    bool free_scarce_resources_immediately);

/**
 * Initiates shutdown on slot. callbacks->on_shutdown_completed will be called
 * once the shutdown process is completed.
 */
AWS_IO_API
int aws_channel_slot_shutdown(
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int err_code,
    bool free_scarce_resources_immediately);

/**
 * Fetches the downstream read window. This gives you the information necessary to honor the read window. If you call
 * send_message() and it exceeds this window, the message will be rejected.
 */
AWS_IO_API
size_t aws_channel_slot_downstream_read_window(struct aws_channel_slot *slot);

/** Fetches the current overhead of upstream handlers. This provides a hint to avoid fragmentation if you care. */
AWS_IO_API
size_t aws_channel_slot_upstream_message_overhead(struct aws_channel_slot *slot);

/**
 * Calls destroy on handler's vtable
 */
AWS_IO_API
void aws_channel_handler_destroy(struct aws_channel_handler *handler);

/**
 * Calls process_read_message on handler's vtable
 */
AWS_IO_API
int aws_channel_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message);

/**
 * Calls process_write_message on handler's vtable.
 */
AWS_IO_API
int aws_channel_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message);

/**
 * Calls on_window_update on handler's vtable.
 */
AWS_IO_API
int aws_channel_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size);

/**
 * calls shutdown_direction on handler's vtable.
 */
AWS_IO_API
int aws_channel_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately);

/**
 * Calls initial_window_size on handler's vtable.
 */
AWS_IO_API
size_t aws_channel_handler_initial_window_size(struct aws_channel_handler *handler);

AWS_IO_API
struct aws_channel_slot *aws_channel_get_first_slot(struct aws_channel *channel);

/**
 * A way for external processes to force a read by the data-source channel handler.  Necessary in certain cases, like
 * when a server channel finishes setting up its initial handlers, a read may have already been triggered on the
 * socket (the client's CLIENT_HELLO tls payload, for example) and absent further data/notifications, this data
 * would never get processed.
 */
AWS_IO_API
int aws_channel_trigger_read(struct aws_channel *channel);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_CHANNEL_H */
