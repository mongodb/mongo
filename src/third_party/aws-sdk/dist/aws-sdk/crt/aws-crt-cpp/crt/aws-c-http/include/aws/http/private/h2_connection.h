#ifndef AWS_HTTP_H2_CONNECTION_H
#define AWS_HTTP_H2_CONNECTION_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>
#include <aws/common/fifo_cache.h>
#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>

#include <aws/http/private/connection_impl.h>
#include <aws/http/private/h2_frames.h>
#include <aws/http/statistics.h>

struct aws_h2_decoder;
struct aws_h2_stream;

struct aws_h2_connection {
    struct aws_http_connection base;

    aws_http2_on_goaway_received_fn *on_goaway_received;
    aws_http2_on_remote_settings_change_fn *on_remote_settings_change;

    struct aws_channel_task cross_thread_work_task;
    struct aws_channel_task outgoing_frames_task;

    bool conn_manual_window_management;

    /* Only the event-loop thread may touch this data */
    struct {
        struct aws_h2_decoder *decoder;
        struct aws_h2_frame_encoder encoder;

        /* True when reading/writing has stopped, whether due to errors or normal channel shutdown. */
        bool is_reading_stopped;
        bool is_writing_stopped;

        bool is_outgoing_frames_task_active;

        /* Settings received from peer, which restricts the message to send */
        uint32_t settings_peer[AWS_HTTP2_SETTINGS_END_RANGE];
        /* Local settings to send/sent to peer, which affects the decoding */
        uint32_t settings_self[AWS_HTTP2_SETTINGS_END_RANGE];

        /* List using aws_h2_pending_settings.node
         * Contains settings waiting to be ACKed by peer and applied */
        struct aws_linked_list pending_settings_queue;

        /* List using aws_h2_pending_ping.node
         * Pings waiting to be ACKed by peer */
        struct aws_linked_list pending_ping_queue;

        /* Most recent stream-id that was initiated by peer */
        uint32_t latest_peer_initiated_stream_id;

        /* Maps stream-id to aws_h2_stream*.
         * Contains all streams in the open, reserved, and half-closed states (terms from RFC-7540 5.1).
         * Once a stream enters closed state, it is removed from this map. */
        struct aws_hash_table active_streams_map;

        /* List using aws_h2_stream.node.
         * Contains all streams with DATA frames to send.
         * Any stream in this list is also in the active_streams_map. */
        struct aws_linked_list outgoing_streams_list;

        /* List using aws_h2_stream.node.
         * Contains all streams with DATA frames to send, and cannot send now due to flow control.
         * Waiting for WINDOW_UPDATE to set them free */
        struct aws_linked_list stalled_window_streams_list;

        /* List using aws_h2_stream.node.
         * Contains all streams that are open, but are only sending data when notified, rather than polling
         * for it (e.g. event streams)
         * Streams are moved to the outgoing_streams_list until they send pending data, then are moved back
         * to this list to sleep until more data comes in
         */
        struct aws_linked_list waiting_streams_list;

        /* List using aws_h2_frame.node.
         * Queues all frames (except DATA frames) for connection to send.
         * When queue is empty, then we send DATA frames from the outgoing_streams_list */
        struct aws_linked_list outgoing_frames_queue;

        /* FIFO cache for closed stream, key: stream-id, value: aws_h2_stream_closed_when.
         * Contains data about streams that were recently closed.
         * The oldest entry will be removed if the cache is full */
        struct aws_cache *closed_streams;

        /* Flow-control of connection from peer. Indicating the buffer capacity of our peer.
         * Reduce the space after sending a flow-controlled frame. Increment after receiving WINDOW_UPDATE for
         * connection */
        size_t window_size_peer;

        /* Flow-control of connection for this side.
         * Reduce the space after receiving a flow-controlled frame. Increment after sending WINDOW_UPDATE for
         * connection */
        size_t window_size_self;

        /* Highest self-initiated stream-id that peer might have processed.
         * Defaults to max stream-id, may be lowered when GOAWAY frame received. */
        uint32_t goaway_received_last_stream_id;

        /* Last-stream-id sent in most recent GOAWAY frame. Defaults to max stream-id. */
        uint32_t goaway_sent_last_stream_id;

        /* Frame we are encoding now. NULL if we are not encoding anything. */
        struct aws_h2_frame *current_outgoing_frame;

        /* Pointer to initial pending settings. If ACKed by peer, it will be NULL. */
        struct aws_h2_pending_settings *init_pending_settings;

        /* Cached channel shutdown values.
         * If possible, we delay shutdown-in-the-write-dir until GOAWAY is written. */
        int channel_shutdown_error_code;
        bool channel_shutdown_immediately;
        bool channel_shutdown_waiting_for_goaway_to_be_written;

        /* TODO: Consider adding stream monitor */
        struct aws_crt_statistics_http2_channel stats;

        /* Timestamp when connection has data to send, which is when there is an active stream with body to send */
        uint64_t outgoing_timestamp_ns;
        /* Timestamp when connection has data to receive, which is when there is an active stream */
        uint64_t incoming_timestamp_ns;

    } thread_data;

    /* Any thread may touch this data, but the lock must be held (unless it's an atomic) */
    struct {
        struct aws_mutex lock;

        /* New `aws_h2_stream *` that haven't moved to `thread_data` yet */
        struct aws_linked_list pending_stream_list;

        /* New `aws_h2_frames *`, connection control frames created by user that haven't moved to `thread_data` yet */
        struct aws_linked_list pending_frame_list;

        /* New `aws_h2_pending_settings *` created by user that haven't moved to `thread_data` yet */
        struct aws_linked_list pending_settings_list;

        /* New `aws_h2_pending_ping *` created by user that haven't moved to `thread_data` yet */
        struct aws_linked_list pending_ping_list;

        /* New `aws_h2_pending_goaway *` created by user that haven't sent yet */
        struct aws_linked_list pending_goaway_list;

        bool is_cross_thread_work_task_scheduled;

        /* The window_update value for `thread_data.window_size_self` that haven't applied yet */
        size_t window_update_size;

        /* For checking status from outside the event-loop thread. */
        bool is_open;

        /* If non-zero, reason to immediately reject new streams. (ex: closing) */
        int new_stream_error_code;

        /* Last-stream-id sent in most recent GOAWAY frame. Defaults to AWS_H2_STREAM_ID_MAX + 1 indicates no GOAWAY has
         * been sent so far.*/
        uint32_t goaway_sent_last_stream_id;
        /* aws_http2_error_code sent in most recent GOAWAY frame. Defaults to 0, check goaway_sent_last_stream_id for
         * any GOAWAY has sent or not */
        uint32_t goaway_sent_http2_error_code;

        /* Last-stream-id received in most recent GOAWAY frame. Defaults to AWS_H2_STREAM_ID_MAX + 1 indicates no GOAWAY
         * has been received so far.*/
        uint32_t goaway_received_last_stream_id;
        /* aws_http2_error_code received in most recent GOAWAY frame. Defaults to 0, check
         * goaway_received_last_stream_id for any GOAWAY has received or not */
        uint32_t goaway_received_http2_error_code;

        /* For checking settings received from peer from outside the event-loop thread. */
        uint32_t settings_peer[AWS_HTTP2_SETTINGS_END_RANGE];
        /* For checking local settings to send/sent to peer from outside the event-loop thread. */
        uint32_t settings_self[AWS_HTTP2_SETTINGS_END_RANGE];
    } synced_data;
};

struct aws_h2_pending_settings {
    struct aws_http2_setting *settings_array;
    size_t num_settings;
    struct aws_linked_list_node node;
    /* user callback */
    void *user_data;
    aws_http2_on_change_settings_complete_fn *on_completed;
};

struct aws_h2_pending_ping {
    uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE];
    /* For calculating round-trip time */
    uint64_t started_time;
    struct aws_linked_list_node node;
    /* user callback */
    void *user_data;
    aws_http2_on_ping_complete_fn *on_completed;
};

struct aws_h2_pending_goaway {
    bool allow_more_streams;
    uint32_t http2_error;
    struct aws_byte_cursor debug_data;
    struct aws_linked_list_node node;
};

/**
 * The action which caused the stream to close.
 */
enum aws_h2_stream_closed_when {
    AWS_H2_STREAM_CLOSED_UNKNOWN,
    AWS_H2_STREAM_CLOSED_WHEN_BOTH_SIDES_END_STREAM,
    AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_RECEIVED,
    AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_SENT,
};

enum aws_h2_data_encode_status {
    AWS_H2_DATA_ENCODE_COMPLETE,
    AWS_H2_DATA_ENCODE_ONGOING,
    AWS_H2_DATA_ENCODE_ONGOING_BODY_STREAM_STALLED, /* stalled reading from body stream */
    AWS_H2_DATA_ENCODE_ONGOING_WAITING_FOR_WRITES,  /* waiting for next manual write */
    AWS_H2_DATA_ENCODE_ONGOING_WINDOW_STALLED,      /* stalled due to reduced window size */
};

/* When window size is too small to fit the possible padding into it, we stop sending data and wait for WINDOW_UPDATE */
#define AWS_H2_MIN_WINDOW_SIZE (256)

/* Private functions called from tests... */

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http2_server(
    struct aws_allocator *allocator,
    bool manual_window_management,
    const struct aws_http2_connection_options *http2_options);

AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_http2_client(
    struct aws_allocator *allocator,
    bool manual_window_management,
    const struct aws_http2_connection_options *http2_options);

AWS_EXTERN_C_END

/* Private functions called from multiple .c files... */

/**
 * Enqueue outgoing frame.
 * Connection takes ownership of frame.
 * Frames are sent into FIFO order.
 * Do not enqueue DATA frames, these are sent by other means when the frame queue is empty.
 */
void aws_h2_connection_enqueue_outgoing_frame(struct aws_h2_connection *connection, struct aws_h2_frame *frame);

/**
 * Invoked immediately after a stream enters the CLOSED state.
 * The connection will remove the stream from its "active" datastructures,
 * guaranteeing that no further decoder callbacks are invoked on the stream.
 *
 * This should NOT be invoked in the case of a "Connection Error",
 * though a "Stream Error", in which a RST_STREAM is sent and the stream
 * is closed early, would invoke this.
 */
int aws_h2_connection_on_stream_closed(
    struct aws_h2_connection *connection,
    struct aws_h2_stream *stream,
    enum aws_h2_stream_closed_when closed_when,
    int aws_error_code);

/**
 * Send RST_STREAM and close a stream reserved via PUSH_PROMISE.
 */
int aws_h2_connection_send_rst_and_close_reserved_stream(
    struct aws_h2_connection *connection,
    uint32_t stream_id,
    uint32_t h2_error_code);

/**
 * Error happens while writing into channel, shutdown the connection. Only called within the eventloop thread
 */
void aws_h2_connection_shutdown_due_to_write_err(struct aws_h2_connection *connection, int error_code);

/**
 * Try to write outgoing frames, if the outgoing-frames-task isn't scheduled, run it immediately.
 */
void aws_h2_try_write_outgoing_frames(struct aws_h2_connection *connection);

#endif /* AWS_HTTP_H2_CONNECTION_H */
