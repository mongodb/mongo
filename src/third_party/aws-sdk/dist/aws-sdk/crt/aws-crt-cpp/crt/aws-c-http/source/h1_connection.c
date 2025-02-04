/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/clock.h>
#include <aws/common/math.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/http/private/h1_connection.h>
#include <aws/http/private/h1_decoder.h>
#include <aws/http/private/h1_stream.h>
#include <aws/http/private/request_response_impl.h>
#include <aws/http/status_code.h>
#include <aws/io/event_loop.h>
#include <aws/io/logging.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

enum {
    DECODER_INITIAL_SCRATCH_SIZE = 256,
};

static int s_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message);

static int s_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message);

static int s_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size);

static int s_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately);

static size_t s_handler_initial_window_size(struct aws_channel_handler *handler);
static size_t s_handler_message_overhead(struct aws_channel_handler *handler);
static void s_handler_destroy(struct aws_channel_handler *handler);
static void s_handler_installed(struct aws_channel_handler *handler, struct aws_channel_slot *slot);
static struct aws_http_stream *s_make_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options);
static struct aws_http_stream *s_new_server_request_handler_stream(
    const struct aws_http_request_handler_options *options);
static int s_stream_send_response(struct aws_http_stream *stream, struct aws_http_message *response);
static void s_connection_close(struct aws_http_connection *connection_base);
static void s_connection_stop_new_request(struct aws_http_connection *connection_base);
static bool s_connection_is_open(const struct aws_http_connection *connection_base);
static bool s_connection_new_requests_allowed(const struct aws_http_connection *connection_base);
static int s_decoder_on_request(
    enum aws_http_method method_enum,
    const struct aws_byte_cursor *method_str,
    const struct aws_byte_cursor *uri,
    void *user_data);
static int s_decoder_on_response(int status_code, void *user_data);
static int s_decoder_on_header(const struct aws_h1_decoded_header *header, void *user_data);
static int s_decoder_on_body(const struct aws_byte_cursor *data, bool finished, void *user_data);
static int s_decoder_on_done(void *user_data);
static void s_reset_statistics(struct aws_channel_handler *handler);
static void s_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats);
static void s_write_outgoing_stream(struct aws_h1_connection *connection, bool first_try);
static int s_try_process_next_stream_read_message(struct aws_h1_connection *connection, bool *out_stop_processing);

static struct aws_http_connection_vtable s_h1_connection_vtable = {
    .channel_handler_vtable =
        {
            .process_read_message = s_handler_process_read_message,
            .process_write_message = s_handler_process_write_message,
            .increment_read_window = s_handler_increment_read_window,
            .shutdown = s_handler_shutdown,
            .initial_window_size = s_handler_initial_window_size,
            .message_overhead = s_handler_message_overhead,
            .destroy = s_handler_destroy,
            .reset_statistics = s_reset_statistics,
            .gather_statistics = s_gather_statistics,
        },
    .on_channel_handler_installed = s_handler_installed,
    .make_request = s_make_request,
    .new_server_request_handler_stream = s_new_server_request_handler_stream,
    .stream_send_response = s_stream_send_response,
    .close = s_connection_close,
    .stop_new_requests = s_connection_stop_new_request,
    .is_open = s_connection_is_open,
    .new_requests_allowed = s_connection_new_requests_allowed,
    .change_settings = NULL,
    .send_ping = NULL,
    .send_goaway = NULL,
    .get_sent_goaway = NULL,
    .get_received_goaway = NULL,
    .get_local_settings = NULL,
    .get_remote_settings = NULL,
};

static const struct aws_h1_decoder_vtable s_h1_decoder_vtable = {
    .on_request = s_decoder_on_request,
    .on_response = s_decoder_on_response,
    .on_header = s_decoder_on_header,
    .on_body = s_decoder_on_body,
    .on_done = s_decoder_on_done,
};

void aws_h1_connection_lock_synced_data(struct aws_h1_connection *connection) {
    int err = aws_mutex_lock(&connection->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

void aws_h1_connection_unlock_synced_data(struct aws_h1_connection *connection) {
    int err = aws_mutex_unlock(&connection->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

/**
 * Internal function for bringing connection to a stop.
 * Invoked multiple times, including when:
 * - Channel is shutting down in the read direction.
 * - Channel is shutting down in the write direction.
 * - An error occurs.
 */
static void s_stop(
    struct aws_h1_connection *connection,
    bool stop_reading,
    bool stop_writing,
    bool schedule_shutdown,
    int error_code) {

    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_ASSERT(stop_reading || stop_writing || schedule_shutdown); /* You are required to stop at least 1 thing */

    if (stop_reading) {
        if (connection->thread_data.read_state == AWS_CONNECTION_READ_OPEN) {
            connection->thread_data.read_state = AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE;
        } else if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUTTING_DOWN) {
            /* Shutdown after pending */
            if (connection->thread_data.pending_shutdown_error_code != 0) {
                error_code = connection->thread_data.pending_shutdown_error_code;
            }
            connection->thread_data.read_state = AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE;
            aws_channel_slot_on_handler_shutdown_complete(
                connection->base.channel_slot, AWS_CHANNEL_DIR_READ, error_code, false);
        }
    }

    if (stop_writing) {
        connection->thread_data.is_writing_stopped = true;
    }
    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);

        /* Even if we're not scheduling shutdown just yet (ex: sent final request but waiting to read final response)
         * we don't consider the connection "open" anymore so user can't create more streams */
        connection->synced_data.is_open = false;
        connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;

        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    if (schedule_shutdown) {
        AWS_LOGF_INFO(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Shutting down connection with error code %d (%s).",
            (void *)&connection->base,
            error_code,
            aws_error_name(error_code));

        aws_channel_shutdown(connection->base.channel_slot->channel, error_code);

        if (stop_reading) {
            /* Increase the window size after shutdown starts, to prevent deadlock when data still pending in the TLS
             * handler. */
            aws_channel_slot_increment_read_window(connection->base.channel_slot, SIZE_MAX);
        }
    }
}

static void s_shutdown_due_to_error(struct aws_h1_connection *connection, int error_code) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (!error_code) {
        error_code = AWS_ERROR_UNKNOWN;
    }

    /* Stop reading AND writing if an error occurs.
     *
     * It doesn't currently seem worth the complexity to distinguish between read errors and write errors.
     * The only scenarios that would benefit from this are pipelining scenarios (ex: A server
     * could continue sending a response to request A if there was an error reading request B).
     * But pipelining in HTTP/1.1 is known to be fragile with regards to errors, so let's just keep it simple.
     */
    s_stop(connection, true /*stop_reading*/, true /*stop_writing*/, true /*schedule_shutdown*/, error_code);
}

/**
 * Helper to shutdown the connection from non-channel thread. (User wishes to close the connection)
 **/
static void s_shutdown_from_off_thread(struct aws_h1_connection *connection, int error_code) {
    bool should_schedule_task = false;
    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        if (!connection->synced_data.is_cross_thread_work_task_scheduled) {
            connection->synced_data.is_cross_thread_work_task_scheduled = true;
            should_schedule_task = true;
        }
        if (!connection->synced_data.shutdown_requested) {
            connection->synced_data.shutdown_requested = true;
            connection->synced_data.shutdown_requested_error_code = error_code;
        }
        /* Connection has shutdown, new streams should not be allowed. */
        connection->synced_data.is_open = false;
        connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (should_schedule_task) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION, "id=%p: Scheduling connection cross-thread work task.", (void *)&connection->base);
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Connection cross-thread work task was already scheduled",
            (void *)&connection->base);
    }
}

/**
 * Public function for closing connection.
 */
static void s_connection_close(struct aws_http_connection *connection_base) {
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);
    s_shutdown_from_off_thread(connection, AWS_ERROR_SUCCESS);
}

static void s_connection_stop_new_request(struct aws_http_connection *connection_base) {
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);

    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        if (!connection->synced_data.new_stream_error_code) {
            connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
        }
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
}

static bool s_connection_is_open(const struct aws_http_connection *connection_base) {
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);
    bool is_open;

    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        is_open = connection->synced_data.is_open;
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    return is_open;
}

static bool s_connection_new_requests_allowed(const struct aws_http_connection *connection_base) {
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);
    int new_stream_error_code;
    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        new_stream_error_code = connection->synced_data.new_stream_error_code;
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    return new_stream_error_code == 0;
}

static int s_stream_send_response(struct aws_http_stream *stream, struct aws_http_message *response) {
    AWS_PRECONDITION(stream);
    AWS_PRECONDITION(response);
    struct aws_h1_stream *h1_stream = AWS_CONTAINER_OF(stream, struct aws_h1_stream, base);
    return aws_h1_stream_send_response(h1_stream, response);
}

/* Calculate the desired window size for connection that has switched protocols and become a midchannel handler. */
static size_t s_calculate_midchannel_desired_connection_window(struct aws_h1_connection *connection) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_ASSERT(connection->thread_data.has_switched_protocols);

    if (!connection->base.channel_slot->adj_right) {
        /* No downstream handler installed. */
        return 0;
    }

    /* Connection is just dumbly forwarding aws_io_messages, so try to match downstream handler. */
    return aws_channel_slot_downstream_read_window(connection->base.channel_slot);
}

/* Calculate the desired window size for a connection that is processing data for aws_http_streams. */
static size_t s_calculate_stream_mode_desired_connection_window(struct aws_h1_connection *connection) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_ASSERT(!connection->thread_data.has_switched_protocols);

    if (!connection->base.stream_manual_window_management) {
        return SIZE_MAX;
    }

    /* Connection window should match the available space in the read-buffer */
    AWS_ASSERT(
        connection->thread_data.read_buffer.pending_bytes <= connection->thread_data.read_buffer.capacity &&
        "This isn't fatal, but our math is off");
    const size_t desired_connection_window = aws_sub_size_saturating(
        connection->thread_data.read_buffer.capacity, connection->thread_data.read_buffer.pending_bytes);

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Window stats: connection=%zu+%zu stream=%" PRIu64 " buffer=%zu/%zu",
        (void *)&connection->base,
        connection->thread_data.connection_window,
        desired_connection_window - connection->thread_data.connection_window /*increment_size*/,
        connection->thread_data.incoming_stream ? connection->thread_data.incoming_stream->thread_data.stream_window
                                                : 0,
        connection->thread_data.read_buffer.pending_bytes,
        connection->thread_data.read_buffer.capacity);

    return desired_connection_window;
}

/* Increment connection window, if necessary */
static int s_update_connection_window(struct aws_h1_connection *connection) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE) {
        return AWS_OP_SUCCESS;
    }

    const size_t desired_size = connection->thread_data.has_switched_protocols
                                    ? s_calculate_midchannel_desired_connection_window(connection)
                                    : s_calculate_stream_mode_desired_connection_window(connection);

    const size_t increment_size = aws_sub_size_saturating(desired_size, connection->thread_data.connection_window);
    if (increment_size > 0) {
        /* Update local `connection_window`. See comments at variable's declaration site
         * on why we use this instead of the official `aws_channel_slot.window_size` */
        connection->thread_data.connection_window += increment_size;
        connection->thread_data.recent_window_increments =
            aws_add_size_saturating(connection->thread_data.recent_window_increments, increment_size);
        if (aws_channel_slot_increment_read_window(connection->base.channel_slot, increment_size)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Failed to increment read window, error %d (%s). Closing connection.",
                (void *)&connection->base,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_h1_stream_activate(struct aws_http_stream *stream) {
    struct aws_h1_stream *h1_stream = AWS_CONTAINER_OF(stream, struct aws_h1_stream, base);

    struct aws_http_connection *base_connection = stream->owning_connection;
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(base_connection, struct aws_h1_connection, base);

    bool should_schedule_task = false;

    { /* BEGIN CRITICAL SECTION */
        /* Note: We're touching both the connection's and stream's synced_data in this section,
         * which is OK because an h1_connection and all its h1_streams share a single lock. */
        aws_h1_connection_lock_synced_data(connection);

        if (stream->id) {
            /* stream has already been activated. */
            aws_h1_connection_unlock_synced_data(connection);
            return AWS_OP_SUCCESS;
        }

        if (connection->synced_data.new_stream_error_code) {
            aws_h1_connection_unlock_synced_data(connection);
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Failed to activate the stream id=%p, new streams are not allowed now. error %d (%s)",
                (void *)&connection->base,
                (void *)stream,
                connection->synced_data.new_stream_error_code,
                aws_error_name(connection->synced_data.new_stream_error_code));
            return aws_raise_error(connection->synced_data.new_stream_error_code);
        }

        stream->id = aws_http_connection_get_next_stream_id(base_connection);
        if (!stream->id) {
            aws_h1_connection_unlock_synced_data(connection);
            /* aws_http_connection_get_next_stream_id() raises its own error. */
            return AWS_OP_ERR;
        }

        /* ID successfully assigned */
        h1_stream->synced_data.api_state = AWS_H1_STREAM_API_STATE_ACTIVE;

        aws_linked_list_push_back(&connection->synced_data.new_client_stream_list, &h1_stream->node);
        if (!connection->synced_data.is_cross_thread_work_task_scheduled) {
            connection->synced_data.is_cross_thread_work_task_scheduled = true;
            should_schedule_task = true;
        }

        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    /* connection keeps activated stream alive until stream completes */
    aws_atomic_fetch_add(&stream->refcount, 1);
    stream->metrics.stream_id = stream->id;

    if (should_schedule_task) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION, "id=%p: Scheduling connection cross-thread work task.", (void *)base_connection);
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Connection cross-thread work task was already scheduled",
            (void *)base_connection);
    }

    return AWS_OP_SUCCESS;
}

void aws_h1_stream_cancel(struct aws_http_stream *stream, int error_code) {
    struct aws_h1_stream *h1_stream = AWS_CONTAINER_OF(stream, struct aws_h1_stream, base);
    struct aws_http_connection *base_connection = stream->owning_connection;
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(base_connection, struct aws_h1_connection, base);

    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        if (h1_stream->synced_data.api_state != AWS_H1_STREAM_API_STATE_ACTIVE ||
            connection->synced_data.is_open == false) {
            /* Not active, nothing to cancel. */
            aws_h1_connection_unlock_synced_data(connection);
            AWS_LOGF_DEBUG(AWS_LS_HTTP_STREAM, "id=%p: Stream not active, nothing to cancel.", (void *)stream);
            return;
        }

        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Connection shutting down due to stream=%p cancelled with error code %d (%s).",
        (void *)&connection->base,
        (void *)stream,
        error_code,
        aws_error_name(error_code));
    s_shutdown_from_off_thread(connection, error_code);
}

struct aws_http_stream *s_make_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options) {
    struct aws_h1_stream *stream = aws_h1_stream_new_request(client_connection, options);
    if (!stream) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Cannot create request stream, error %d (%s)",
            (void *)client_connection,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        return NULL;
    }

    struct aws_h1_connection *connection = AWS_CONTAINER_OF(client_connection, struct aws_h1_connection, base);

    /* Insert new stream into pending list, and schedule outgoing_stream_task if it's not already running. */
    int new_stream_error_code;
    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        new_stream_error_code = connection->synced_data.new_stream_error_code;
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    if (new_stream_error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Cannot create request stream, error %d (%s)",
            (void *)client_connection,
            new_stream_error_code,
            aws_error_name(new_stream_error_code));

        aws_raise_error(new_stream_error_code);
        goto error;
    }

    /* Success! */
    struct aws_byte_cursor method;
    aws_http_message_get_request_method(options->request, &method);
    stream->base.request_method = aws_http_str_to_method(method);
    struct aws_byte_cursor path;
    aws_http_message_get_request_path(options->request, &path);
    AWS_LOGF_DEBUG(
        AWS_LS_HTTP_STREAM,
        "id=%p: Created client request on connection=%p: " PRInSTR " " PRInSTR " " PRInSTR,
        (void *)&stream->base,
        (void *)client_connection,
        AWS_BYTE_CURSOR_PRI(method),
        AWS_BYTE_CURSOR_PRI(path),
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(connection->base.http_version)));

    return &stream->base;

error:
    /* Force destruction of the stream, avoiding ref counting */
    stream->base.vtable->destroy(&stream->base);
    return NULL;
}

/* Extract work items from synced_data, and perform the work on-thread. */
static void s_cross_thread_work_task(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    (void)channel_task;
    struct aws_h1_connection *connection = arg;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION, "id=%p: Running connection cross-thread work task.", (void *)&connection->base);

    /* BEGIN CRITICAL SECTION */
    aws_h1_connection_lock_synced_data(connection);

    connection->synced_data.is_cross_thread_work_task_scheduled = false;

    bool has_new_client_streams = !aws_linked_list_empty(&connection->synced_data.new_client_stream_list);
    aws_linked_list_move_all_back(
        &connection->thread_data.stream_list, &connection->synced_data.new_client_stream_list);
    bool shutdown_requested = connection->synced_data.shutdown_requested;
    int shutdown_error = connection->synced_data.shutdown_requested_error_code;
    connection->synced_data.shutdown_requested = false;
    connection->synced_data.shutdown_requested_error_code = 0;

    aws_h1_connection_unlock_synced_data(connection);
    /* END CRITICAL SECTION */

    if (shutdown_requested) {
        s_stop(connection, true /*stop_reading*/, true /*stop_writing*/, true /*schedule_shutdown*/, shutdown_error);
    }
    /* Kick off outgoing-stream task if necessary */
    if (has_new_client_streams) {
        aws_h1_connection_try_write_outgoing_stream(connection);
    }
}

static bool s_aws_http_stream_was_successful_connect(struct aws_h1_stream *stream) {
    struct aws_http_stream *base = &stream->base;
    if (base->request_method != AWS_HTTP_METHOD_CONNECT) {
        return false;
    }

    if (base->client_data == NULL) {
        return false;
    }

    if (base->client_data->response_status != AWS_HTTP_STATUS_CODE_200_OK) {
        return false;
    }

    return true;
}

/**
 * Validate and perform a protocol switch on a connection.  Protocol switching essentially turns the connection's
 * handler into a dummy pass-through.  It is valid to switch protocols to the same protocol resulting in a channel
 * that has a "dead" http handler in the middle of the channel (which negotiated the CONNECT through the proxy) and
 * a "live" handler on the end which takes the actual http requests.  By doing this, we get the exact same
 * behavior whether we're transitioning to http or any other protocol: once the CONNECT succeeds
 * the first http handler is put in pass-through mode and a new protocol (which could be http) is tacked onto the end.
 */
static int s_aws_http1_switch_protocols(struct aws_h1_connection *connection) {
    AWS_FATAL_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    /* Switching protocols while there are multiple streams is too complex to deal with.
     * Ensure stream_list has exactly this 1 stream in it. */
    if (aws_linked_list_begin(&connection->thread_data.stream_list) !=
        aws_linked_list_rbegin(&connection->thread_data.stream_list)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Cannot switch protocols while further streams are pending, closing connection.",
            (void *)&connection->base);

        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Connection has switched protocols, another channel handler must be installed to"
        " deal with further data.",
        (void *)&connection->base);

    connection->thread_data.has_switched_protocols = true;
    { /* BEGIN CRITICAL SECTION */
        aws_h1_connection_lock_synced_data(connection);
        connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_SWITCHED_PROTOCOLS;
        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    return AWS_OP_SUCCESS;
}

static void s_stream_complete(struct aws_h1_stream *stream, int error_code) {
    struct aws_h1_connection *connection =
        AWS_CONTAINER_OF(stream->base.owning_connection, struct aws_h1_connection, base);
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    /*
     * If this is the end of a successful CONNECT request, mark ourselves as pass-through since the proxy layer
     * will be tacking on a new http handler (and possibly a tls handler in-between).
     */
    if (error_code == AWS_ERROR_SUCCESS && s_aws_http_stream_was_successful_connect(stream)) {
        if (s_aws_http1_switch_protocols(connection)) {
            error_code = AWS_ERROR_HTTP_PROTOCOL_SWITCH_FAILURE;
            s_shutdown_due_to_error(connection, error_code);
        }
    }

    if (stream->base.client_data && stream->base.client_data->response_first_byte_timeout_task.fn != NULL) {
        /* There is an outstanding response timeout task, but stream completed, we can cancel it now. We are
         * safe to do it as we always on connection thread to schedule the task or cancel it */
        struct aws_event_loop *connection_loop = aws_channel_get_event_loop(connection->base.channel_slot->channel);
        /* The task will be zeroed out within the call */
        aws_event_loop_cancel_task(connection_loop, &stream->base.client_data->response_first_byte_timeout_task);
    }

    if (error_code != AWS_ERROR_SUCCESS) {
        if (stream->base.client_data && stream->is_incoming_message_done) {
            /* As a request that finished receiving the response, we ignore error and
             * consider it finished successfully */
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_STREAM,
                "id=%p: Ignoring error code %d (%s). The response has been fully received,"
                "so the stream will complete successfully.",
                (void *)&stream->base,
                error_code,
                aws_error_name(error_code));
            error_code = AWS_ERROR_SUCCESS;
        }
        if (stream->base.server_data && stream->is_outgoing_message_done) {
            /* As a server finished sending the response, but still failed with the request was not finished receiving.
             * We ignore error and consider it finished successfully */
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_STREAM,
                "id=%p: Ignoring error code %d (%s). The response has been fully sent,"
                " so the stream will complete successfully",
                (void *)&stream->base,
                error_code,
                aws_error_name(error_code));
            error_code = AWS_ERROR_SUCCESS;
        }
    }

    /* Remove stream from list. */
    aws_linked_list_remove(&stream->node);

    /* Nice logging */
    if (error_code) {
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Stream completed with error code %d (%s).",
            (void *)&stream->base,
            error_code,
            aws_error_name(error_code));

    } else if (stream->base.client_data) {
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Client request complete, response status: %d (%s).",
            (void *)&stream->base,
            stream->base.client_data->response_status,
            aws_http_status_text(stream->base.client_data->response_status));
    } else {
        AWS_ASSERT(stream->base.server_data);
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_STREAM,
            "id=%p: Server response to " PRInSTR " request complete.",
            (void *)&stream->base,
            AWS_BYTE_CURSOR_PRI(stream->base.server_data->request_method_str));
    }

    /* If connection must shut down, do it BEFORE invoking stream-complete callback.
     * That way, if aws_http_connection_is_open() is called from stream-complete callback, it returns false. */
    if (stream->is_final_stream) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Closing connection due to completion of final stream.",
            (void *)&connection->base);

        s_connection_close(&connection->base);
    }

    { /* BEGIN CRITICAL SECTION */
        /* Note: We're touching the stream's synced_data here, which is OK
         * because an h1_connection and all its h1_streams share a single lock. */
        aws_h1_connection_lock_synced_data(connection);

        /* Mark stream complete */
        stream->synced_data.api_state = AWS_H1_STREAM_API_STATE_COMPLETE;

        /* Move chunks out of synced data */
        aws_linked_list_move_all_back(&stream->thread_data.pending_chunk_list, &stream->synced_data.pending_chunk_list);

        aws_h1_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    /* Complete any leftover chunks */
    while (!aws_linked_list_empty(&stream->thread_data.pending_chunk_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&stream->thread_data.pending_chunk_list);
        struct aws_h1_chunk *chunk = AWS_CONTAINER_OF(node, struct aws_h1_chunk, node);
        aws_h1_chunk_complete_and_destroy(chunk, &stream->base, AWS_ERROR_HTTP_STREAM_HAS_COMPLETED);
    }

    if (stream->base.on_metrics) {
        stream->base.on_metrics(&stream->base, &stream->base.metrics, stream->base.user_data);
    }

    /* Invoke callback and clean up stream. */
    if (stream->base.on_complete) {
        stream->base.on_complete(&stream->base, error_code, stream->base.user_data);
    }

    aws_http_stream_release(&stream->base);
}

static void s_add_time_measurement_to_stats(uint64_t start_ns, uint64_t end_ns, uint64_t *output_ms) {
    if (end_ns > start_ns) {
        *output_ms += aws_timestamp_convert(end_ns - start_ns, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL);
    }
}

static void s_set_outgoing_stream_ptr(
    struct aws_h1_connection *connection,
    struct aws_h1_stream *next_outgoing_stream) {
    struct aws_h1_stream *prev = connection->thread_data.outgoing_stream;

    uint64_t now_ns = 0;
    aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns);
    if (prev == NULL && next_outgoing_stream != NULL) {
        /* transition from nothing to write -> something to write */
        connection->thread_data.outgoing_stream_timestamp_ns = now_ns;
    } else if (prev != NULL && next_outgoing_stream == NULL) {
        /* transition from something to write -> nothing to write */
        s_add_time_measurement_to_stats(
            connection->thread_data.outgoing_stream_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_outgoing_stream_ms);
    }

    connection->thread_data.outgoing_stream = next_outgoing_stream;
}

static void s_set_incoming_stream_ptr(
    struct aws_h1_connection *connection,
    struct aws_h1_stream *next_incoming_stream) {
    struct aws_h1_stream *prev = connection->thread_data.incoming_stream;

    uint64_t now_ns = 0;
    aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns);
    if (prev == NULL && next_incoming_stream != NULL) {
        /* transition from nothing to read -> something to read */
        connection->thread_data.incoming_stream_timestamp_ns = now_ns;
    } else if (prev != NULL && next_incoming_stream == NULL) {
        /* transition from something to read -> nothing to read */
        s_add_time_measurement_to_stats(
            connection->thread_data.incoming_stream_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_incoming_stream_ms);
    }

    connection->thread_data.incoming_stream = next_incoming_stream;
}

/**
 * Ensure `incoming_stream` is pointing at the correct stream, and update state if it changes.
 */
static void s_client_update_incoming_stream_ptr(struct aws_h1_connection *connection) {
    struct aws_linked_list *list = &connection->thread_data.stream_list;
    struct aws_h1_stream *desired;
    if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE) {
        desired = NULL;
    } else if (aws_linked_list_empty(list)) {
        desired = NULL;
    } else {
        desired = AWS_CONTAINER_OF(aws_linked_list_begin(list), struct aws_h1_stream, node);
    }

    if (connection->thread_data.incoming_stream == desired) {
        return;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Current incoming stream is now %p.",
        (void *)&connection->base,
        desired ? (void *)&desired->base : NULL);

    s_set_incoming_stream_ptr(connection, desired);
}

static void s_http_stream_response_first_byte_timeout_task(
    struct aws_task *task,
    void *arg,
    enum aws_task_status status) {
    (void)task;
    struct aws_h1_stream *stream = arg;
    struct aws_http_connection *connection_base = stream->base.owning_connection;
    /* zero-out task to indicate that it's no longer scheduled */
    AWS_ZERO_STRUCT(stream->base.client_data->response_first_byte_timeout_task);

    if (status == AWS_TASK_STATUS_CANCELED) {
        return;
    }

    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);
    /* Timeout happened, close the connection */
    uint64_t response_first_byte_timeout_ms = stream->base.client_data->response_first_byte_timeout_ms == 0
                                                  ? connection_base->client_data->response_first_byte_timeout_ms
                                                  : stream->base.client_data->response_first_byte_timeout_ms;
    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Closing connection. Timed out waiting for first byte of HTTP response, after sending the full request."
        " response_first_byte_timeout_ms=%" PRIu64,
        (void *)connection_base,
        response_first_byte_timeout_ms);

    /* Shutdown the connection. */
    s_shutdown_due_to_error(connection, AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT);
}

static void s_set_outgoing_message_done(struct aws_h1_stream *stream) {
    struct aws_http_connection *connection = stream->base.owning_connection;
    struct aws_channel *channel = aws_http_connection_get_channel(connection);
    AWS_ASSERT(aws_channel_thread_is_callers_thread(channel));

    if (stream->is_outgoing_message_done) {
        /* Already did the job */
        return;
    }

    stream->is_outgoing_message_done = true;
    AWS_ASSERT(stream->base.metrics.send_end_timestamp_ns == -1);
    aws_high_res_clock_get_ticks((uint64_t *)&stream->base.metrics.send_end_timestamp_ns);
    AWS_ASSERT(stream->base.metrics.send_start_timestamp_ns != -1);
    AWS_ASSERT(stream->base.metrics.send_end_timestamp_ns >= stream->base.metrics.send_start_timestamp_ns);
    stream->base.metrics.sending_duration_ns =
        stream->base.metrics.send_end_timestamp_ns - stream->base.metrics.send_start_timestamp_ns;
    if (stream->base.metrics.receive_start_timestamp_ns == -1) {
        /* We haven't receive any message, schedule the response timeout task */

        uint64_t response_first_byte_timeout_ms = 0;
        if (stream->base.client_data != NULL && connection->client_data != NULL) {
            response_first_byte_timeout_ms = stream->base.client_data->response_first_byte_timeout_ms == 0
                                                 ? connection->client_data->response_first_byte_timeout_ms
                                                 : stream->base.client_data->response_first_byte_timeout_ms;
        }
        if (response_first_byte_timeout_ms != 0) {
            /* The task should not be initialized before. */
            AWS_ASSERT(stream->base.client_data->response_first_byte_timeout_task.fn == NULL);
            aws_task_init(
                &stream->base.client_data->response_first_byte_timeout_task,
                s_http_stream_response_first_byte_timeout_task,
                stream,
                "http_stream_response_first_byte_timeout_task");
            uint64_t now_ns = 0;
            aws_channel_current_clock_time(channel, &now_ns);
            struct aws_event_loop *connection_loop = aws_channel_get_event_loop(channel);
            aws_event_loop_schedule_task_future(
                connection_loop,
                &stream->base.client_data->response_first_byte_timeout_task,
                now_ns + aws_timestamp_convert(
                             response_first_byte_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL));
        }
    }
}

/**
 * If necessary, update `outgoing_stream` so it is pointing at a stream
 * with data to send, or NULL if all streams are done sending data.
 *
 * Called from event-loop thread.
 * This function has lots of side effects.
 */
static struct aws_h1_stream *s_update_outgoing_stream_ptr(struct aws_h1_connection *connection) {
    struct aws_h1_stream *current = connection->thread_data.outgoing_stream;
    bool current_changed = false;
    int err;

    /* If current stream is done sending data... */
    if (current && !aws_h1_encoder_is_message_in_progress(&connection->thread_data.encoder)) {
        s_set_outgoing_message_done(current);

        /* RFC-7230 section 6.6: Tear-down.
         * If this was the final stream, don't allows any further streams to be sent */
        if (current->is_final_stream) {
            AWS_LOGF_TRACE(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Done sending final stream, no further streams will be sent.",
                (void *)&connection->base);

            s_stop(
                connection,
                false /*stop_reading*/,
                true /*stop_writing*/,
                false /*schedule_shutdown*/,
                AWS_ERROR_SUCCESS);
        }

        /* If it's also done receiving data, then it's complete! */
        if (current->is_incoming_message_done) {
            /* Only 1st stream in list could finish receiving before it finished sending */
            AWS_ASSERT(&current->node == aws_linked_list_begin(&connection->thread_data.stream_list));

            /* This removes stream from list */
            s_stream_complete(current, AWS_ERROR_SUCCESS);
        }

        current = NULL;
        current_changed = true;
    }

    /* If current stream is NULL, look for more work. */
    if (!current && !connection->thread_data.is_writing_stopped) {

        /* Look for next stream we can work on. */
        for (struct aws_linked_list_node *node = aws_linked_list_begin(&connection->thread_data.stream_list);
             node != aws_linked_list_end(&connection->thread_data.stream_list);
             node = aws_linked_list_next(node)) {

            struct aws_h1_stream *stream = AWS_CONTAINER_OF(node, struct aws_h1_stream, node);

            /* If we already sent this stream's data, keep looking... */
            if (stream->is_outgoing_message_done) {
                continue;
            }

            /* STOP if we're a server, and this stream's response isn't ready to send.
             * It's not like we can skip this and start on the next stream because responses must be sent in order.
             * Don't need a check like this for clients because their streams always start with data to send. */
            if (connection->base.server_data && !stream->thread_data.has_outgoing_response) {
                break;
            }

            /* We found a stream to work on! */
            current = stream;
            current_changed = true;
            break;
        }
    }

    /* Update current incoming and outgoing streams. */
    if (current_changed) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Current outgoing stream is now %p.",
            (void *)&connection->base,
            current ? (void *)&current->base : NULL);

        s_set_outgoing_stream_ptr(connection, current);

        if (current) {
            AWS_ASSERT(current->base.metrics.send_start_timestamp_ns == -1);
            aws_high_res_clock_get_ticks((uint64_t *)&current->base.metrics.send_start_timestamp_ns);

            err = aws_h1_encoder_start_message(
                &connection->thread_data.encoder, &current->encoder_message, &current->base);
            (void)err;
            AWS_ASSERT(connection->thread_data.encoder.state == AWS_H1_ENCODER_STATE_INIT);
            AWS_ASSERT(!err);
        }

        /* incoming_stream update is only for client */
        if (connection->base.client_data) {
            s_client_update_incoming_stream_ptr(connection);
        }
    }

    return current;
}

/* Runs after an aws_io_message containing HTTP has completed (written to the network, or failed).
 * This does NOT run after switching protocols, when we're dumbly forwarding aws_io_messages
 * as a midchannel handler. */
static void s_on_channel_write_complete(
    struct aws_channel *channel,
    struct aws_io_message *message,
    int err_code,
    void *user_data) {

    (void)message;
    struct aws_h1_connection *connection = user_data;
    AWS_ASSERT(connection->thread_data.is_outgoing_stream_task_active);
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (err_code) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Message did not write to network, error %d (%s)",
            (void *)&connection->base,
            err_code,
            aws_error_name(err_code));

        s_shutdown_due_to_error(connection, err_code);
        return;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Message finished writing to network. Rescheduling outgoing stream task.",
        (void *)&connection->base);

    /* To avoid wasting memory, we only want ONE of our written aws_io_messages in the channel at a time.
     * Therefore, we wait until it's written to the network before trying to send another
     * by running the outgoing-stream-task again.
     *
     * We also want to share the network with other channels.
     * Therefore, when the write completes, we SCHEDULE the outgoing-stream-task
     * to run again instead of calling the function directly.
     * This way, if the message completes synchronously,
     * we're not hogging the network by writing message after message in a tight loop */
    aws_channel_schedule_task_now(channel, &connection->outgoing_stream_task);
}

static void s_outgoing_stream_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_h1_connection *connection = arg;
    AWS_ASSERT(connection->thread_data.is_outgoing_stream_task_active);
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    s_write_outgoing_stream(connection, false /*first_try*/);
}

void aws_h1_connection_try_write_outgoing_stream(struct aws_h1_connection *connection) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (connection->thread_data.is_outgoing_stream_task_active) {
        /* Task is already active */
        return;
    }

    connection->thread_data.is_outgoing_stream_task_active = true;
    s_write_outgoing_stream(connection, true /*first_try*/);
}

/* Do the actual work of the outgoing-stream-task */
static void s_write_outgoing_stream(struct aws_h1_connection *connection, bool first_try) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_PRECONDITION(connection->thread_data.is_outgoing_stream_task_active);

    /* Just stop if we're no longer writing stream data */
    if (connection->thread_data.is_writing_stopped || connection->thread_data.has_switched_protocols) {
        return;
    }

    /* Determine whether we have data available to send, and end task immediately if there's not.
     * The outgoing stream task will be kicked off again when user adds more data (new stream, new chunk, etc) */
    struct aws_h1_stream *outgoing_stream = s_update_outgoing_stream_ptr(connection);
    bool waiting_for_chunks = aws_h1_encoder_is_waiting_for_chunks(&connection->thread_data.encoder);
    if (!outgoing_stream || waiting_for_chunks) {
        if (!first_try) {
            AWS_LOGF_TRACE(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Outgoing stream task stopped. outgoing_stream=%p waiting_for_chunks:%d",
                (void *)&connection->base,
                outgoing_stream ? (void *)&outgoing_stream->base : NULL,
                waiting_for_chunks);
        }
        connection->thread_data.is_outgoing_stream_task_active = false;
        return;
    }

    if (first_try) {
        AWS_LOGF_TRACE(AWS_LS_HTTP_CONNECTION, "id=%p: Outgoing stream task has begun.", (void *)&connection->base);
    }

    struct aws_io_message *msg = aws_channel_slot_acquire_max_message_for_write(connection->base.channel_slot);
    if (!msg) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Failed to acquire message from pool, error %d (%s). Closing connection.",
            (void *)&connection->base,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    /* Set up callback so we can send another message when this one completes */
    msg->on_completion = s_on_channel_write_complete;
    msg->user_data = connection;

    /*
     * Fill message data from the outgoing stream.
     * Note that we might be resuming work on a stream from a previous run of this task.
     */
    if (AWS_OP_SUCCESS != aws_h1_encoder_process(&connection->thread_data.encoder, &msg->message_data)) {
        /* Error sending data, abandon ship */
        goto error;
    }

    if (msg->message_data.len > 0) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Outgoing stream task is sending message of size %zu.",
            (void *)&connection->base,
            msg->message_data.len);

        if (aws_channel_slot_send_message(connection->base.channel_slot, msg, AWS_CHANNEL_DIR_WRITE)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Failed to send message in write direction, error %d (%s). Closing connection.",
                (void *)&connection->base,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            goto error;
        }

    } else {
        /* If message is empty, warn that no work is being done
         * and reschedule the task to try again next tick.
         * It's likely that body isn't ready, so body streaming function has no data to write yet.
         * If this scenario turns out to be common we should implement a "pause" feature. */
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Current outgoing stream %p sent no data, will try again next tick.",
            (void *)&connection->base,
            outgoing_stream ? (void *)&outgoing_stream->base : NULL);

        aws_mem_release(msg->allocator, msg);

        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->outgoing_stream_task);
    }

    return;
error:
    if (msg) {
        aws_mem_release(msg->allocator, msg);
    }
    s_shutdown_due_to_error(connection, aws_last_error());
}

static int s_decoder_on_request(
    enum aws_http_method method_enum,
    const struct aws_byte_cursor *method_str,
    const struct aws_byte_cursor *uri,
    void *user_data) {

    struct aws_h1_connection *connection = user_data;
    struct aws_h1_stream *incoming_stream = connection->thread_data.incoming_stream;

    AWS_FATAL_ASSERT(connection->thread_data.incoming_stream->base.server_data); /* Request but I'm a client?!?!? */

    AWS_ASSERT(incoming_stream->base.server_data->request_method_str.len == 0);
    AWS_ASSERT(incoming_stream->base.server_data->request_path.len == 0);

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: Incoming request: method=" PRInSTR " uri=" PRInSTR,
        (void *)&incoming_stream->base,
        AWS_BYTE_CURSOR_PRI(*method_str),
        AWS_BYTE_CURSOR_PRI(*uri));

    /* Copy strings to internal buffer */
    struct aws_byte_buf *storage_buf = &incoming_stream->incoming_storage_buf;
    AWS_ASSERT(storage_buf->capacity == 0);

    size_t storage_size = 0;
    int err = aws_add_size_checked(uri->len, method_str->len, &storage_size);
    if (err) {
        goto error;
    }

    err = aws_byte_buf_init(storage_buf, incoming_stream->base.alloc, storage_size);
    if (err) {
        goto error;
    }

    aws_byte_buf_write_from_whole_cursor(storage_buf, *method_str);
    incoming_stream->base.server_data->request_method_str = aws_byte_cursor_from_buf(storage_buf);

    aws_byte_buf_write_from_whole_cursor(storage_buf, *uri);
    incoming_stream->base.server_data->request_path = aws_byte_cursor_from_buf(storage_buf);
    aws_byte_cursor_advance(&incoming_stream->base.server_data->request_path, storage_buf->len - uri->len);
    incoming_stream->base.request_method = method_enum;

    /* No user callbacks, so we're not checking for shutdown */
    return AWS_OP_SUCCESS;

error:
    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Failed to process new incoming request, error %d (%s).",
        (void *)&connection->base,
        aws_last_error(),
        aws_error_name(aws_last_error()));

    return AWS_OP_ERR;
}

static int s_decoder_on_response(int status_code, void *user_data) {
    struct aws_h1_connection *connection = user_data;

    AWS_FATAL_ASSERT(connection->thread_data.incoming_stream->base.client_data); /* Response but I'm a server?!?!? */

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: Incoming response status: %d (%s).",
        (void *)&connection->thread_data.incoming_stream->base,
        status_code,
        aws_http_status_text(status_code));

    connection->thread_data.incoming_stream->base.client_data->response_status = status_code;

    /* No user callbacks, so we're not checking for shutdown */
    return AWS_OP_SUCCESS;
}

static int s_decoder_on_header(const struct aws_h1_decoded_header *header, void *user_data) {
    struct aws_h1_connection *connection = user_data;
    struct aws_h1_stream *incoming_stream = connection->thread_data.incoming_stream;

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: Incoming header: " PRInSTR ": " PRInSTR,
        (void *)&incoming_stream->base,
        AWS_BYTE_CURSOR_PRI(header->name_data),
        AWS_BYTE_CURSOR_PRI(header->value_data));

    enum aws_http_header_block header_block =
        aws_h1_decoder_get_header_block(connection->thread_data.incoming_stream_decoder);

    /* RFC-7230 section 6.1.
     * "Connection: close" header signals that a connection will not persist after the current request/response */
    if (header->name == AWS_HTTP_HEADER_CONNECTION) {
        /* Certain L7 proxies send a connection close header on a 200/OK response to a CONNECT request. This is nutty
         * behavior, but the obviously desired behavior on a 200 CONNECT response is to leave the connection open
         * for the tunneling. */
        bool ignore_connection_close =
            incoming_stream->base.request_method == AWS_HTTP_METHOD_CONNECT && incoming_stream->base.client_data &&
            incoming_stream->base.client_data->response_status == AWS_HTTP_STATUS_CODE_200_OK;

        if (!ignore_connection_close && aws_byte_cursor_eq_c_str_ignore_case(&header->value_data, "close")) {
            AWS_LOGF_TRACE(
                AWS_LS_HTTP_STREAM,
                "id=%p: Received 'Connection: close' header. This will be the final stream on this connection.",
                (void *)&incoming_stream->base);

            incoming_stream->is_final_stream = true;
            { /* BEGIN CRITICAL SECTION */
                aws_h1_connection_lock_synced_data(connection);
                connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
                aws_h1_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */

            if (connection->base.client_data) {
                /**
                 * RFC-9112 section 9.6.
                 * A client that receives a "close" connection option MUST cease sending
                 * requests on that connection and close the connection after reading the
                 * response message containing the "close" connection option.
                 *
                 * Mark the stream's outgoing message as complete,
                 * so that we stop sending, and stop waiting for it to finish sending.
                 **/
                if (!incoming_stream->is_outgoing_message_done) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_HTTP_STREAM,
                        "id=%p: Received 'Connection: close' header, no more request data will be sent.",
                        (void *)&incoming_stream->base);
                    s_set_outgoing_message_done(incoming_stream);
                }
                /* Stop writing right now.
                 * Shutdown will be scheduled after we finishing parsing the response */
                s_stop(
                    connection,
                    false /*stop_reading*/,
                    true /*stop_writing*/,
                    false /*schedule_shutdown*/,
                    AWS_ERROR_SUCCESS);
            }
        }
    }

    if (incoming_stream->base.on_incoming_headers) {
        struct aws_http_header deliver = {
            .name = header->name_data,
            .value = header->value_data,
        };

        int err = incoming_stream->base.on_incoming_headers(
            &incoming_stream->base, header_block, &deliver, 1, incoming_stream->base.user_data);

        if (err) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Incoming header callback raised error %d (%s).",
                (void *)&incoming_stream->base,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_mark_head_done(struct aws_h1_stream *incoming_stream) {
    /* Bail out if we've already done this */
    if (incoming_stream->is_incoming_head_done) {
        return AWS_OP_SUCCESS;
    }

    struct aws_h1_connection *connection =
        AWS_CONTAINER_OF(incoming_stream->base.owning_connection, struct aws_h1_connection, base);

    enum aws_http_header_block header_block =
        aws_h1_decoder_get_header_block(connection->thread_data.incoming_stream_decoder);

    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Main header block done.", (void *)&incoming_stream->base);
        incoming_stream->is_incoming_head_done = true;

    } else if (header_block == AWS_HTTP_HEADER_BLOCK_INFORMATIONAL) {
        AWS_LOGF_TRACE(AWS_LS_HTTP_STREAM, "id=%p: Informational header block done.", (void *)&incoming_stream->base);

        /* Only clients can receive informational headers.
         * Check whether we're switching protocols */
        if (incoming_stream->base.client_data->response_status == AWS_HTTP_STATUS_CODE_101_SWITCHING_PROTOCOLS) {
            if (s_aws_http1_switch_protocols(connection)) {
                return AWS_OP_ERR;
            }
        }
    }

    /* Invoke user cb */
    if (incoming_stream->base.on_incoming_header_block_done) {
        int err = incoming_stream->base.on_incoming_header_block_done(
            &incoming_stream->base, header_block, incoming_stream->base.user_data);
        if (err) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Incoming-header-block-done callback raised error %d (%s).",
                (void *)&incoming_stream->base,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_body(const struct aws_byte_cursor *data, bool finished, void *user_data) {
    (void)finished;

    struct aws_h1_connection *connection = user_data;
    struct aws_h1_stream *incoming_stream = connection->thread_data.incoming_stream;
    AWS_ASSERT(incoming_stream);

    int err = s_mark_head_done(incoming_stream);
    if (err) {
        return AWS_OP_ERR;
    }

    /* No need to invoke callback for 0-length data */
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM, "id=%p: Incoming body: %zu bytes received.", (void *)&incoming_stream->base, data->len);

    if (connection->base.stream_manual_window_management) {
        /* Let stream window shrink by amount of body data received */
        if (data->len > incoming_stream->thread_data.stream_window) {
            /* This error shouldn't be possible, but it's all complicated, so do runtime check to be safe. */
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Internal error. Data exceeds HTTP-stream's window.",
                (void *)&incoming_stream->base);
            return aws_raise_error(AWS_ERROR_INVALID_STATE);
        }
        incoming_stream->thread_data.stream_window -= data->len;

        if (incoming_stream->thread_data.stream_window == 0) {
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_STREAM,
                "id=%p: Flow-control window has reached 0. No more data can be received until window is updated.",
                (void *)&incoming_stream->base);
        }
    }

    if (incoming_stream->base.on_incoming_body) {
        err = incoming_stream->base.on_incoming_body(&incoming_stream->base, data, incoming_stream->base.user_data);
        if (err) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_STREAM,
                "id=%p: Incoming body callback raised error %d (%s).",
                (void *)&incoming_stream->base,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_decoder_on_done(void *user_data) {
    struct aws_h1_connection *connection = user_data;
    struct aws_h1_stream *incoming_stream = connection->thread_data.incoming_stream;
    AWS_ASSERT(incoming_stream);

    /* Ensure head was marked done */
    int err = s_mark_head_done(incoming_stream);
    if (err) {
        return AWS_OP_ERR;
    }
    /* If it is a informational response, we stop here, keep waiting for new response */
    enum aws_http_header_block header_block =
        aws_h1_decoder_get_header_block(connection->thread_data.incoming_stream_decoder);
    if (header_block == AWS_HTTP_HEADER_BLOCK_INFORMATIONAL) {
        return AWS_OP_SUCCESS;
    }

    /* Otherwise the incoming stream is finished decoding and we will update it if needed */
    incoming_stream->is_incoming_message_done = true;
    aws_high_res_clock_get_ticks((uint64_t *)&incoming_stream->base.metrics.receive_end_timestamp_ns);
    AWS_ASSERT(incoming_stream->base.metrics.receive_start_timestamp_ns != -1);
    AWS_ASSERT(
        incoming_stream->base.metrics.receive_end_timestamp_ns >=
        incoming_stream->base.metrics.receive_start_timestamp_ns);
    incoming_stream->base.metrics.receiving_duration_ns = incoming_stream->base.metrics.receive_end_timestamp_ns -
                                                          incoming_stream->base.metrics.receive_start_timestamp_ns;

    /* RFC-7230 section 6.6
     * After reading the final message, the connection must not read any more */
    if (incoming_stream->is_final_stream) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Done reading final stream, no further streams will be read.",
            (void *)&connection->base);

        s_stop(
            connection, true /*stop_reading*/, false /*stop_writing*/, false /*schedule_shutdown*/, AWS_ERROR_SUCCESS);
    }

    if (connection->base.server_data) {
        /* Server side */
        aws_http_on_incoming_request_done_fn *on_request_done = incoming_stream->base.server_data->on_request_done;
        if (on_request_done) {
            err = on_request_done(&incoming_stream->base, incoming_stream->base.user_data);
            if (err) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_STREAM,
                    "id=%p: Incoming request done callback raised error %d (%s).",
                    (void *)&incoming_stream->base,
                    aws_last_error(),
                    aws_error_name(aws_last_error()));
                return AWS_OP_ERR;
            }
        }
        if (incoming_stream->is_outgoing_message_done) {
            AWS_ASSERT(&incoming_stream->node == aws_linked_list_begin(&connection->thread_data.stream_list));
            s_stream_complete(incoming_stream, AWS_ERROR_SUCCESS);
        }
        s_set_incoming_stream_ptr(connection, NULL);

    } else if (incoming_stream->is_outgoing_message_done) {
        /* Client side */
        AWS_ASSERT(&incoming_stream->node == aws_linked_list_begin(&connection->thread_data.stream_list));

        s_stream_complete(incoming_stream, AWS_ERROR_SUCCESS);

        s_client_update_incoming_stream_ptr(connection);
    }

    /* Report success even if user's on_complete() callback shuts down on the connection.
     * We don't want it to look like something went wrong while decoding.
     * The decode() function returns after each message completes,
     * and we won't call decode() again if the connection has been shut down */
    return AWS_OP_SUCCESS;
}

/* Common new() logic for server & client */
static struct aws_h1_connection *s_connection_new(
    struct aws_allocator *alloc,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http1_connection_options *http1_options,
    bool server) {

    struct aws_h1_connection *connection = aws_mem_calloc(alloc, 1, sizeof(struct aws_h1_connection));
    if (!connection) {
        goto error_connection_alloc;
    }

    connection->base.vtable = &s_h1_connection_vtable;
    connection->base.alloc = alloc;
    connection->base.channel_handler.vtable = &s_h1_connection_vtable.channel_handler_vtable;
    connection->base.channel_handler.alloc = alloc;
    connection->base.channel_handler.impl = connection;
    connection->base.http_version = AWS_HTTP_VERSION_1_1;
    connection->base.stream_manual_window_management = manual_window_management;

    /* Init the next stream id (server must use even ids, client odd [RFC 7540 5.1.1])*/
    connection->base.next_stream_id = server ? 2 : 1;

    /* 1 refcount for user */
    aws_atomic_init_int(&connection->base.refcount, 1);

    if (manual_window_management) {
        connection->initial_stream_window_size = initial_window_size;

        if (http1_options->read_buffer_capacity > 0) {
            connection->thread_data.read_buffer.capacity = http1_options->read_buffer_capacity;
        } else {
            /* User did not set capacity, choose something reasonable based on initial_window_size */
            /* NOTE: These values are currently guesses, we should test to find good values */
            const size_t clamp_min = aws_min_size(g_aws_channel_max_fragment_size * 4, /*256KB*/ 256 * 1024);
            const size_t clamp_max = /*1MB*/ 1 * 1024 * 1024;
            connection->thread_data.read_buffer.capacity =
                aws_max_size(clamp_min, aws_min_size(clamp_max, initial_window_size));
        }

        connection->thread_data.connection_window = connection->thread_data.read_buffer.capacity;
    } else {
        /* No backpressure, keep connection window at SIZE_MAX */
        connection->initial_stream_window_size = SIZE_MAX;
        connection->thread_data.read_buffer.capacity = SIZE_MAX;
        connection->thread_data.connection_window = SIZE_MAX;
    }

    aws_h1_encoder_init(&connection->thread_data.encoder, alloc);

    aws_channel_task_init(
        &connection->outgoing_stream_task, s_outgoing_stream_task, connection, "http1_connection_outgoing_stream");
    aws_channel_task_init(
        &connection->cross_thread_work_task,
        s_cross_thread_work_task,
        connection,
        "http1_connection_cross_thread_work");
    aws_linked_list_init(&connection->thread_data.stream_list);
    aws_linked_list_init(&connection->thread_data.read_buffer.messages);
    aws_crt_statistics_http1_channel_init(&connection->thread_data.stats);

    int err = aws_mutex_init(&connection->synced_data.lock);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to initialize mutex, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error_mutex;
    }

    aws_linked_list_init(&connection->synced_data.new_client_stream_list);
    connection->synced_data.is_open = true;

    struct aws_h1_decoder_params options = {
        .alloc = alloc,
        .is_decoding_requests = server,
        .user_data = connection,
        .vtable = s_h1_decoder_vtable,
        .scratch_space_initial_size = DECODER_INITIAL_SCRATCH_SIZE,
    };
    connection->thread_data.incoming_stream_decoder = aws_h1_decoder_new(&options);
    if (!connection->thread_data.incoming_stream_decoder) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create decoder, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error_decoder;
    }

    return connection;

error_decoder:
    aws_mutex_clean_up(&connection->synced_data.lock);
error_mutex:
    aws_mem_release(alloc, connection);
error_connection_alloc:
    return NULL;
}

struct aws_http_connection *aws_http_connection_new_http1_1_server(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http1_connection_options *http1_options) {

    struct aws_h1_connection *connection =
        s_connection_new(allocator, manual_window_management, initial_window_size, http1_options, true /*is_server*/);
    if (!connection) {
        return NULL;
    }

    connection->base.server_data = &connection->base.client_or_server_data.server;

    return &connection->base;
}

struct aws_http_connection *aws_http_connection_new_http1_1_client(
    struct aws_allocator *allocator,
    bool manual_window_management,
    size_t initial_window_size,
    const struct aws_http1_connection_options *http1_options) {

    struct aws_h1_connection *connection =
        s_connection_new(allocator, manual_window_management, initial_window_size, http1_options, false /*is_server*/);
    if (!connection) {
        return NULL;
    }

    connection->base.client_data = &connection->base.client_or_server_data.client;

    return &connection->base;
}

static void s_handler_destroy(struct aws_channel_handler *handler) {
    struct aws_h1_connection *connection = handler->impl;

    AWS_LOGF_TRACE(AWS_LS_HTTP_CONNECTION, "id=%p: Destroying connection.", (void *)&connection->base);

    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.stream_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.new_client_stream_list));

    /* Clean up any buffered read messages. */
    while (!aws_linked_list_empty(&connection->thread_data.read_buffer.messages)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->thread_data.read_buffer.messages);
        struct aws_io_message *msg = AWS_CONTAINER_OF(node, struct aws_io_message, queueing_handle);
        aws_mem_release(msg->allocator, msg);
    }

    aws_h1_decoder_destroy(connection->thread_data.incoming_stream_decoder);
    aws_h1_encoder_clean_up(&connection->thread_data.encoder);
    aws_mutex_clean_up(&connection->synced_data.lock);
    aws_mem_release(connection->base.alloc, connection);
}

static void s_handler_installed(struct aws_channel_handler *handler, struct aws_channel_slot *slot) {
    struct aws_h1_connection *connection = handler->impl;
    connection->base.channel_slot = slot;

    /* Acquire a hold on the channel to prevent its destruction until the user has
     * given the go-ahead via aws_http_connection_release() */
    aws_channel_acquire_hold(slot->channel);
}

/* Try to send the next queued aws_io_message to the downstream handler.
 * This can only be called after the connection has switched protocols and becoming a midchannel handler. */
static int s_try_process_next_midchannel_read_message(struct aws_h1_connection *connection, bool *out_stop_processing) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_ASSERT(connection->thread_data.has_switched_protocols);
    AWS_ASSERT(connection->thread_data.read_state != AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE);
    AWS_ASSERT(!aws_linked_list_empty(&connection->thread_data.read_buffer.messages));

    *out_stop_processing = false;
    struct aws_io_message *sending_msg = NULL;

    if (!connection->base.channel_slot->adj_right) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Connection has switched protocols, but no handler is installed to deal with this data.",
            (void *)connection);

        return aws_raise_error(AWS_ERROR_HTTP_SWITCHED_PROTOCOLS);
    }

    size_t downstream_window = aws_channel_slot_downstream_read_window(connection->base.channel_slot);
    if (downstream_window == 0) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Downstream window is 0, cannot send switched-protocol message now.",
            (void *)&connection->base);

        *out_stop_processing = true;
        return AWS_OP_SUCCESS;
    }

    struct aws_linked_list_node *queued_msg_node = aws_linked_list_front(&connection->thread_data.read_buffer.messages);
    struct aws_io_message *queued_msg = AWS_CONTAINER_OF(queued_msg_node, struct aws_io_message, queueing_handle);

    /* Note that copy_mark is used to mark the progress of partially sent messages. */
    AWS_ASSERT(queued_msg->message_data.len > queued_msg->copy_mark);
    size_t sending_bytes = aws_min_size(queued_msg->message_data.len - queued_msg->copy_mark, downstream_window);

    AWS_ASSERT(connection->thread_data.read_buffer.pending_bytes >= sending_bytes);
    connection->thread_data.read_buffer.pending_bytes -= sending_bytes;

    /* If we can't send the whole entire queued_msg, copy its data into a new aws_io_message and send that. */
    if (sending_bytes != queued_msg->message_data.len) {
        sending_msg = aws_channel_acquire_message_from_pool(
            connection->base.channel_slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, sending_bytes);
        if (!sending_msg) {
            goto error;
        }

        aws_byte_buf_write(
            &sending_msg->message_data, queued_msg->message_data.buffer + queued_msg->copy_mark, sending_bytes);

        queued_msg->copy_mark += sending_bytes;

        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Sending %zu bytes switched-protocol message to downstream handler, %zu bytes remain.",
            (void *)&connection->base,
            sending_bytes,
            queued_msg->message_data.len - queued_msg->copy_mark);

        /* If the last of queued_msg has been copied, it can be deleted now. */
        if (queued_msg->copy_mark == queued_msg->message_data.len) {
            aws_linked_list_remove(queued_msg_node);
            aws_mem_release(queued_msg->allocator, queued_msg);
        }
    } else {
        /* Sending all of queued_msg along. */
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Sending full switched-protocol message of size %zu to downstream handler.",
            (void *)&connection->base,
            queued_msg->message_data.len);

        aws_linked_list_remove(queued_msg_node);
        sending_msg = queued_msg;
    }

    int err = aws_channel_slot_send_message(connection->base.channel_slot, sending_msg, AWS_CHANNEL_DIR_READ);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Failed to send message in read direction, error %d (%s).",
            (void *)&connection->base,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    if (sending_msg) {
        aws_mem_release(sending_msg->allocator, sending_msg);
    }
    return AWS_OP_ERR;
}

static struct aws_http_stream *s_new_server_request_handler_stream(
    const struct aws_http_request_handler_options *options) {

    struct aws_h1_connection *connection = AWS_CONTAINER_OF(options->server_connection, struct aws_h1_connection, base);

    if (!aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel) ||
        !connection->thread_data.can_create_request_handler_stream) {

        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: aws_http_stream_new_server_request_handler() can only be called during incoming request callback.",
            (void *)&connection->base);

        aws_raise_error(AWS_ERROR_INVALID_STATE);
        return NULL;
    }

    struct aws_h1_stream *stream = aws_h1_stream_new_request_handler(options);
    if (!stream) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Failed to create request handler stream, error %d (%s).",
            (void *)&connection->base,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        return NULL;
    }

    /*
     * Success!
     * Everything beyond this point cannot fail
     */

    /* Prevent further streams from being created until it's ok to do so. */
    connection->thread_data.can_create_request_handler_stream = false;

    /* Stream is waiting for response. */
    aws_linked_list_push_back(&connection->thread_data.stream_list, &stream->node);

    /* Connection owns stream, and must outlive stream */
    aws_http_connection_acquire(&connection->base);

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_STREAM,
        "id=%p: Created request handler stream on server connection=%p",
        (void *)&stream->base,
        (void *)&connection->base);

    return &stream->base;
}

/* Invokes the on_incoming_request callback and returns new stream. */
static struct aws_h1_stream *s_server_invoke_on_incoming_request(struct aws_h1_connection *connection) {
    AWS_PRECONDITION(connection->base.server_data);
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_PRECONDITION(!connection->thread_data.can_create_request_handler_stream);
    AWS_PRECONDITION(!connection->thread_data.incoming_stream);

    /**
     * The user MUST create the new request-handler stream during the on-incoming-request callback.
     */
    connection->thread_data.can_create_request_handler_stream = true;

    struct aws_http_stream *new_stream =
        connection->base.server_data->on_incoming_request(&connection->base, connection->base.user_data);

    connection->thread_data.can_create_request_handler_stream = false;

    return new_stream ? AWS_CONTAINER_OF(new_stream, struct aws_h1_stream, base) : NULL;
}

static int s_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    (void)slot;
    struct aws_h1_connection *connection = handler->impl;
    const size_t message_size = message->message_data.len;

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION, "id=%p: Incoming message of size %zu.", (void *)&connection->base, message_size);
    if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE) {
        /* Read has stopped, ignore the data, shutdown the channel incase it has not started yet. */
        aws_mem_release(message->allocator, message); /* Release the message as we return success. */
        s_shutdown_due_to_error(connection, AWS_ERROR_HTTP_CONNECTION_CLOSED);
        return AWS_OP_SUCCESS;
    }

    /* Shrink connection window by amount of data received. See comments at variable's
     * declaration site on why we use this instead of the official `aws_channel_slot.window_size`. */
    if (message_size > connection->thread_data.connection_window) {
        /* This error shouldn't be possible, but this is all complicated so check at runtime to be safe. */
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Internal error. Message exceeds connection's window.",
            (void *)&connection->base);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    connection->thread_data.connection_window -= message_size;

    /* Push message into queue of buffered messages */
    aws_linked_list_push_back(&connection->thread_data.read_buffer.messages, &message->queueing_handle);
    connection->thread_data.read_buffer.pending_bytes += message_size;

    /* Try to process messages in queue */
    aws_h1_connection_try_process_read_messages(connection);
    return AWS_OP_SUCCESS;
}

void aws_h1_connection_try_process_read_messages(struct aws_h1_connection *connection) {
    int error_code = 0;
    /* Protect against this function being called recursively. */
    if (connection->thread_data.is_processing_read_messages) {
        return;
    }
    connection->thread_data.is_processing_read_messages = true;

    /* Process queued messages */
    while (!aws_linked_list_empty(&connection->thread_data.read_buffer.messages)) {
        if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Cannot process message because connection is shutting down.",
                (void *)&connection->base);

            aws_raise_error(AWS_ERROR_HTTP_CONNECTION_CLOSED);
            goto shutdown;
        }

        bool stop_processing = false;

        /* When connection has switched protocols, messages are processed very differently.
         * We need to do this check in the middle of the normal processing loop,
         * in case the switch happens in the middle of processing a message. */
        if (connection->thread_data.has_switched_protocols) {
            if (s_try_process_next_midchannel_read_message(connection, &stop_processing)) {
                goto shutdown;
            }
        } else {
            if (s_try_process_next_stream_read_message(connection, &stop_processing)) {
                goto shutdown;
            }
        }

        /* Break out of loop if we can't process any more data */
        if (stop_processing) {
            break;
        }
    }

    if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUTTING_DOWN &&
        connection->thread_data.read_buffer.pending_bytes == 0) {
        /* Done processing the pending buffer. */
        aws_raise_error(connection->thread_data.pending_shutdown_error_code);
        goto shutdown;
    }

    /* Increment connection window, if necessary */
    if (s_update_connection_window(connection)) {
        goto shutdown;
    }

    connection->thread_data.is_processing_read_messages = false;
    return;

shutdown:
    error_code = aws_last_error();
    if (connection->thread_data.read_state == AWS_CONNECTION_READ_SHUTTING_DOWN &&
        connection->thread_data.pending_shutdown_error_code != 0) {
        error_code = connection->thread_data.pending_shutdown_error_code;
    }
    if (error_code == 0) {
        /* Graceful shutdown, don't stop writing yet. */
        s_stop(connection, true /*stop_reading*/, false /*stop_writing*/, true /*schedule_shutdown*/, error_code);
    } else {
        s_shutdown_due_to_error(connection, aws_last_error());
    }
}

/* Try to process the next queued aws_io_message as normal HTTP data for an aws_http_stream.
 * This MUST NOT be called if the connection has switched protocols and become a midchannel handler. */
static int s_try_process_next_stream_read_message(struct aws_h1_connection *connection, bool *out_stop_processing) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_ASSERT(!connection->thread_data.has_switched_protocols);
    AWS_ASSERT(connection->thread_data.read_state != AWS_CONNECTION_READ_SHUT_DOWN_COMPLETE);
    AWS_ASSERT(!aws_linked_list_empty(&connection->thread_data.read_buffer.messages));

    *out_stop_processing = false;

    /* Ensure that an incoming stream exists to receive the data */
    if (!connection->thread_data.incoming_stream) {
        if (aws_http_connection_is_client(&connection->base)) {
            /* Client side */
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "id=%p: Cannot process message because no requests are currently awaiting response, closing "
                "connection.",
                (void *)&connection->base);

            return aws_raise_error(AWS_ERROR_INVALID_STATE);

        } else {
            /* Server side.
             * Invoke on-incoming-request callback. The user MUST create a new stream from this callback.
             * The new stream becomes the current incoming stream */
            s_set_incoming_stream_ptr(connection, s_server_invoke_on_incoming_request(connection));
            if (!connection->thread_data.incoming_stream) {
                AWS_LOGF_ERROR(
                    AWS_LS_HTTP_CONNECTION,
                    "id=%p: Incoming request callback failed to provide a new stream, last error %d (%s). "
                    "Closing connection.",
                    (void *)&connection->base,
                    aws_last_error(),
                    aws_error_name(aws_last_error()));

                return AWS_OP_ERR;
            }
        }
    }

    struct aws_h1_stream *incoming_stream = connection->thread_data.incoming_stream;

    /* Stop processing if stream's window reaches 0. */
    const uint64_t stream_window = incoming_stream->thread_data.stream_window;
    if (stream_window == 0) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: HTTP-stream's window is 0, cannot process message now.",
            (void *)&connection->base);
        *out_stop_processing = true;
        return AWS_OP_SUCCESS;
    }

    struct aws_linked_list_node *queued_msg_node = aws_linked_list_front(&connection->thread_data.read_buffer.messages);
    struct aws_io_message *queued_msg = AWS_CONTAINER_OF(queued_msg_node, struct aws_io_message, queueing_handle);

    /* Note that copy_mark is used to mark the progress of partially decoded messages */
    struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&queued_msg->message_data);
    aws_byte_cursor_advance(&message_cursor, queued_msg->copy_mark);

    /* Don't process more data than the stream's window can accept.
     *
     * TODO: Let the decoder know about stream-window size so it can stop itself,
     * instead of limiting the amount of data we feed into the decoder at a time.
     * This would be more optimal, AND avoid an edge-case where the stream-window goes
     * to 0 as the body ends, and the connection can't proceed to the trailing headers.
     */
    message_cursor.len = (size_t)aws_min_u64(message_cursor.len, stream_window);

    const size_t prev_cursor_len = message_cursor.len;

    /* Set some decoder state, based on current stream */
    aws_h1_decoder_set_logging_id(connection->thread_data.incoming_stream_decoder, incoming_stream);

    bool body_headers_ignored = incoming_stream->base.request_method == AWS_HTTP_METHOD_HEAD;
    aws_h1_decoder_set_body_headers_ignored(connection->thread_data.incoming_stream_decoder, body_headers_ignored);

    if (incoming_stream->base.metrics.receive_start_timestamp_ns == -1) {
        /* That's the first time for the stream receives any message */
        aws_high_res_clock_get_ticks((uint64_t *)&incoming_stream->base.metrics.receive_start_timestamp_ns);
        if (incoming_stream->base.client_data &&
            incoming_stream->base.client_data->response_first_byte_timeout_task.fn != NULL) {
            /* There is an outstanding response timeout task, as we already received the data, we can cancel it now. We
             * are safe to do it as we always on connection thread to schedule the task or cancel it */
            struct aws_event_loop *connection_loop = aws_channel_get_event_loop(connection->base.channel_slot->channel);
            /* The task will be zeroed out within the call */
            aws_event_loop_cancel_task(
                connection_loop, &incoming_stream->base.client_data->response_first_byte_timeout_task);
        }
    }

    /* As decoder runs, it invokes the internal s_decoder_X callbacks, which in turn invoke user callbacks.
     * The decoder will stop once it hits the end of the request/response OR the end of the message data. */
    if (aws_h1_decode(connection->thread_data.incoming_stream_decoder, &message_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Message processing failed, error %d (%s). Closing connection.",
            (void *)&connection->base,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        return AWS_OP_ERR;
    }

    size_t bytes_processed = prev_cursor_len - message_cursor.len;
    queued_msg->copy_mark += bytes_processed;

    AWS_ASSERT(connection->thread_data.read_buffer.pending_bytes >= bytes_processed);
    connection->thread_data.read_buffer.pending_bytes -= bytes_processed;

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Decoded %zu bytes of message, %zu bytes remain.",
        (void *)&connection->base,
        bytes_processed,
        queued_msg->message_data.len - queued_msg->copy_mark);

    /* If the last of queued_msg has been processed, it can be deleted now.
     * Otherwise, it remains in the queue for further processing later. */
    if (queued_msg->copy_mark == queued_msg->message_data.len) {
        aws_linked_list_remove(&queued_msg->queueing_handle);
        aws_mem_release(queued_msg->allocator, queued_msg);
    }

    return AWS_OP_SUCCESS;
}

static int s_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    struct aws_h1_connection *connection = handler->impl;

    if (connection->thread_data.is_writing_stopped) {
        aws_raise_error(AWS_ERROR_HTTP_CONNECTION_CLOSED);
        goto error;
    }

    if (!connection->thread_data.has_switched_protocols) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    /* Pass the message right along. */
    int err = aws_channel_slot_send_message(slot, message, AWS_CHANNEL_DIR_WRITE);
    if (err) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Destroying write message without passing it along, error %d (%s)",
        (void *)&connection->base,
        aws_last_error(),
        aws_error_name(aws_last_error()));

    if (message->on_completion) {
        message->on_completion(connection->base.channel_slot->channel, message, aws_last_error(), message->user_data);
    }
    aws_mem_release(message->allocator, message);
    s_shutdown_due_to_error(connection, aws_last_error());
    return AWS_OP_SUCCESS;
}

static int s_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {

    (void)slot;
    struct aws_h1_connection *connection = handler->impl;

    if (!connection->thread_data.has_switched_protocols) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: HTTP connection cannot have a downstream handler without first switching protocols",
            (void *)&connection->base);

        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Handler in read direction incremented read window by %zu. Sending queued messages, if any.",
        (void *)&connection->base,
        size);

    /* Send along any queued messages, and increment connection's window if necessary */
    aws_h1_connection_try_process_read_messages(connection);
    return AWS_OP_SUCCESS;

error:
    s_shutdown_due_to_error(connection, aws_last_error());
    return AWS_OP_SUCCESS;
}

static void s_initialize_read_delay_shutdown(struct aws_h1_connection *connection, int error_code) {

    AWS_LOGF_DEBUG(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Connection still have pending data to be delivered during shutdown. Wait until downstream "
        "reads the data.",
        (void *)&connection->base);

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Current window stats: connection=%zu, stream=%" PRIu64 " buffer=%zu/%zu",
        (void *)&connection->base,
        connection->thread_data.connection_window,
        connection->thread_data.incoming_stream ? connection->thread_data.incoming_stream->thread_data.stream_window
                                                : 0,
        connection->thread_data.read_buffer.pending_bytes,
        connection->thread_data.read_buffer.capacity);

    /* Still have data buffered in connection, wait for it to be processed */
    connection->thread_data.read_state = AWS_CONNECTION_READ_SHUTTING_DOWN;
    connection->thread_data.pending_shutdown_error_code = error_code;
    /* Try to process messages in queue */
    aws_h1_connection_try_process_read_messages(connection);
}

static int s_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {

    (void)free_scarce_resources_immediately;
    struct aws_h1_connection *connection = handler->impl;

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: Channel shutting down in %s direction with error code %d (%s).",
        (void *)&connection->base,
        (dir == AWS_CHANNEL_DIR_READ) ? "read" : "write",
        error_code,
        aws_error_name(error_code));

    if (dir == AWS_CHANNEL_DIR_READ) {
        /* This call ensures that no further streams will be created or worked on. */
        if (!free_scarce_resources_immediately && connection->thread_data.read_state == AWS_CONNECTION_READ_OPEN &&
            connection->thread_data.read_buffer.pending_bytes > 0) {
            s_initialize_read_delay_shutdown(connection, error_code);
            /* Return success, and wait for the buffered data to be processed to propagate the shutdown. */
            return AWS_OP_SUCCESS;
        }
        s_stop(connection, true /*stop_reading*/, false /*stop_writing*/, false /*schedule_shutdown*/, error_code);
    } else /* dir == AWS_CHANNEL_DIR_WRITE */ {

        s_stop(connection, false /*stop_reading*/, true /*stop_writing*/, false /*schedule_shutdown*/, error_code);

        /* Mark all pending streams as complete. */
        int stream_error_code = error_code == AWS_ERROR_SUCCESS ? AWS_ERROR_HTTP_CONNECTION_CLOSED : error_code;

        while (!aws_linked_list_empty(&connection->thread_data.stream_list)) {
            struct aws_linked_list_node *node = aws_linked_list_front(&connection->thread_data.stream_list);
            s_stream_complete(AWS_CONTAINER_OF(node, struct aws_h1_stream, node), stream_error_code);
        }

        /* It's OK to access synced_data.new_client_stream_list without holding the lock because
         * no more streams can be added after s_stop() has been invoked. */
        while (!aws_linked_list_empty(&connection->synced_data.new_client_stream_list)) {
            struct aws_linked_list_node *node = aws_linked_list_front(&connection->synced_data.new_client_stream_list);
            s_stream_complete(AWS_CONTAINER_OF(node, struct aws_h1_stream, node), stream_error_code);
        }
    }

    aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resources_immediately);
    return AWS_OP_SUCCESS;
}

static size_t s_handler_initial_window_size(struct aws_channel_handler *handler) {
    struct aws_h1_connection *connection = handler->impl;
    return connection->thread_data.connection_window;
}

static size_t s_handler_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return 0;
}

static void s_reset_statistics(struct aws_channel_handler *handler) {
    struct aws_h1_connection *connection = handler->impl;

    aws_crt_statistics_http1_channel_reset(&connection->thread_data.stats);
}

static void s_pull_up_stats_timestamps(struct aws_h1_connection *connection) {
    uint64_t now_ns = 0;
    if (aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns)) {
        return;
    }

    if (connection->thread_data.outgoing_stream) {
        s_add_time_measurement_to_stats(
            connection->thread_data.outgoing_stream_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_outgoing_stream_ms);

        connection->thread_data.outgoing_stream_timestamp_ns = now_ns;

        connection->thread_data.stats.current_outgoing_stream_id =
            aws_http_stream_get_id(&connection->thread_data.outgoing_stream->base);
    }

    if (connection->thread_data.incoming_stream) {
        s_add_time_measurement_to_stats(
            connection->thread_data.incoming_stream_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_incoming_stream_ms);

        connection->thread_data.incoming_stream_timestamp_ns = now_ns;

        connection->thread_data.stats.current_incoming_stream_id =
            aws_http_stream_get_id(&connection->thread_data.incoming_stream->base);
    }
}

static void s_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats) {
    struct aws_h1_connection *connection = handler->impl;

    /* TODO: Need update the way we calculate statistics, to account for user-controlled pauses.
     * If user is adding chunks 1 by 1, there can naturally be a gap in the upload.
     * If the user lets the stream-window go to zero, there can naturally be a gap in the download. */
    s_pull_up_stats_timestamps(connection);

    void *stats_base = &connection->thread_data.stats;
    aws_array_list_push_back(stats, &stats_base);
}

struct aws_crt_statistics_http1_channel *aws_h1_connection_get_statistics(struct aws_http_connection *connection) {
    AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->channel_slot->channel));

    struct aws_h1_connection *h1_conn = (void *)connection;

    return &h1_conn->thread_data.stats;
}

struct aws_h1_window_stats aws_h1_connection_window_stats(struct aws_http_connection *connection_base) {
    struct aws_h1_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h1_connection, base);
    struct aws_h1_window_stats stats = {
        .connection_window = connection->thread_data.connection_window,
        .buffer_capacity = connection->thread_data.read_buffer.capacity,
        .buffer_pending_bytes = connection->thread_data.read_buffer.pending_bytes,
        .recent_window_increments = connection->thread_data.recent_window_increments,
        .has_incoming_stream = connection->thread_data.incoming_stream != NULL,
        .stream_window = connection->thread_data.incoming_stream
                             ? connection->thread_data.incoming_stream->thread_data.stream_window
                             : 0,
    };

    /* Resets each time it's queried */
    connection->thread_data.recent_window_increments = 0;

    return stats;
}
