/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/h2_stream.h>

#include <aws/common/clock.h>
#include <aws/http/private/h2_connection.h>
#include <aws/http/private/strutil.h>
#include <aws/http/status_code.h>
#include <aws/io/channel.h>
#include <aws/io/logging.h>
#include <aws/io/stream.h>

/* Apple toolchains such as xcode and swiftpm define the DEBUG symbol. undef it here so we can actually use the token */
#undef DEBUG

static void s_stream_destroy(struct aws_http_stream *stream_base);
static void s_stream_update_window(struct aws_http_stream *stream_base, size_t increment_size);
static int s_stream_reset_stream(struct aws_http_stream *stream_base, uint32_t http2_error);
static int s_stream_get_received_error_code(struct aws_http_stream *stream_base, uint32_t *out_http2_error);
static int s_stream_get_sent_error_code(struct aws_http_stream *stream_base, uint32_t *out_http2_error);
static int s_stream_write_data(
    struct aws_http_stream *stream_base,
    const struct aws_http2_stream_write_data_options *options);

static void s_stream_cross_thread_work_task(struct aws_channel_task *task, void *arg, enum aws_task_status status);
static struct aws_h2err s_send_rst_and_close_stream(struct aws_h2_stream *stream, struct aws_h2err stream_error);
static int s_stream_reset_stream_internal(
    struct aws_http_stream *stream_base,
    struct aws_h2err stream_error,
    bool cancelling);
static void s_stream_cancel(struct aws_http_stream *stream, int error_code);

struct aws_http_stream_vtable s_h2_stream_vtable = {
    .destroy = s_stream_destroy,
    .update_window = s_stream_update_window,
    .activate = aws_h2_stream_activate,
    .cancel = s_stream_cancel,
    .http1_write_chunk = NULL,
    .http2_reset_stream = s_stream_reset_stream,
    .http2_get_received_error_code = s_stream_get_received_error_code,
    .http2_get_sent_error_code = s_stream_get_sent_error_code,
    .http2_write_data = s_stream_write_data,
};

const char *aws_h2_stream_state_to_str(enum aws_h2_stream_state state) {
    switch (state) {
        case AWS_H2_STREAM_STATE_IDLE:
            return "IDLE";
        case AWS_H2_STREAM_STATE_RESERVED_LOCAL:
            return "RESERVED_LOCAL";
        case AWS_H2_STREAM_STATE_RESERVED_REMOTE:
            return "RESERVED_REMOTE";
        case AWS_H2_STREAM_STATE_OPEN:
            return "OPEN";
        case AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL:
            return "HALF_CLOSED_LOCAL";
        case AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE:
            return "HALF_CLOSED_REMOTE";
        case AWS_H2_STREAM_STATE_CLOSED:
            return "CLOSED";
        default:
            /* unreachable */
            AWS_ASSERT(0);
            return "*** UNKNOWN ***";
    }
}

static struct aws_h2_connection *s_get_h2_connection(const struct aws_h2_stream *stream) {
    return AWS_CONTAINER_OF(stream->base.owning_connection, struct aws_h2_connection, base);
}

static void s_lock_synced_data(struct aws_h2_stream *stream) {
    int err = aws_mutex_lock(&stream->synced_data.lock);
    AWS_ASSERT(!err && "lock failed");
    (void)err;
}

static void s_unlock_synced_data(struct aws_h2_stream *stream) {
    int err = aws_mutex_unlock(&stream->synced_data.lock);
    AWS_ASSERT(!err && "unlock failed");
    (void)err;
}

#define AWS_PRECONDITION_ON_CHANNEL_THREAD(STREAM)                                                                     \
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(s_get_h2_connection(STREAM)->base.channel_slot->channel))

static bool s_client_state_allows_frame_type[AWS_H2_STREAM_STATE_COUNT][AWS_H2_FRAME_TYPE_COUNT] = {
    /* State before anything is sent or received */
    [AWS_H2_STREAM_STATE_IDLE] = {0},
    /* Client streams are never in reserved (local) state */
    [AWS_H2_STREAM_STATE_RESERVED_LOCAL] = {0},
    /* Client received push-request via PUSH_PROMISE on another stream.
     * Waiting for push-response to start arriving on this server-initiated stream. */
    [AWS_H2_STREAM_STATE_RESERVED_REMOTE] =
        {
            [AWS_H2_FRAME_T_HEADERS] = true,
            [AWS_H2_FRAME_T_RST_STREAM] = true,
        },
    /* Client is sending request and has not received full response yet. */
    [AWS_H2_STREAM_STATE_OPEN] =
        {
            [AWS_H2_FRAME_T_DATA] = true,
            [AWS_H2_FRAME_T_HEADERS] = true,
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_PUSH_PROMISE] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Client has sent full request (END_STREAM), but has not received full response yet. */
    [AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL] =
        {
            [AWS_H2_FRAME_T_DATA] = true,
            [AWS_H2_FRAME_T_HEADERS] = true,
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_PUSH_PROMISE] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Client has received full response (END_STREAM), but is still sending request (uncommon). */
    [AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE] =
        {
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Full request sent (END_STREAM) and full response received (END_STREAM).
     * OR sent RST_STREAM. OR received RST_STREAM. */
    [AWS_H2_STREAM_STATE_CLOSED] = {0},
};

static bool s_server_state_allows_frame_type[AWS_H2_STREAM_STATE_COUNT][AWS_H2_FRAME_TYPE_COUNT] = {
    /* State before anything is sent or received, waiting for request headers to arrives and start things off */
    [AWS_H2_STREAM_STATE_IDLE] =
        {
            [AWS_H2_FRAME_T_HEADERS] = true,
        },
    /* Server sent push-request via PUSH_PROMISE on a client-initiated stream,
     * but hasn't started sending the push-response on this server-initiated stream yet. */
    [AWS_H2_STREAM_STATE_RESERVED_LOCAL] =
        {
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Server streams are never in reserved (remote) state */
    [AWS_H2_STREAM_STATE_RESERVED_REMOTE] = {0},
    /* Server is receiving request, and has sent full response yet. */
    [AWS_H2_STREAM_STATE_OPEN] =
        {
            [AWS_H2_FRAME_T_HEADERS] = true,
            [AWS_H2_FRAME_T_DATA] = true,
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Server has sent full response (END_STREAM), but has not received full response yet (uncommon). */
    [AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL] =
        {
            [AWS_H2_FRAME_T_HEADERS] = true,
            [AWS_H2_FRAME_T_DATA] = true,
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Server has received full request (END_STREAM), and is still sending response. */
    [AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE] =
        {
            [AWS_H2_FRAME_T_RST_STREAM] = true,
            [AWS_H2_FRAME_T_WINDOW_UPDATE] = true,
        },
    /* Full request received (END_STREAM) and full response sent (END_STREAM).
     * OR sent RST_STREAM. OR received RST_STREAM. */
    [AWS_H2_STREAM_STATE_CLOSED] = {0},
};

/* Returns the appropriate Stream Error if given frame not allowed in current state */
static struct aws_h2err s_check_state_allows_frame_type(
    const struct aws_h2_stream *stream,
    enum aws_h2_frame_type frame_type) {

    AWS_PRECONDITION(frame_type < AWS_H2_FRAME_T_UNKNOWN); /* Decoder won't invoke callbacks for unknown frame types */
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    const enum aws_h2_stream_state state = stream->thread_data.state;

    bool allowed;
    if (stream->base.server_data) {
        allowed = s_server_state_allows_frame_type[state][frame_type];
    } else {
        allowed = s_client_state_allows_frame_type[state][frame_type];
    }

    if (allowed) {
        return AWS_H2ERR_SUCCESS;
    }

    /* Determine specific error code */
    enum aws_http2_error_code h2_error_code = AWS_HTTP2_ERR_PROTOCOL_ERROR;

    /* If peer knows the state is closed, then it's a STREAM_CLOSED error */
    if (state == AWS_H2_STREAM_STATE_CLOSED || state == AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE) {
        h2_error_code = AWS_HTTP2_ERR_STREAM_CLOSED;
    }

    AWS_H2_STREAM_LOGF(
        ERROR,
        stream,
        "Malformed message, cannot receive %s frame in %s state",
        aws_h2_frame_type_to_str(frame_type),
        aws_h2_stream_state_to_str(state));

    return aws_h2err_from_h2_code(h2_error_code);
}

static int s_stream_send_update_window_frame(struct aws_h2_stream *stream, size_t increment_size) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);
    AWS_PRECONDITION(increment_size <= AWS_H2_WINDOW_UPDATE_MAX);

    struct aws_h2_connection *connection = s_get_h2_connection(stream);
    struct aws_h2_frame *stream_window_update_frame =
        aws_h2_frame_new_window_update(stream->base.alloc, stream->base.id, (uint32_t)increment_size);

    if (!stream_window_update_frame) {
        AWS_H2_STREAM_LOGF(
            ERROR,
            stream,
            "Failed to create WINDOW_UPDATE frame on connection, error %s",
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }
    aws_h2_connection_enqueue_outgoing_frame(connection, stream_window_update_frame);

    return AWS_OP_SUCCESS;
}

struct aws_h2_stream *aws_h2_stream_new_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options) {
    AWS_PRECONDITION(client_connection);
    AWS_PRECONDITION(options);

    struct aws_h2_stream *stream = aws_mem_calloc(client_connection->alloc, 1, sizeof(struct aws_h2_stream));

    /* Initialize base stream */
    stream->base.vtable = &s_h2_stream_vtable;
    stream->base.alloc = client_connection->alloc;
    stream->base.owning_connection = client_connection;
    stream->base.user_data = options->user_data;
    stream->base.on_incoming_headers = options->on_response_headers;
    stream->base.on_incoming_header_block_done = options->on_response_header_block_done;
    stream->base.on_incoming_body = options->on_response_body;
    stream->base.on_metrics = options->on_metrics;
    stream->base.on_complete = options->on_complete;
    stream->base.on_destroy = options->on_destroy;
    stream->base.client_data = &stream->base.client_or_server_data.client;
    stream->base.client_data->response_status = AWS_HTTP_STATUS_CODE_UNKNOWN;
    stream->base.metrics.send_start_timestamp_ns = -1;
    stream->base.metrics.send_end_timestamp_ns = -1;
    stream->base.metrics.sending_duration_ns = -1;
    stream->base.metrics.receive_start_timestamp_ns = -1;
    stream->base.metrics.receive_end_timestamp_ns = -1;
    stream->base.metrics.receiving_duration_ns = -1;
    aws_linked_list_init(&stream->thread_data.outgoing_writes);
    aws_linked_list_init(&stream->synced_data.pending_write_list);

    /* Stream refcount starts at 1, and gets incremented again for the connection upon a call to activate() */
    aws_atomic_init_int(&stream->base.refcount, 1);

    enum aws_http_version message_version = aws_http_message_get_protocol_version(options->request);
    switch (message_version) {
        case AWS_HTTP_VERSION_1_1:
            /* TODO: don't automatic transform HTTP/1 message. Let user explicitly pass in HTTP/2 request */
            stream->thread_data.outgoing_message =
                aws_http2_message_new_from_http1(stream->base.alloc, options->request);
            if (!stream->thread_data.outgoing_message) {
                AWS_H2_STREAM_LOG(ERROR, stream, "Stream failed to create the HTTP/2 message from HTTP/1.1 message");
                goto error;
            }
            break;
        case AWS_HTTP_VERSION_2:
            stream->thread_data.outgoing_message = options->request;
            aws_http_message_acquire(stream->thread_data.outgoing_message);
            break;
        default:
            /* Not supported */
            aws_raise_error(AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL);
            goto error;
    }
    struct aws_byte_cursor method;
    AWS_ZERO_STRUCT(method);
    if (aws_http_message_get_request_method(options->request, &method)) {
        goto error;
    }
    stream->base.request_method = aws_http_str_to_method(method);

    /* Init H2 specific stuff */
    stream->thread_data.state = AWS_H2_STREAM_STATE_IDLE;
    /* stream end is implicit if the request isn't using manual data writes */
    stream->synced_data.manual_write_ended = !options->http2_use_manual_data_writes;
    stream->manual_write = options->http2_use_manual_data_writes;

    /* if there's a request body to write, add it as the first outgoing write */
    struct aws_input_stream *body_stream = aws_http_message_get_body_stream(options->request);
    if (body_stream) {
        struct aws_h2_stream_data_write *body_write =
            aws_mem_calloc(stream->base.alloc, 1, sizeof(struct aws_h2_stream_data_write));
        body_write->data_stream = aws_input_stream_acquire(body_stream);
        body_write->end_stream = !stream->manual_write;
        aws_linked_list_push_back(&stream->thread_data.outgoing_writes, &body_write->node);
    }

    stream->sent_reset_error_code = -1;
    stream->received_reset_error_code = -1;
    stream->synced_data.reset_error.h2_code = AWS_HTTP2_ERR_COUNT;
    stream->synced_data.api_state = AWS_H2_STREAM_API_STATE_INIT;
    if (aws_mutex_init(&stream->synced_data.lock)) {
        AWS_H2_STREAM_LOGF(
            ERROR, stream, "Mutex init error %d (%s).", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }
    aws_channel_task_init(
        &stream->cross_thread_work_task, s_stream_cross_thread_work_task, stream, "HTTP/2 stream cross-thread work");
    return stream;
error:
    s_stream_destroy(&stream->base);
    return NULL;
}

static void s_stream_cross_thread_work_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_h2_stream *stream = arg;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto end;
    }

    struct aws_h2_connection *connection = s_get_h2_connection(stream);

    if (aws_h2_stream_get_state(stream) == AWS_H2_STREAM_STATE_CLOSED) {
        /* stream is closed, silently ignoring the requests from user */
        AWS_H2_STREAM_LOG(
            TRACE, stream, "Stream closed before cross thread work task runs, ignoring everything was sent by user.");
        goto end;
    }

    /* Not sending window update at half closed remote state */
    bool ignore_window_update = (aws_h2_stream_get_state(stream) == AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE);
    bool reset_called;
    size_t window_update_size;
    struct aws_h2err reset_error;

    struct aws_linked_list pending_writes;
    aws_linked_list_init(&pending_writes);

    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream);
        stream->synced_data.is_cross_thread_work_task_scheduled = false;

        /* window_update_size is ensured to be not greater than AWS_H2_WINDOW_UPDATE_MAX */
        window_update_size = stream->synced_data.window_update_size;
        stream->synced_data.window_update_size = 0;
        reset_called = stream->synced_data.reset_called;
        reset_error = stream->synced_data.reset_error;

        /* copy out pending writes */
        aws_linked_list_swap_contents(&pending_writes, &stream->synced_data.pending_write_list);

        s_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (window_update_size > 0 && !ignore_window_update) {
        if (s_stream_send_update_window_frame(stream, window_update_size)) {
            /* Treat this as a connection error */
            aws_h2_connection_shutdown_due_to_write_err(connection, aws_last_error());
        }
    }

    /* The largest legal value will be 2 * max window size, which is way less than INT64_MAX, so if the window_size_self
     * overflows, remote peer will find it out. So just apply the change and ignore the possible overflow.*/
    stream->thread_data.window_size_self += window_update_size;

    if (reset_called) {
        struct aws_h2err returned_h2err = s_send_rst_and_close_stream(stream, reset_error);
        if (aws_h2err_failed(returned_h2err)) {
            aws_h2_connection_shutdown_due_to_write_err(connection, returned_h2err.aws_code);
        }
    }

    if (stream->thread_data.waiting_for_writes && !aws_linked_list_empty(&pending_writes)) {
        /* Got more to write, move the stream back to outgoing list */
        aws_linked_list_remove(&stream->node);
        aws_linked_list_push_back(&connection->thread_data.outgoing_streams_list, &stream->node);
        stream->thread_data.waiting_for_writes = false;
    }
    /* move any pending writes to the outgoing write queue */
    aws_linked_list_move_all_back(&stream->thread_data.outgoing_writes, &pending_writes);

    /* It's likely that frames were queued while processing cross-thread work.
     * If so, try writing them now */
    aws_h2_try_write_outgoing_frames(connection);

end:
    aws_http_stream_release(&stream->base);
}

static void s_stream_data_write_destroy(
    struct aws_h2_stream *stream,
    struct aws_h2_stream_data_write *write,
    int error_code) {

    AWS_PRECONDITION(stream);
    AWS_PRECONDITION(write);
    if (write->on_complete) {
        write->on_complete(&stream->base, error_code, write->user_data);
    }
    if (write->data_stream) {
        aws_input_stream_release(write->data_stream);
    }
    aws_mem_release(stream->base.alloc, write);
}

static void s_h2_stream_destroy_pending_writes(struct aws_h2_stream *stream) {
    /**
     * Only called when stream is not active and will never be active afterward (destroying).
     * Under this assumption, we can safely touch `stream->synced_data.pending_write_list` without
     * lock, as the user can only add write to the list when the stream is ACTIVE
     */
    AWS_ASSERT(stream->synced_data.api_state != AWS_H2_STREAM_API_STATE_ACTIVE);
    aws_linked_list_move_all_back(
        &stream->thread_data.outgoing_writes,
        &stream->synced_data.pending_write_list); /* clean up any outgoing writes */
    while (!aws_linked_list_empty(&stream->thread_data.outgoing_writes)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&stream->thread_data.outgoing_writes);
        struct aws_h2_stream_data_write *write = AWS_CONTAINER_OF(node, struct aws_h2_stream_data_write, node);
        AWS_LOGF_DEBUG(AWS_LS_HTTP_STREAM, "Stream closing, cancelling write of stream %p", (void *)write->data_stream);
        s_stream_data_write_destroy(stream, write, AWS_ERROR_HTTP_STREAM_HAS_COMPLETED);
    }
}

static void s_stream_destroy(struct aws_http_stream *stream_base) {
    AWS_PRECONDITION(stream_base);
    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);

    s_h2_stream_destroy_pending_writes(stream);

    AWS_H2_STREAM_LOG(DEBUG, stream, "Destroying stream");
    aws_mutex_clean_up(&stream->synced_data.lock);
    aws_http_message_release(stream->thread_data.outgoing_message);

    aws_mem_release(stream->base.alloc, stream);
}

void aws_h2_stream_complete(struct aws_h2_stream *stream, int error_code) {
    { /* BEGIN CRITICAL SECTION */
        /* clean up any pending writes */
        s_lock_synced_data(stream);
        /* The stream is complete now, this will prevent further writes from being queued */
        stream->synced_data.api_state = AWS_H2_STREAM_API_STATE_COMPLETE;
        s_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    s_h2_stream_destroy_pending_writes(stream);

    /* Invoke callback */
    if (stream->base.on_metrics) {
        stream->base.on_metrics(&stream->base, &stream->base.metrics, stream->base.user_data);
    }
    if (stream->base.on_complete) {
        stream->base.on_complete(&stream->base, error_code, stream->base.user_data);
    }
}

static void s_stream_update_window(struct aws_http_stream *stream_base, size_t increment_size) {
    AWS_PRECONDITION(stream_base);
    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);
    struct aws_h2_connection *connection = s_get_h2_connection(stream);
    if (!increment_size) {
        return;
    }
    if (!connection->base.stream_manual_window_management) {
        /* auto-mode, manual update window is not supported */
        AWS_H2_STREAM_LOG(
            DEBUG, stream, "Manual window management is off, update window operations are not supported.");
        return;
    }

    int err = 0;
    bool stream_is_init;
    bool cross_thread_work_should_schedule = false;
    size_t sum_size;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream);

        err |= aws_add_size_checked(stream->synced_data.window_update_size, increment_size, &sum_size);
        err |= sum_size > AWS_H2_WINDOW_UPDATE_MAX;
        stream_is_init = stream->synced_data.api_state == AWS_H2_STREAM_API_STATE_INIT;

        if (!err && !stream_is_init) {
            cross_thread_work_should_schedule = !stream->synced_data.is_cross_thread_work_task_scheduled;
            stream->synced_data.is_cross_thread_work_task_scheduled = true;
            stream->synced_data.window_update_size = sum_size;
        }
        s_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (cross_thread_work_should_schedule) {
        AWS_H2_STREAM_LOG(TRACE, stream, "Scheduling stream cross-thread work task");
        /* increment the refcount of stream to keep it alive until the task runs */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &stream->cross_thread_work_task);
        return;
    }

    if (stream_is_init) {
        AWS_H2_STREAM_LOG(
            ERROR,
            stream,
            "Stream update window failed. Stream is in initialized state, please activate the stream first.");
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        return;
    }

    if (err) {
        /* The increment_size is still not 100% safe, since we cannot control the incoming data frame. So just
         * ruled out the value that is obviously wrong values */
        AWS_H2_STREAM_LOG(
            ERROR,
            stream,
            "The stream's flow-control window has been incremented beyond 2**31 -1, the max for HTTP/2. The stream "
            "will close.");
        aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
        struct aws_h2err stream_error = {
            .aws_code = AWS_ERROR_OVERFLOW_DETECTED,
            .h2_code = AWS_HTTP2_ERR_INTERNAL_ERROR,
        };
        /* Only when stream is not initialized reset will fail. So, we can assert it to be succeed. */
        AWS_FATAL_ASSERT(
            s_stream_reset_stream_internal(stream_base, stream_error, false /*cancelling*/) == AWS_OP_SUCCESS);
    }
    return;
}

static int s_stream_reset_stream_internal(
    struct aws_http_stream *stream_base,
    struct aws_h2err stream_error,
    bool cancelling) {

    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);
    struct aws_h2_connection *connection = s_get_h2_connection(stream);
    bool reset_called;
    bool stream_is_init;
    bool cross_thread_work_should_schedule = false;

    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream);

        reset_called = stream->synced_data.reset_called;
        stream_is_init = stream->synced_data.api_state == AWS_H2_STREAM_API_STATE_INIT;
        if (!reset_called && !stream_is_init) {
            cross_thread_work_should_schedule = !stream->synced_data.is_cross_thread_work_task_scheduled;
            stream->synced_data.reset_called = true;
            stream->synced_data.reset_error = stream_error;
        }
        s_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (stream_is_init) {
        if (cancelling) {
            /* Not an error if we are just cancelling. */
            AWS_LOGF_DEBUG(AWS_LS_HTTP_STREAM, "id=%p: Stream not in process, nothing to cancel.", (void *)stream);
            return AWS_OP_SUCCESS;
        }
        AWS_H2_STREAM_LOG(
            ERROR, stream, "Reset stream failed. Stream is in initialized state, please activate the stream first.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    if (reset_called) {
        AWS_H2_STREAM_LOG(DEBUG, stream, "Reset stream ignored. Reset stream has been called already.");
    }

    if (cross_thread_work_should_schedule) {
        AWS_H2_STREAM_LOG(TRACE, stream, "Scheduling stream cross-thread work task");
        /* increment the refcount of stream to keep it alive until the task runs */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &stream->cross_thread_work_task);
    }
    return AWS_OP_SUCCESS;
}

static int s_stream_reset_stream(struct aws_http_stream *stream_base, uint32_t http2_error) {
    struct aws_h2err stream_error = {
        .aws_code = AWS_ERROR_HTTP_RST_STREAM_SENT,
        .h2_code = http2_error,
    };

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: User requested RST_STREAM with error code %s (0x%x)",
        (void *)stream_base,
        aws_http2_error_code_to_str(http2_error),
        http2_error);
    return s_stream_reset_stream_internal(stream_base, stream_error, false /*cancelling*/);
}

void s_stream_cancel(struct aws_http_stream *stream_base, int error_code) {
    struct aws_h2err stream_error = {
        .aws_code = error_code,
        .h2_code = AWS_HTTP2_ERR_CANCEL,
    };
    s_stream_reset_stream_internal(stream_base, stream_error, true /*cancelling*/);
    return;
}

static int s_stream_get_received_error_code(struct aws_http_stream *stream_base, uint32_t *out_http2_error) {
    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);
    if (stream->received_reset_error_code == -1) {
        return aws_raise_error(AWS_ERROR_HTTP_DATA_NOT_AVAILABLE);
    }
    *out_http2_error = (uint32_t)stream->received_reset_error_code;
    return AWS_OP_SUCCESS;
}

static int s_stream_get_sent_error_code(struct aws_http_stream *stream_base, uint32_t *out_http2_error) {
    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);
    if (stream->sent_reset_error_code == -1) {
        return aws_raise_error(AWS_ERROR_HTTP_DATA_NOT_AVAILABLE);
    }
    *out_http2_error = (uint32_t)stream->sent_reset_error_code;
    return AWS_OP_SUCCESS;
}

enum aws_h2_stream_state aws_h2_stream_get_state(const struct aws_h2_stream *stream) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);
    return stream->thread_data.state;
}

/* Given a Stream Error, send RST_STREAM frame and close stream.
 * A Connection Error is returned if something goes catastrophically wrong */
static struct aws_h2err s_send_rst_and_close_stream(struct aws_h2_stream *stream, struct aws_h2err stream_error) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);
    AWS_PRECONDITION(stream->thread_data.state != AWS_H2_STREAM_STATE_CLOSED);

    struct aws_h2_connection *connection = s_get_h2_connection(stream);

    stream->thread_data.state = AWS_H2_STREAM_STATE_CLOSED;
    AWS_H2_STREAM_LOGF(
        DEBUG,
        stream,
        "Sending RST_STREAM with error code %s (0x%x). State -> CLOSED",
        aws_http2_error_code_to_str(stream_error.h2_code),
        stream_error.h2_code);

    /* Send RST_STREAM */
    struct aws_h2_frame *rst_stream_frame =
        aws_h2_frame_new_rst_stream(stream->base.alloc, stream->base.id, stream_error.h2_code);
    AWS_FATAL_ASSERT(rst_stream_frame != NULL);
    aws_h2_connection_enqueue_outgoing_frame(connection, rst_stream_frame); /* connection takes ownership of frame */
    stream->sent_reset_error_code = stream_error.h2_code;

    /* Tell connection that stream is now closed */
    if (aws_h2_connection_on_stream_closed(
            connection, stream, AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_SENT, stream_error.aws_code)) {
        return aws_h2err_from_last_error();
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_window_size_change(struct aws_h2_stream *stream, int32_t size_changed, bool self) {
    if (self) {
        if (stream->thread_data.window_size_self + size_changed > AWS_H2_WINDOW_UPDATE_MAX) {
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR);
        }
        stream->thread_data.window_size_self += size_changed;
    } else {
        if ((int64_t)stream->thread_data.window_size_peer + size_changed > AWS_H2_WINDOW_UPDATE_MAX) {
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR);
        }
        stream->thread_data.window_size_peer += size_changed;
    }
    return AWS_H2ERR_SUCCESS;
}

static inline bool s_h2_stream_has_outgoing_writes(struct aws_h2_stream *stream) {
    return !aws_linked_list_empty(&stream->thread_data.outgoing_writes);
}

static void s_h2_stream_write_data_complete(struct aws_h2_stream *stream, bool *waiting_writes) {
    AWS_PRECONDITION(waiting_writes);
    AWS_PRECONDITION(s_h2_stream_has_outgoing_writes(stream));

    /* finish/clean up the current write operation */
    struct aws_linked_list_node *node = aws_linked_list_pop_front(&stream->thread_data.outgoing_writes);
    struct aws_h2_stream_data_write *write_op = AWS_CONTAINER_OF(node, struct aws_h2_stream_data_write, node);
    const bool ending_stream = write_op->end_stream;
    s_stream_data_write_destroy(stream, write_op, AWS_OP_SUCCESS);

    /* check to see if there are more queued writes or stream_end was called */
    *waiting_writes = !ending_stream && !s_h2_stream_has_outgoing_writes(stream);
}

static struct aws_h2_stream_data_write *s_h2_stream_get_current_write(struct aws_h2_stream *stream) {
    AWS_PRECONDITION(s_h2_stream_has_outgoing_writes(stream));
    struct aws_linked_list_node *node = aws_linked_list_front(&stream->thread_data.outgoing_writes);
    struct aws_h2_stream_data_write *write = AWS_CONTAINER_OF(node, struct aws_h2_stream_data_write, node);
    return write;
}

static struct aws_input_stream *s_h2_stream_get_data_stream(struct aws_h2_stream *stream) {
    struct aws_h2_stream_data_write *write = s_h2_stream_get_current_write(stream);
    return write->data_stream;
}

static bool s_h2_stream_does_current_write_end_stream(struct aws_h2_stream *stream) {
    struct aws_h2_stream_data_write *write = s_h2_stream_get_current_write(stream);
    return write->end_stream;
}

int aws_h2_stream_on_activated(struct aws_h2_stream *stream, enum aws_h2_stream_body_state *body_state) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    struct aws_h2_connection *connection = s_get_h2_connection(stream);

    /* Create HEADERS frame */
    struct aws_http_message *msg = stream->thread_data.outgoing_message;
    /* Should be ensured when the stream is created */
    AWS_ASSERT(aws_http_message_get_protocol_version(msg) == AWS_HTTP_VERSION_2);
    /* If manual write, always has data to be sent. */
    bool with_data = aws_http_message_get_body_stream(msg) != NULL || stream->manual_write;

    struct aws_http_headers *h2_headers = aws_http_message_get_headers(msg);

    struct aws_h2_frame *headers_frame = aws_h2_frame_new_headers(
        stream->base.alloc,
        stream->base.id,
        h2_headers,
        !with_data /* end_stream */,
        0 /* padding - not currently configurable via public API */,
        NULL /* priority - not currently configurable via public API */);

    if (!headers_frame) {
        AWS_H2_STREAM_LOGF(ERROR, stream, "Failed to create HEADERS frame: %s", aws_error_name(aws_last_error()));
        goto error;
    }
    AWS_ASSERT(stream->base.metrics.send_start_timestamp_ns == -1);
    aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.send_start_timestamp_ns);
    /* Initialize the flow-control window size */
    stream->thread_data.window_size_peer =
        connection->thread_data.settings_peer[AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE];
    stream->thread_data.window_size_self =
        connection->thread_data.settings_self[AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE];

    if (with_data) {
        /* If stream has DATA to send, put it in the outgoing_streams_list, and we'll send data later */
        stream->thread_data.state = AWS_H2_STREAM_STATE_OPEN;
        AWS_H2_STREAM_LOG(TRACE, stream, "Sending HEADERS. State -> OPEN");
    } else {
        /* If stream has no body, then HEADERS frame marks the end of outgoing data */
        stream->thread_data.state = AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL;
        AWS_H2_STREAM_LOG(TRACE, stream, "Sending HEADERS with END_STREAM. State -> HALF_CLOSED_LOCAL");
        /* There is no further frames to be sent, now is the end timestamp of sending. */
        AWS_ASSERT(stream->base.metrics.send_end_timestamp_ns == -1);
        aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.send_end_timestamp_ns);
        stream->base.metrics.sending_duration_ns =
            stream->base.metrics.send_end_timestamp_ns - stream->base.metrics.send_start_timestamp_ns;
    }

    if (s_h2_stream_has_outgoing_writes(stream)) {
        *body_state = AWS_H2_STREAM_BODY_STATE_ONGOING;
    } else {
        if (stream->manual_write) {
            stream->thread_data.waiting_for_writes = true;
            *body_state = AWS_H2_STREAM_BODY_STATE_WAITING_WRITES;
        } else {
            *body_state = AWS_H2_STREAM_BODY_STATE_NONE;
        }
    }
    aws_h2_connection_enqueue_outgoing_frame(connection, headers_frame);
    return AWS_OP_SUCCESS;

error:
    return AWS_OP_ERR;
}

int aws_h2_stream_encode_data_frame(
    struct aws_h2_stream *stream,
    struct aws_h2_frame_encoder *encoder,
    struct aws_byte_buf *output,
    int *data_encode_status) {

    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);
    AWS_PRECONDITION(
        stream->thread_data.state == AWS_H2_STREAM_STATE_OPEN ||
        stream->thread_data.state == AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE);
    struct aws_h2_connection *connection = s_get_h2_connection(stream);
    AWS_PRECONDITION(connection->thread_data.window_size_peer > AWS_H2_MIN_WINDOW_SIZE);

    if (stream->thread_data.window_size_peer <= AWS_H2_MIN_WINDOW_SIZE) {
        /* The stream is stalled now */
        *data_encode_status = AWS_H2_DATA_ENCODE_ONGOING_WINDOW_STALLED;
        return AWS_OP_SUCCESS;
    }

    *data_encode_status = AWS_H2_DATA_ENCODE_COMPLETE;
    struct aws_input_stream *input_stream = s_h2_stream_get_data_stream(stream);
    AWS_ASSERT(input_stream);

    bool input_stream_complete = false;
    bool input_stream_stalled = false;
    bool ends_stream = s_h2_stream_does_current_write_end_stream(stream);
    if (aws_h2_encode_data_frame(
            encoder,
            stream->base.id,
            input_stream,
            ends_stream,
            0 /*pad_length*/,
            &stream->thread_data.window_size_peer,
            &connection->thread_data.window_size_peer,
            output,
            &input_stream_complete,
            &input_stream_stalled)) {

        /* Failed to write DATA, treat it as a Stream Error */
        AWS_H2_STREAM_LOGF(ERROR, stream, "Error encoding stream DATA, %s", aws_error_name(aws_last_error()));
        struct aws_h2err returned_h2err = s_send_rst_and_close_stream(stream, aws_h2err_from_last_error());
        if (aws_h2err_failed(returned_h2err)) {
            aws_h2_connection_shutdown_due_to_write_err(connection, returned_h2err.aws_code);
        }
        return AWS_OP_SUCCESS;
    }

    bool waiting_writes = false;
    if (input_stream_complete) {
        s_h2_stream_write_data_complete(stream, &waiting_writes);
    }

    /*
     * input_stream_complete for manual writes just means the current outgoing_write is complete. The body is not
     * complete for real until the stream is told to close
     */
    if (input_stream_complete && ends_stream) {
        /* Done sending data. No more data will be sent. */
        AWS_ASSERT(stream->base.metrics.send_end_timestamp_ns == -1);
        aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.send_end_timestamp_ns);
        stream->base.metrics.sending_duration_ns =
            stream->base.metrics.send_end_timestamp_ns - stream->base.metrics.send_start_timestamp_ns;

        if (stream->thread_data.state == AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE) {
            /* Both sides have sent END_STREAM */
            stream->thread_data.state = AWS_H2_STREAM_STATE_CLOSED;
            AWS_H2_STREAM_LOG(TRACE, stream, "Sent END_STREAM. State -> CLOSED");
            /* Tell connection that stream is now closed */
            if (aws_h2_connection_on_stream_closed(
                    connection, stream, AWS_H2_STREAM_CLOSED_WHEN_BOTH_SIDES_END_STREAM, AWS_ERROR_SUCCESS)) {
                return AWS_OP_ERR;
            }
        } else {
            /* Else can't close until we receive END_STREAM */
            stream->thread_data.state = AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL;
            AWS_H2_STREAM_LOG(TRACE, stream, "Sent END_STREAM. State -> HALF_CLOSED_LOCAL");
        }
    } else {
        *data_encode_status = AWS_H2_DATA_ENCODE_ONGOING;
        if (input_stream_stalled) {
            AWS_ASSERT(!input_stream_complete);
            *data_encode_status = AWS_H2_DATA_ENCODE_ONGOING_BODY_STREAM_STALLED;
        }
        if (stream->thread_data.window_size_peer <= AWS_H2_MIN_WINDOW_SIZE) {
            /* if body and window both stalled, we take the window stalled status, which will take the stream out
             * from outgoing list */
            *data_encode_status = AWS_H2_DATA_ENCODE_ONGOING_WINDOW_STALLED;
        }
        if (waiting_writes) {
            /* if window stalled and we waiting for manual writes, we take waiting writes status, which will be handled
             * properly if more writes coming, but windows is still stalled. But not the other way around. */
            AWS_ASSERT(input_stream_complete);
            *data_encode_status = AWS_H2_DATA_ENCODE_ONGOING_WAITING_FOR_WRITES;
        }
    }

    return AWS_OP_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_headers_begin(struct aws_h2_stream *stream) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    struct aws_h2err stream_err = s_check_state_allows_frame_type(stream, AWS_H2_FRAME_T_HEADERS);
    if (aws_h2err_failed(stream_err)) {
        return s_send_rst_and_close_stream(stream, stream_err);
    }
    aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.receive_start_timestamp_ns);

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_headers_i(
    struct aws_h2_stream *stream,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type) {

    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    /* Not calling s_check_state_allows_frame_type() here because we already checked
     * at start of HEADERS frame in aws_h2_stream_on_decoder_headers_begin() */

    bool is_server = stream->base.server_data;

    /* RFC-7540 8.1 - Message consists of:
     * - 0+ Informational 1xx headers (response-only, decoder validates that this only occurs in responses)
     * - 1 main headers with normal request or response.
     * - 0 or 1 trailing headers with no pseudo-headers */
    switch (block_type) {
        case AWS_HTTP_HEADER_BLOCK_INFORMATIONAL:
            if (stream->thread_data.received_main_headers) {
                AWS_H2_STREAM_LOG(
                    ERROR, stream, "Malformed message, received informational (1xx) response after main response");
                goto malformed;
            }
            break;
        case AWS_HTTP_HEADER_BLOCK_MAIN:
            if (stream->thread_data.received_main_headers) {
                AWS_H2_STREAM_LOG(ERROR, stream, "Malformed message, received second set of headers");
                goto malformed;
            }
            break;
        case AWS_HTTP_HEADER_BLOCK_TRAILING:
            if (!stream->thread_data.received_main_headers) {
                /* A HEADERS frame without any pseudo-headers looks like trailing headers to the decoder */
                AWS_H2_STREAM_LOG(ERROR, stream, "Malformed headers lack required pseudo-header fields.");
                goto malformed;
            }
            break;
        default:
            AWS_ASSERT(0);
    }

    if (is_server) {
        return aws_h2err_from_aws_code(AWS_ERROR_UNIMPLEMENTED);

    } else {
        /* Client */
        switch (name_enum) {
            case AWS_HTTP_HEADER_STATUS: {
                uint64_t status_code = 0;
                int err = aws_byte_cursor_utf8_parse_u64(header->value, &status_code);
                AWS_ASSERT(!err && "Invalid :status value. Decoder should have already validated this");
                (void)err;

                stream->base.client_data->response_status = (int)status_code;
            } break;
            case AWS_HTTP_HEADER_CONTENT_LENGTH: {
                if (stream->thread_data.content_length_received) {
                    AWS_H2_STREAM_LOG(ERROR, stream, "Duplicate content-length value");
                    goto malformed;
                }
                if (aws_byte_cursor_utf8_parse_u64(header->value, &stream->thread_data.incoming_content_length)) {
                    AWS_H2_STREAM_LOG(ERROR, stream, "Invalid content-length value");
                    goto malformed;
                }
                stream->thread_data.content_length_received = true;
            } break;
            default:
                break;
        }
    }

    if (stream->base.on_incoming_headers) {
        if (stream->base.on_incoming_headers(&stream->base, block_type, header, 1, stream->base.user_data)) {
            AWS_H2_STREAM_LOGF(
                ERROR, stream, "Incoming header callback raised error, %s", aws_error_name(aws_last_error()));
            return s_send_rst_and_close_stream(stream, aws_h2err_from_last_error());
        }
    }

    return AWS_H2ERR_SUCCESS;

malformed:
    /* RFC-9113 8.1.1 Malformed requests or responses that are detected MUST be treated as a stream error
     * (Section 5.4.2) of type PROTOCOL_ERROR.*/
    return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
}

struct aws_h2err aws_h2_stream_on_decoder_headers_end(
    struct aws_h2_stream *stream,
    bool malformed,
    enum aws_http_header_block block_type) {

    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    /* Not calling s_check_state_allows_frame_type() here because we already checked
     * at start of HEADERS frame in aws_h2_stream_on_decoder_headers_begin() */

    if (malformed) {
        AWS_H2_STREAM_LOG(ERROR, stream, "Headers are malformed");
        return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
    }

    switch (block_type) {
        case AWS_HTTP_HEADER_BLOCK_INFORMATIONAL:
            AWS_H2_STREAM_LOG(TRACE, stream, "Informational 1xx header-block done.");
            break;
        case AWS_HTTP_HEADER_BLOCK_MAIN:
            AWS_H2_STREAM_LOG(TRACE, stream, "Main header-block done.");
            stream->thread_data.received_main_headers = true;
            break;
        case AWS_HTTP_HEADER_BLOCK_TRAILING:
            AWS_H2_STREAM_LOG(TRACE, stream, "Trailing 1xx header-block done.");
            break;
        default:
            AWS_ASSERT(0);
    }

    if (stream->base.on_incoming_header_block_done) {
        if (stream->base.on_incoming_header_block_done(&stream->base, block_type, stream->base.user_data)) {
            AWS_H2_STREAM_LOGF(
                ERROR,
                stream,
                "Incoming-header-block-done callback raised error, %s",
                aws_error_name(aws_last_error()));
            return s_send_rst_and_close_stream(stream, aws_h2err_from_last_error());
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_push_promise(struct aws_h2_stream *stream, uint32_t promised_stream_id) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    struct aws_h2err stream_err = s_check_state_allows_frame_type(stream, AWS_H2_FRAME_T_PUSH_PROMISE);
    if (aws_h2err_failed(stream_err)) {
        return s_send_rst_and_close_stream(stream, stream_err);
    }

    /* Note: Until we have a need for it, PUSH_PROMISE is not a fully supported feature.
     * Promised streams are automatically rejected in a manner compliant with RFC-7540. */
    AWS_H2_STREAM_LOG(DEBUG, stream, "Automatically rejecting promised stream, PUSH_PROMISE is not fully supported");
    if (aws_h2_connection_send_rst_and_close_reserved_stream(
            s_get_h2_connection(stream), promised_stream_id, AWS_HTTP2_ERR_REFUSED_STREAM)) {
        return aws_h2err_from_last_error();
    }

    return AWS_H2ERR_SUCCESS;
}

static int s_stream_send_update_window(struct aws_h2_stream *stream, uint32_t window_size) {
    struct aws_h2_frame *stream_window_update_frame =
        aws_h2_frame_new_window_update(stream->base.alloc, stream->base.id, window_size);
    if (!stream_window_update_frame) {
        AWS_H2_STREAM_LOGF(
            ERROR,
            stream,
            "WINDOW_UPDATE frame on stream failed to be sent, error %s",
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }

    aws_h2_connection_enqueue_outgoing_frame(s_get_h2_connection(stream), stream_window_update_frame);
    stream->thread_data.window_size_self += window_size;
    return AWS_OP_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_data_begin(
    struct aws_h2_stream *stream,
    uint32_t payload_len,
    uint32_t total_padding_bytes,
    bool end_stream) {

    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    struct aws_h2err stream_err = s_check_state_allows_frame_type(stream, AWS_H2_FRAME_T_DATA);
    if (aws_h2err_failed(stream_err)) {
        return s_send_rst_and_close_stream(stream, stream_err);
    }

    if (!stream->thread_data.received_main_headers) {
        AWS_H2_STREAM_LOG(ERROR, stream, "Malformed message, received DATA before main HEADERS");
        return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
    }

    if (stream->thread_data.content_length_received) {
        uint64_t data_len = payload_len - total_padding_bytes;
        if (aws_add_u64_checked(
                stream->thread_data.incoming_data_length, data_len, &stream->thread_data.incoming_data_length)) {
            return s_send_rst_and_close_stream(stream, aws_h2err_from_aws_code(AWS_ERROR_OVERFLOW_DETECTED));
        }

        if (stream->thread_data.incoming_data_length > stream->thread_data.incoming_content_length) {
            AWS_H2_STREAM_LOGF(
                ERROR,
                stream,
                "Total received data payload=%" PRIu64 " has exceed the received content-length header, which=%" PRIi64
                ". Closing malformed stream",
                stream->thread_data.incoming_data_length,
                stream->thread_data.incoming_content_length);
            return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
        }
    }

    /* RFC-7540 6.9.1:
     * The sender MUST NOT send a flow-controlled frame with a length that exceeds
     * the space available in either of the flow-control windows advertised by the receiver.
     * Frames with zero length with the END_STREAM flag set (that is, an empty DATA frame)
     * MAY be sent if there is no available space in either flow-control window. */
    if ((int32_t)payload_len > stream->thread_data.window_size_self && payload_len != 0) {
        AWS_H2_STREAM_LOGF(
            ERROR,
            stream,
            "DATA length=%" PRIu32 " exceeds flow-control window=%" PRIi64,
            payload_len,
            stream->thread_data.window_size_self);
        return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR));
    }
    stream->thread_data.window_size_self -= payload_len;

    /* If stream isn't over, we may need to send automatic window updates to keep data flowing */
    if (!end_stream) {
        uint32_t auto_window_update;
        if (stream->base.owning_connection->stream_manual_window_management) {
            /* Automatically update the flow-window to account for padding, even though "manual window management"
             * is enabled, because the current API doesn't have any way to inform the user about padding,
             * so we can't expect them to manage it themselves. */
            auto_window_update = total_padding_bytes;
        } else {
            /* Automatically update the full amount we just received */
            auto_window_update = payload_len;
        }

        if (auto_window_update != 0) {
            if (s_stream_send_update_window(stream, auto_window_update)) {
                return aws_h2err_from_last_error();
            }
            AWS_H2_STREAM_LOGF(
                TRACE,
                stream,
                "Automatically updating stream window by %" PRIu32 "(%" PRIu32 " due to padding).",
                auto_window_update,
                total_padding_bytes);
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_data_i(struct aws_h2_stream *stream, struct aws_byte_cursor data) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    /* Not calling s_check_state_allows_frame_type() here because we already checked at start of DATA frame in
     * aws_h2_stream_on_decoder_data_begin() */

    if (stream->base.on_incoming_body) {
        if (stream->base.on_incoming_body(&stream->base, &data, stream->base.user_data)) {
            AWS_H2_STREAM_LOGF(
                ERROR, stream, "Incoming body callback raised error, %s", aws_error_name(aws_last_error()));
            return s_send_rst_and_close_stream(stream, aws_h2err_from_last_error());
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_window_update(
    struct aws_h2_stream *stream,
    uint32_t window_size_increment,
    bool *window_resume) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    *window_resume = false;

    struct aws_h2err stream_err = s_check_state_allows_frame_type(stream, AWS_H2_FRAME_T_WINDOW_UPDATE);
    if (aws_h2err_failed(stream_err)) {
        return s_send_rst_and_close_stream(stream, stream_err);
    }
    if (window_size_increment == 0) {
        /* flow-control window increment of 0 MUST be treated as error (RFC7540 6.9.1) */
        AWS_H2_STREAM_LOG(ERROR, stream, "Window update frame with 0 increment size");
        return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
    }
    int32_t old_window_size = stream->thread_data.window_size_peer;
    stream_err = (aws_h2_stream_window_size_change(stream, window_size_increment, false /*self*/));
    if (aws_h2err_failed(stream_err)) {
        /* We MUST NOT allow a flow-control window to exceed the max */
        AWS_H2_STREAM_LOG(
            ERROR, stream, "Window update frame causes the stream flow-control window to exceed the maximum size");
        return s_send_rst_and_close_stream(stream, stream_err);
    }
    if (stream->thread_data.window_size_peer > AWS_H2_MIN_WINDOW_SIZE && old_window_size <= AWS_H2_MIN_WINDOW_SIZE) {
        *window_resume = true;
    }
    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_end_stream(struct aws_h2_stream *stream) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    /* Not calling s_check_state_allows_frame_type() here because END_STREAM isn't
     * an actual frame type. It's a flag on DATA or HEADERS frames, and we
     * already checked the legality of those frames in their respective callbacks. */

    AWS_ASSERT(stream->base.metrics.receive_start_timestamp_ns != -1);
    AWS_ASSERT(stream->base.metrics.receive_end_timestamp_ns == -1);
    aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.receive_end_timestamp_ns);
    AWS_ASSERT(stream->base.metrics.receive_end_timestamp_ns >= stream->base.metrics.receive_start_timestamp_ns);
    stream->base.metrics.receiving_duration_ns =
        stream->base.metrics.receive_end_timestamp_ns - stream->base.metrics.receive_start_timestamp_ns;

    if (stream->thread_data.content_length_received) {
        if (stream->base.request_method != AWS_HTTP_METHOD_HEAD &&
            stream->base.client_data->response_status != AWS_HTTP_STATUS_CODE_304_NOT_MODIFIED) {
            /**
             * RFC-9110 8.6.
             * A server MAY send a Content-Length header field in a response to a HEAD request.
             * A server MAY send a Content-Length header field in a 304 (Not Modified) response.
             * But both of these condition will have no body receive.
             */
            if (stream->thread_data.incoming_data_length != stream->thread_data.incoming_content_length) {
                /**
                 * RFC-9113 8.1.1:
                 * A request or response is also malformed if the value of a content-length header field does not equal
                 * the sum of the DATA frame payload lengths that form the content, unless the message is defined as
                 * having no content.
                 *
                 * Clients MUST NOT accept a malformed response.
                 */
                AWS_H2_STREAM_LOGF(
                    ERROR,
                    stream,
                    "Total received data payload=%" PRIu64
                    " does not match the received content-length header, which=%" PRIi64 ". Closing malformed stream",
                    stream->thread_data.incoming_data_length,
                    stream->thread_data.incoming_content_length);
                return s_send_rst_and_close_stream(stream, aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR));
            }
        }
    }

    if (stream->thread_data.state == AWS_H2_STREAM_STATE_HALF_CLOSED_LOCAL) {
        /* Both sides have sent END_STREAM */
        stream->thread_data.state = AWS_H2_STREAM_STATE_CLOSED;
        AWS_H2_STREAM_LOG(TRACE, stream, "Received END_STREAM. State -> CLOSED");
        /* Tell connection that stream is now closed */
        if (aws_h2_connection_on_stream_closed(
                s_get_h2_connection(stream),
                stream,
                AWS_H2_STREAM_CLOSED_WHEN_BOTH_SIDES_END_STREAM,
                AWS_ERROR_SUCCESS)) {
            return aws_h2err_from_last_error();
        }

    } else {
        /* Else can't close until our side sends END_STREAM */
        stream->thread_data.state = AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE;
        AWS_H2_STREAM_LOG(TRACE, stream, "Received END_STREAM. State -> HALF_CLOSED_REMOTE");
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err aws_h2_stream_on_decoder_rst_stream(struct aws_h2_stream *stream, uint32_t h2_error_code) {
    AWS_PRECONDITION_ON_CHANNEL_THREAD(stream);

    /* Check that this state allows RST_STREAM. */
    struct aws_h2err err = s_check_state_allows_frame_type(stream, AWS_H2_FRAME_T_RST_STREAM);
    if (aws_h2err_failed(err)) {
        /* Usually we send a RST_STREAM when the state doesn't allow a frame type, but RFC-7540 5.4.2 says:
         * "To avoid looping, an endpoint MUST NOT send a RST_STREAM in response to a RST_STREAM frame." */
        return err;
    }

    /* RFC-7540 8.1 - a server MAY request that the client abort transmission of a request without error by sending a
     * RST_STREAM with an error code of NO_ERROR after sending a complete response (i.e., a frame with the END_STREAM
     * flag). Clients MUST NOT discard responses as a result of receiving such a RST_STREAM */
    int aws_error_code;
    if (stream->base.client_data && (h2_error_code == AWS_HTTP2_ERR_NO_ERROR) &&
        (stream->thread_data.state == AWS_H2_STREAM_STATE_HALF_CLOSED_REMOTE)) {

        aws_error_code = AWS_ERROR_SUCCESS;

    } else {
        aws_error_code = AWS_ERROR_HTTP_RST_STREAM_RECEIVED;
        AWS_H2_STREAM_LOGF(
            ERROR,
            stream,
            "Peer terminated stream with HTTP/2 RST_STREAM frame, error-code=0x%x(%s)",
            h2_error_code,
            aws_http2_error_code_to_str(h2_error_code));
    }

    stream->thread_data.state = AWS_H2_STREAM_STATE_CLOSED;
    stream->received_reset_error_code = h2_error_code;

    AWS_H2_STREAM_LOGF(
        TRACE,
        stream,
        "Received RST_STREAM code=0x%x(%s). State -> CLOSED",
        h2_error_code,
        aws_http2_error_code_to_str(h2_error_code));

    if (aws_h2_connection_on_stream_closed(
            s_get_h2_connection(stream), stream, AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_RECEIVED, aws_error_code)) {
        return aws_h2err_from_last_error();
    }

    return AWS_H2ERR_SUCCESS;
}

static int s_stream_write_data(
    struct aws_http_stream *stream_base,
    const struct aws_http2_stream_write_data_options *options) {
    struct aws_h2_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h2_stream, base);
    if (!stream->manual_write) {
        AWS_H2_STREAM_LOG(
            ERROR,
            stream,
            "Manual writes are not enabled. You need to enable manual writes using by setting "
            "'http2_use_manual_data_writes' to true in 'aws_http_make_request_options'");
        return aws_raise_error(AWS_ERROR_HTTP_MANUAL_WRITE_NOT_ENABLED);
    }
    struct aws_h2_connection *connection = s_get_h2_connection(stream);

    /* queue this new write into the pending write list for the stream */
    struct aws_h2_stream_data_write *pending_write =
        aws_mem_calloc(stream->base.alloc, 1, sizeof(struct aws_h2_stream_data_write));
    if (options->data) {
        pending_write->data_stream = aws_input_stream_acquire(options->data);
    } else {
        struct aws_byte_cursor empty_cursor;
        AWS_ZERO_STRUCT(empty_cursor);
        pending_write->data_stream = aws_input_stream_new_from_cursor(stream->base.alloc, &empty_cursor);
    }
    bool schedule_cross_thread_work = false;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(stream);
        {
            if (stream->synced_data.api_state != AWS_H2_STREAM_API_STATE_ACTIVE) {
                s_unlock_synced_data(stream);
                int error_code = stream->synced_data.api_state == AWS_H2_STREAM_API_STATE_INIT
                                     ? AWS_ERROR_HTTP_STREAM_NOT_ACTIVATED
                                     : AWS_ERROR_HTTP_STREAM_HAS_COMPLETED;
                s_stream_data_write_destroy(stream, pending_write, error_code);
                AWS_H2_STREAM_LOG(ERROR, stream, "Cannot write DATA frames to an inactive or closed stream");
                return aws_raise_error(error_code);
            }

            if (stream->synced_data.manual_write_ended) {
                s_unlock_synced_data(stream);
                s_stream_data_write_destroy(stream, pending_write, AWS_ERROR_HTTP_MANUAL_WRITE_HAS_COMPLETED);
                AWS_H2_STREAM_LOG(ERROR, stream, "Cannot write DATA frames to a stream after manual write ended");
                /* Fail with error, otherwise, people can wait for on_complete callback that will never be invoked. */
                return aws_raise_error(AWS_ERROR_HTTP_MANUAL_WRITE_HAS_COMPLETED);
            }
            /* Not setting this until we're sure we succeeded, so that callback doesn't fire on cleanup if we fail */
            if (options->end_stream) {
                stream->synced_data.manual_write_ended = true;
            }
            pending_write->end_stream = options->end_stream;
            pending_write->on_complete = options->on_complete;
            pending_write->user_data = options->user_data;

            aws_linked_list_push_back(&stream->synced_data.pending_write_list, &pending_write->node);
            schedule_cross_thread_work = !stream->synced_data.is_cross_thread_work_task_scheduled;
            stream->synced_data.is_cross_thread_work_task_scheduled = true;
        }
        s_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (schedule_cross_thread_work) {
        AWS_H2_STREAM_LOG(TRACE, stream, "Scheduling stream cross-thread work task");
        /* increment the refcount of stream to keep it alive until the task runs */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &stream->cross_thread_work_task);
    }

    return AWS_OP_SUCCESS;
}
