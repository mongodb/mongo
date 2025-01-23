/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/h1_stream.h>

#include <aws/http/private/h1_connection.h>
#include <aws/http/private/h1_encoder.h>

#include <aws/http/status_code.h>
#include <aws/io/logging.h>
#include <aws/io/stream.h>

#include <inttypes.h>

static void s_stream_destroy(struct aws_http_stream *stream_base) {
    struct aws_h1_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h1_stream, base);
    AWS_ASSERT(
        stream->synced_data.api_state != AWS_H1_STREAM_API_STATE_ACTIVE &&
        "Stream should be complete (or never-activated) when stream destroyed");
    AWS_ASSERT(
        aws_linked_list_empty(&stream->thread_data.pending_chunk_list) &&
        aws_linked_list_empty(&stream->synced_data.pending_chunk_list) &&
        "Chunks should be marked complete before stream destroyed");

    aws_h1_encoder_message_clean_up(&stream->encoder_message);
    aws_byte_buf_clean_up(&stream->incoming_storage_buf);
    aws_mem_release(stream->base.alloc, stream);
}

static struct aws_h1_connection *s_get_h1_connection(const struct aws_h1_stream *stream) {
    return AWS_CONTAINER_OF(stream->base.owning_connection, struct aws_h1_connection, base);
}

static void s_stream_lock_synced_data(struct aws_h1_stream *stream) {
    aws_h1_connection_lock_synced_data(s_get_h1_connection(stream));
}

static void s_stream_unlock_synced_data(struct aws_h1_stream *stream) {
    aws_h1_connection_unlock_synced_data(s_get_h1_connection(stream));
}

static void s_stream_cross_thread_work_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct aws_h1_stream *stream = arg;
    struct aws_h1_connection *connection = s_get_h1_connection(stream);

    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Running stream cross-thread work task.", (void *)&stream->base);

    /* BEGIN CRITICAL SECTION */
    s_stream_lock_synced_data(stream);

    stream->synced_data.is_cross_thread_work_task_scheduled = false;

    int api_state = stream->synced_data.api_state;

    bool found_chunks = !aws_linked_list_empty(&stream->synced_data.pending_chunk_list);
    aws_linked_list_move_all_back(&stream->thread_data.pending_chunk_list, &stream->synced_data.pending_chunk_list);

    stream->encoder_message.trailer = stream->synced_data.pending_trailer;
    stream->synced_data.pending_trailer = NULL;

    bool has_outgoing_response = stream->synced_data.has_outgoing_response;

    uint64_t pending_window_update = stream->synced_data.pending_window_update;
    stream->synced_data.pending_window_update = 0;

    s_stream_unlock_synced_data(stream);
    /* END CRITICAL SECTION */

    /* If we have any new outgoing data, prompt the connection to try and send it. */
    bool new_outgoing_data = found_chunks;

    /* If we JUST learned about having an outgoing response, that's a reason to try sending data */
    if (has_outgoing_response && !stream->thread_data.has_outgoing_response) {
        stream->thread_data.has_outgoing_response = true;
        new_outgoing_data = true;
    }

    if (new_outgoing_data && (api_state == AWS_H1_STREAM_API_STATE_ACTIVE)) {
        aws_h1_connection_try_write_outgoing_stream(connection);
    }

    /* Add to window size using saturated sum to prevent overflow.
     * Saturating is fine because it's a u64, the stream could never receive that much data. */
    stream->thread_data.stream_window =
        aws_add_u64_saturating(stream->thread_data.stream_window, pending_window_update);
    if ((pending_window_update > 0) && (api_state == AWS_H1_STREAM_API_STATE_ACTIVE)) {
        /* Now that stream window is larger, connection might have buffered
         * data to send, or might need to increment its own window */
        aws_h1_connection_try_process_read_messages(connection);
    }

done:
    /* Release reference that kept stream alive until task ran */
    aws_http_stream_release(&stream->base);
}

/* Note the update in synced_data, and schedule the cross_thread_work_task if necessary */
static void s_stream_update_window(struct aws_http_stream *stream_base, size_t increment_size) {
    if (increment_size == 0) {
        return;
    }

    if (!stream_base->owning_connection->stream_manual_window_management) {
        return;
    }

    struct aws_h1_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h1_stream, base);
    bool should_schedule_task = false;

    { /* BEGIN CRITICAL SECTION */
        s_stream_lock_synced_data(stream);

        /* Saturated sum. It's a u64. The stream could never receive that much data. */
        stream->synced_data.pending_window_update =
            aws_add_u64_saturating(stream->synced_data.pending_window_update, increment_size);

        /* Don't alert the connection unless the stream is active */
        if (stream->synced_data.api_state == AWS_H1_STREAM_API_STATE_ACTIVE) {
            if (!stream->synced_data.is_cross_thread_work_task_scheduled) {
                stream->synced_data.is_cross_thread_work_task_scheduled = true;
                should_schedule_task = true;
            }
        }

        s_stream_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (should_schedule_task) {
        /* Keep stream alive until task completes */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Scheduling stream cross-thread work task.", (void *)stream_base);
        aws_channel_schedule_task_now(
            stream->base.owning_connection->channel_slot->channel, &stream->cross_thread_work_task);
    }
}

static int s_stream_write_chunk(struct aws_http_stream *stream_base, const struct aws_http1_chunk_options *options) {
    AWS_PRECONDITION(stream_base);
    AWS_PRECONDITION(options);
    struct aws_h1_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h1_stream, base);

    if (options->chunk_data == NULL && options->chunk_data_size > 0) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM, "id=%p: Chunk data cannot be NULL if data size is non-zero", (void *)stream_base);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_h1_chunk *chunk = aws_h1_chunk_new(stream_base->alloc, options);
    if (AWS_UNLIKELY(NULL == chunk)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=%p: Failed to initialize streamed chunk, error %d (%s).",
            (void *)stream_base,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }

    int error_code = 0;
    bool should_schedule_task = false;

    { /* BEGIN CRITICAL SECTION */
        s_stream_lock_synced_data(stream);

        /* Can only add chunks while stream is active. */
        if (stream->synced_data.api_state != AWS_H1_STREAM_API_STATE_ACTIVE) {
            error_code = (stream->synced_data.api_state == AWS_H1_STREAM_API_STATE_INIT)
                             ? AWS_ERROR_HTTP_STREAM_NOT_ACTIVATED
                             : AWS_ERROR_HTTP_STREAM_HAS_COMPLETED;
            goto unlock;
        }

        /* Prevent user trying to submit chunks without having set the required headers.
         * This check also prevents a server-user submitting chunks before the response has been submitted. */
        if (!stream->synced_data.using_chunked_encoding) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Cannot write chunks without 'transfer-encoding: chunked' header.",
                (void *)stream_base);
            error_code = AWS_ERROR_INVALID_STATE;
            goto unlock;
        }

        if (stream->synced_data.has_final_chunk) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM, "id=%p: Cannot write additional chunk after final chunk.", (void *)stream_base);
            error_code = AWS_ERROR_INVALID_STATE;
            goto unlock;
        }

        /* success */
        if (chunk->data_size == 0) {
            stream->synced_data.has_final_chunk = true;
        }
        aws_linked_list_push_back(&stream->synced_data.pending_chunk_list, &chunk->node);
        should_schedule_task = !stream->synced_data.is_cross_thread_work_task_scheduled;
        stream->synced_data.is_cross_thread_work_task_scheduled = true;

    unlock:
        s_stream_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=%p: Failed to add chunk, error %d (%s)",
            (void *)stream_base,
            error_code,
            aws_error_name(error_code));

        aws_h1_chunk_destroy(chunk);
        return aws_raise_error(error_code);
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: Adding chunk with size %" PRIu64 " to stream",
        (void *)stream,
        options->chunk_data_size);

    if (should_schedule_task) {
        /* Keep stream alive until task completes */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Scheduling stream cross-thread work task.", (void *)stream_base);
        aws_channel_schedule_task_now(
            stream->base.owning_connection->channel_slot->channel, &stream->cross_thread_work_task);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_STREAM, "id=%p: Stream cross-thread work task was already scheduled.", (void *)stream_base);
    }

    return AWS_OP_SUCCESS;
}

static int s_stream_add_trailer(struct aws_http_stream *stream_base, const struct aws_http_headers *trailing_headers) {
    AWS_PRECONDITION(stream_base);
    AWS_PRECONDITION(trailing_headers);
    struct aws_h1_stream *stream = AWS_CONTAINER_OF(stream_base, struct aws_h1_stream, base);

    struct aws_h1_trailer *trailer = aws_h1_trailer_new(stream_base->alloc, trailing_headers);
    if (AWS_UNLIKELY(NULL == trailer)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=%p: Failed to initialize streamed trailer, error %d (%s).",
            (void *)stream_base,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }

    int error_code = 0;
    bool should_schedule_task = false;

    { /* BEGIN CRITICAL SECTION */
        s_stream_lock_synced_data(stream);
        /* Can only add trailers while stream is active. */
        if (stream->synced_data.api_state != AWS_H1_STREAM_API_STATE_ACTIVE) {
            error_code = (stream->synced_data.api_state == AWS_H1_STREAM_API_STATE_INIT)
                             ? AWS_ERROR_HTTP_STREAM_NOT_ACTIVATED
                             : AWS_ERROR_HTTP_STREAM_HAS_COMPLETED;
            goto unlock;
        }

        if (!stream->synced_data.using_chunked_encoding) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Cannot write trailers without 'transfer-encoding: chunked' header.",
                (void *)stream_base);
            error_code = AWS_ERROR_INVALID_STATE;
            goto unlock;
        }

        if (stream->synced_data.has_added_trailer) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Cannot write trailers twice.", (void *)stream_base);
            error_code = AWS_ERROR_INVALID_STATE;
            goto unlock;
        }

        if (stream->synced_data.has_final_chunk) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Cannot write trailers after final chunk.", (void *)stream_base);
            error_code = AWS_ERROR_INVALID_STATE;
            goto unlock;
        }

        stream->synced_data.has_added_trailer = true;
        stream->synced_data.pending_trailer = trailer;
        should_schedule_task = !stream->synced_data.is_cross_thread_work_task_scheduled;
        stream->synced_data.is_cross_thread_work_task_scheduled = true;

    unlock:
        s_stream_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_STREAM,
            "id=%p: Failed to add trailer, error %d (%s)",
            (void *)stream_base,
            error_code,
            aws_error_name(error_code));

        aws_h1_trailer_destroy(trailer);
        return aws_raise_error(error_code);
    }

    AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Adding trailer to stream", (void *)stream);

    if (should_schedule_task) {
        /* Keep stream alive until task completes */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Scheduling stream cross-thread work task.", (void *)stream_base);
        aws_channel_schedule_task_now(
            stream->base.owning_connection->channel_slot->channel, &stream->cross_thread_work_task);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_STREAM, "id=%p: Stream cross-thread work task was already scheduled.", (void *)stream_base);
    }

    return AWS_OP_SUCCESS;
}

static const struct aws_http_stream_vtable s_stream_vtable = {
    .destroy = s_stream_destroy,
    .update_window = s_stream_update_window,
    .activate = aws_h1_stream_activate,
    .cancel = aws_h1_stream_cancel,
    .http1_write_chunk = s_stream_write_chunk,
    .http1_add_trailer = s_stream_add_trailer,
    .http2_reset_stream = NULL,
    .http2_get_received_error_code = NULL,
    .http2_get_sent_error_code = NULL,
};

static struct aws_h1_stream *s_stream_new_common(
    struct aws_http_connection *connection_base,
    void *user_data,
    aws_http_on_incoming_headers_fn *on_incoming_headers,
    aws_http_on_incoming_header_block_done_fn *on_incoming_header_block_done,
    aws_http_on_incoming_body_fn *on_incoming_body,
    aws_http_on_stream_complete_fn *on_complete,
    aws_http_on_stream_destroy_fn *on_destroy) {

    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);

    struct aws_h1_stream *stream = aws_mem_calloc(connection_base->alloc, 1, sizeof(struct aws_h1_stream));
    if (!stream) {
        return NULL;
    }

    stream->base.vtable = &s_stream_vtable;
    stream->base.alloc = connection_base->alloc;
    stream->base.owning_connection = connection_base;
    stream->base.user_data = user_data;
    stream->base.on_incoming_headers = on_incoming_headers;
    stream->base.on_incoming_header_block_done = on_incoming_header_block_done;
    stream->base.on_incoming_body = on_incoming_body;
    stream->base.on_complete = on_complete;
    stream->base.on_destroy = on_destroy;
    stream->base.metrics.send_start_timestamp_ns = -1;
    stream->base.metrics.send_end_timestamp_ns = -1;
    stream->base.metrics.sending_duration_ns = -1;
    stream->base.metrics.receive_start_timestamp_ns = -1;
    stream->base.metrics.receive_end_timestamp_ns = -1;
    stream->base.metrics.receiving_duration_ns = -1;

    aws_channel_task_init(
        &stream->cross_thread_work_task, s_stream_cross_thread_work_task, stream, "http1_stream_cross_thread_work");

    aws_linked_list_init(&stream->thread_data.pending_chunk_list);
    aws_linked_list_init(&stream->synced_data.pending_chunk_list);

    stream->thread_data.stream_window = connection->initial_stream_window_size;

    /* Stream refcount starts at 1 for user and is incremented upon activation for the connection */
    aws_atomic_init_int(&stream->base.refcount, 1);

    return stream;
}

struct aws_h1_stream *aws_h1_stream_new_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options) {

    struct aws_h1_stream *stream = s_stream_new_common(
        client_connection,
        options->user_data,
        options->on_response_headers,
        options->on_response_header_block_done,
        options->on_response_body,
        options->on_complete,
        options->on_destroy);
    if (!stream) {
        return NULL;
    }

    /* Transform request if necessary */
    if (client_connection->proxy_request_transform) {
        if (client_connection->proxy_request_transform(options->request, client_connection->user_data)) {
            goto error;
        }
    }

    stream->base.client_data = &stream->base.client_or_server_data.client;
    stream->base.client_data->response_status = AWS_HTTP_STATUS_CODE_UNKNOWN;
    stream->base.client_data->response_first_byte_timeout_ms = options->response_first_byte_timeout_ms;
    stream->base.on_metrics = options->on_metrics;

    /* Validate request and cache info that the encoder will eventually need */
    if (aws_h1_encoder_message_init_from_request(
            &stream->encoder_message,
            client_connection->alloc,
            options->request,
            &stream->thread_data.pending_chunk_list)) {
        goto error;
    }

    /* RFC-7230 Section 6.3: The "close" connection option is used to signal
     * that a connection will not persist after the current request/response*/
    if (stream->encoder_message.has_connection_close_header) {
        stream->is_final_stream = true;
    }

    stream->synced_data.using_chunked_encoding = stream->encoder_message.has_chunked_encoding_header;

    return stream;

error:
    s_stream_destroy(&stream->base);
    return NULL;
}

struct aws_h1_stream *aws_h1_stream_new_request_handler(const struct aws_http_request_handler_options *options) {
    struct aws_h1_stream *stream = s_stream_new_common(
        options->server_connection,
        options->user_data,
        options->on_request_headers,
        options->on_request_header_block_done,
        options->on_request_body,
        options->on_complete,
        options->on_destroy);
    if (!stream) {
        return NULL;
    }

    /* This code is only executed in server mode and can only be invoked from the event-loop thread so don't worry
     * with the lock here. */
    stream->base.id = aws_http_connection_get_next_stream_id(options->server_connection);

    /* Request-handler (server) streams don't need user to call activate() on them.
     * Since these these streams can only be created on the event-loop thread,
     * it's not possible for callbacks to fire before the stream pointer is returned.
     * (Clients must call stream.activate() because they might create a stream on any thread) */
    stream->synced_data.api_state = AWS_H1_STREAM_API_STATE_ACTIVE;

    stream->base.server_data = &stream->base.client_or_server_data.server;
    stream->base.server_data->on_request_done = options->on_request_done;
    aws_atomic_fetch_add(&stream->base.refcount, 1);

    return stream;
}

int aws_h1_stream_send_response(struct aws_h1_stream *stream, struct aws_http_message *response) {
    struct aws_h1_connection *connection = s_get_h1_connection(stream);
    int error_code = 0;

    /* Validate the response and cache info that encoder will eventually need.
     * The encoder_message object will be moved into the stream later while holding the lock */
    struct aws_h1_encoder_message encoder_message;
    bool body_headers_ignored = stream->base.request_method == AWS_HTTP_METHOD_HEAD;
    if (aws_h1_encoder_message_init_from_response(
            &encoder_message,
            stream->base.alloc,
            response,
            body_headers_ignored,
            &stream->thread_data.pending_chunk_list)) {
        error_code = aws_last_error();
        goto error;
    }

    bool should_schedule_task = false;
    { /* BEGIN CRITICAL SECTION */
        s_stream_lock_synced_data(stream);
        if (stream->synced_data.api_state == AWS_H1_STREAM_API_STATE_COMPLETE) {
            error_code = AWS_ERROR_HTTP_STREAM_HAS_COMPLETED;
        } else if (stream->synced_data.has_outgoing_response) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_STREAM, "id=%p: Response already created on the stream", (void *)&stream->base);
            error_code = AWS_ERROR_INVALID_STATE;
        } else {
            stream->synced_data.has_outgoing_response = true;
            stream->encoder_message = encoder_message;
            if (encoder_message.has_connection_close_header) {
                /* This will be the last stream connection will process, new streams will be rejected */
                stream->is_final_stream = true;

                /* Note: We're touching the connection's synced_data, which is OK
                 * because an h1_connection and all its h1_streams share a single lock. */
                connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
            }
            stream->synced_data.using_chunked_encoding = stream->encoder_message.has_chunked_encoding_header;

            should_schedule_task = !stream->synced_data.is_cross_thread_work_task_scheduled;
            stream->synced_data.is_cross_thread_work_task_scheduled = true;
        }
        s_stream_unlock_synced_data(stream);
    } /* END CRITICAL SECTION */

    if (error_code) {
        goto error;
    }

    /* Success! */
    AWS_LOGF_DEBUG(
        AWS_LS_HTTP_STREAM, "id=%p: Created response on connection=%p: ", (void *)stream, (void *)connection);

    if (should_schedule_task) {
        /* Keep stream alive until task completes */
        aws_atomic_fetch_add(&stream->base.refcount, 1);
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Scheduling stream cross-thread work task.", (void *)&stream->base);
        aws_channel_schedule_task_now(
            stream->base.owning_connection->channel_slot->channel, &stream->cross_thread_work_task);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_STREAM, "id=%p: Stream cross-thread work task was already scheduled.", (void *)&stream->base);
    }

    return AWS_OP_SUCCESS;

error:
    AWS_LOGF_ERROR(
        AWS_LS_HTTP_STREAM,
        "id=%p: Sending response on the stream failed, error %d (%s)",
        (void *)&stream->base,
        error_code,
        aws_error_name(error_code));

    aws_h1_encoder_message_clean_up(&encoder_message);
    return aws_raise_error(error_code);
}
