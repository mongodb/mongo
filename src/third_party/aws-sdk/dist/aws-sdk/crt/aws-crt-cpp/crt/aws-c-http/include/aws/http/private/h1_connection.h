#ifndef AWS_HTTP_H1_CONNECTION_H
#define AWS_HTTP_H1_CONNECTION_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/mutex.h>
#include <aws/http/private/connection_impl.h>
#include <aws/http/private/h1_encoder.h>
#include <aws/http/statistics.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4214) /* nonstandard extension used: bit field types other than int */
#endif

enum aws_h1_connection_read_state {
    AWS_CONNECTION_READ_OPEN,
    AWS_CONNECTION_READ_SHUTTING_DOWN,
    AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE,
};

struct aws_h1_connection {
    struct aws_http_connection base;

    size_t initial_stream_window_size;

    /* Task responsible for sending data.
     * As long as there is data available to send, the task will be "active" and repeatedly:
     * 1) Encode outgoing stream data to an aws_io_message and send it up the channel.
     * 2) Wait until the aws_io_message's write_complete callback fires.
     * 3) Reschedule the task to run again.
     *
     * `thread_data.is_outgoing_stream_task_active` tells whether the task is "active".
     *
     * If there is no data available to write (waiting for user to add more streams or chunks),
     * then the task stops being active. The task is made active again when the user
     * adds more outgoing data. */
    struct aws_channel_task outgoing_stream_task;

    /* Task that removes items from `synced_data` and does their on-thread work.
     * Runs once and wait until it's scheduled again.
     * Any function that wants to schedule this task MUST:
     * - acquire the synced_data.lock
     * - check whether `synced_data.is_cross_thread_work_scheduled` was true or false.
     * - set `synced_data.is_cross_thread_work_scheduled = true`
     * - release synced_data.lock
     * - ONLY IF `synced_data.is_cross_thread_work_scheduled` CHANGED from false to true:
     *   - then schedule the task
     */
    struct aws_channel_task cross_thread_work_task;

    /* Only the event-loop thread may touch this data */
    struct {
        /* List of streams being worked on. */
        struct aws_linked_list stream_list;

        /* Points to the stream whose data is currently being sent.
         * This stream is ALWAYS in the `stream_list`.
         * HTTP pipelining is supported, so once the stream is completely written
         * we'll start working on the next stream in the list */
        struct aws_h1_stream *outgoing_stream;

        /* Points to the stream being decoded.
         * This stream is ALWAYS in the `stream_list`. */
        struct aws_h1_stream *incoming_stream;
        struct aws_h1_decoder *incoming_stream_decoder;

        /* Used to encode requests and responses */
        struct aws_h1_encoder encoder;

        /**
         * All aws_io_messages arriving in the read direction are queued here before processing.
         * This allows the connection to receive more data than the the current HTTP-stream might allow,
         * and process the data later when HTTP-stream's window opens or the next stream begins.
         *
         * The `aws_io_message.copy_mark` is used to track progress on partially processed messages.
         * `pending_bytes` is the sum of all unprocessed bytes across all queued messages.
         * `capacity` is the limit for how many unprocessed bytes we'd like in the queue.
         */
        struct {
            struct aws_linked_list messages;
            size_t pending_bytes;
            size_t capacity;
        } read_buffer;

        /**
         * The connection's current window size.
         * We use this variable, instead of the existing `aws_channel_slot.window_size`,
         * because that variable is not updated immediately, the channel uses a task to update it.
         * Since we use the difference between current and desired window size when deciding
         * how much to increment, we need the most up-to-date values possible.
         */
        size_t connection_window;

        /* Only used by tests. Sum of window_increments issued by this slot. Resets each time it's queried */
        size_t recent_window_increments;

        struct aws_crt_statistics_http1_channel stats;

        uint64_t outgoing_stream_timestamp_ns;
        uint64_t incoming_stream_timestamp_ns;

        int pending_shutdown_error_code;
        enum aws_h1_connection_read_state read_state;

        /* True when read and/or writing has stopped, whether due to errors or normal channel shutdown. */
        bool is_writing_stopped : 1;

        /* If true, the connection has upgraded to another protocol.
         * It will pass data to adjacent channel handlers without altering it.
         * The connection can no longer service request/response streams. */
        bool has_switched_protocols : 1;

        /* Server-only. Request-handler streams can only be created while this is true. */
        bool can_create_request_handler_stream : 1;

        /* see `outgoing_stream_task` */
        bool is_outgoing_stream_task_active : 1;

        bool is_processing_read_messages : 1;
    } thread_data;

    /* Any thread may touch this data, but the lock must be held */
    struct {
        struct aws_mutex lock;

        /* New client streams that have not been moved to `stream_list` yet.
         * This list is not used on servers. */
        struct aws_linked_list new_client_stream_list;

        /* If non-zero, then window_update_task is scheduled */
        size_t window_update_size;

        /* If non-zero, reason to immediately reject new streams. (ex: closing) */
        int new_stream_error_code;

        /* If true, user has called connection_close() or stream_cancel(),
         * but the cross_thread_work_task hasn't processed it yet */
        bool shutdown_requested;
        int shutdown_requested_error_code;

        /* See `cross_thread_work_task` */
        bool is_cross_thread_work_task_scheduled : 1;

        /* For checking status from outside the event-loop thread. */
        bool is_open : 1;
    } synced_data;
};

/* Allow tests to check current window stats */
struct aws_h1_window_stats {
    size_t connection_window;
    size_t recent_window_increments; /* Resets to 0 each time window stats are queried*/
    size_t buffer_capacity;
    size_t buffer_pending_bytes;
    uint64_t stream_window;
    bool has_incoming_stream;
};

AWS_EXTERN_C_BEGIN

/* The functions below are exported so they can be accessed from tests. */

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http1_1_server(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http1_connection_options *http1_options);

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http1_1_client(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http1_connection_options *http1_options);

/* Allow tests to check current window stats */
AWS_HTTP_API
struct aws_h1_window_stats aws_h1_connection_window_stats(struct aws_http_connection *connection_base);

AWS_EXTERN_C_END

/* DO NOT export functions below. They're only used by other .c files in this library */

/* TODO: introduce naming conventions for private header functions */

void aws_h1_connection_lock_synced_data(struct aws_h1_connection *connection);
void aws_h1_connection_unlock_synced_data(struct aws_h1_connection *connection);

/**
 * Try to kick off the outgoing-stream-task.
 * If task is already active, nothing happens.
 * If there's nothing to do, the task will immediately stop itself.
 * Call this whenever the user provides new outgoing data (ex: new stream, new chunk).
 * MUST be called from the connection's event-loop thread.
 */
void aws_h1_connection_try_write_outgoing_stream(struct aws_h1_connection *connection);

/**
 * If any read messages are queued, and the downstream window is non-zero,
 * process data and send it downstream. Then calculate the connection's
 * desired window size and increment it if necessary.
 *
 * During normal operations "downstream" means the current incoming stream.
 * If the connection has switched protocols "downstream" means the next
 * channel handler in the read direction.
 */
void aws_h1_connection_try_process_read_messages(struct aws_h1_connection *connection);

#endif /* AWS_HTTP_H1_CONNECTION_H */
