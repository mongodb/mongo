/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/h2_connection.h>
#include <aws/http/private/h2_stream.h>

#include <aws/http/private/h2_decoder.h>
#include <aws/http/private/h2_stream.h>
#include <aws/http/private/strutil.h>

#include <aws/common/clock.h>
#include <aws/common/logging.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

/* Apple toolchains such as xcode and swiftpm define the DEBUG symbol. undef it here so we can actually use the token */
#undef DEBUG

#define CONNECTION_LOGF(level, connection, text, ...)                                                                  \
    AWS_LOGF_##level(AWS_LS_HTTP_CONNECTION, "id=%p: " text, (void *)(connection), __VA_ARGS__)
#define CONNECTION_LOG(level, connection, text) CONNECTION_LOGF(level, connection, "%s", text)

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
static struct aws_http_stream *s_connection_make_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options);
static void s_connection_close(struct aws_http_connection *connection_base);
static void s_connection_stop_new_request(struct aws_http_connection *connection_base);
static bool s_connection_is_open(const struct aws_http_connection *connection_base);
static bool s_connection_new_requests_allowed(const struct aws_http_connection *connection_base);
static void s_connection_update_window(struct aws_http_connection *connection_base, uint32_t increment_size);
static int s_connection_change_settings(
    struct aws_http_connection *connection_base,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data);
static int s_connection_send_ping(
    struct aws_http_connection *connection_base,
    const struct aws_byte_cursor *optional_opaque_data,
    aws_http2_on_ping_complete_fn *on_completed,
    void *user_data);
static void s_connection_send_goaway(
    struct aws_http_connection *connection_base,
    uint32_t http2_error,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data);
static int s_connection_get_sent_goaway(
    struct aws_http_connection *connection_base,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id);
static int s_connection_get_received_goaway(
    struct aws_http_connection *connection_base,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id);
static void s_connection_get_local_settings(
    const struct aws_http_connection *connection_base,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);
static void s_connection_get_remote_settings(
    const struct aws_http_connection *connection_base,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);

static void s_cross_thread_work_task(struct aws_channel_task *task, void *arg, enum aws_task_status status);
static void s_outgoing_frames_task(struct aws_channel_task *task, void *arg, enum aws_task_status status);
static int s_encode_outgoing_frames_queue(struct aws_h2_connection *connection, struct aws_byte_buf *output);
static int s_encode_data_from_outgoing_streams(struct aws_h2_connection *connection, struct aws_byte_buf *output);
static int s_record_closed_stream(
    struct aws_h2_connection *connection,
    uint32_t stream_id,
    enum aws_h2_stream_closed_when closed_when);
static void s_stream_complete(struct aws_h2_connection *connection, struct aws_h2_stream *stream, int error_code);
static void s_write_outgoing_frames(struct aws_h2_connection *connection, bool first_try);
static void s_finish_shutdown(struct aws_h2_connection *connection);
static void s_send_goaway(
    struct aws_h2_connection *connection,
    uint32_t h2_error_code,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data);
static struct aws_h2_pending_settings *s_new_pending_settings(
    struct aws_allocator *allocator,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data);

static struct aws_h2err s_decoder_on_headers_begin(uint32_t stream_id, void *userdata);
static struct aws_h2err s_decoder_on_headers_i(
    uint32_t stream_id,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type,
    void *userdata);
static struct aws_h2err s_decoder_on_headers_end(
    uint32_t stream_id,
    bool malformed,
    enum aws_http_header_block block_type,
    void *userdata);
static struct aws_h2err s_decoder_on_push_promise(uint32_t stream_id, uint32_t promised_stream_id, void *userdata);
static struct aws_h2err s_decoder_on_data_begin(
    uint32_t stream_id,
    uint32_t payload_len,
    uint32_t total_padding_bytes,
    bool end_stream,
    void *userdata);
static struct aws_h2err s_decoder_on_data_i(uint32_t stream_id, struct aws_byte_cursor data, void *userdata);
static struct aws_h2err s_decoder_on_end_stream(uint32_t stream_id, void *userdata);
static struct aws_h2err s_decoder_on_rst_stream(uint32_t stream_id, uint32_t h2_error_code, void *userdata);
static struct aws_h2err s_decoder_on_ping_ack(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata);
static struct aws_h2err s_decoder_on_ping(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata);
static struct aws_h2err s_decoder_on_settings(
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    void *userdata);
static struct aws_h2err s_decoder_on_settings_ack(void *userdata);
static struct aws_h2err s_decoder_on_window_update(uint32_t stream_id, uint32_t window_size_increment, void *userdata);
struct aws_h2err s_decoder_on_goaway(
    uint32_t last_stream,
    uint32_t error_code,
    struct aws_byte_cursor debug_data,
    void *userdata);
static void s_reset_statistics(struct aws_channel_handler *handler);
static void s_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats);

static struct aws_http_connection_vtable s_h2_connection_vtable = {
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
    .make_request = s_connection_make_request,
    .new_server_request_handler_stream = NULL,
    .stream_send_response = NULL,
    .close = s_connection_close,
    .stop_new_requests = s_connection_stop_new_request,
    .is_open = s_connection_is_open,
    .new_requests_allowed = s_connection_new_requests_allowed,
    .update_window = s_connection_update_window,
    .change_settings = s_connection_change_settings,
    .send_ping = s_connection_send_ping,
    .send_goaway = s_connection_send_goaway,
    .get_sent_goaway = s_connection_get_sent_goaway,
    .get_received_goaway = s_connection_get_received_goaway,
    .get_local_settings = s_connection_get_local_settings,
    .get_remote_settings = s_connection_get_remote_settings,
};

static const struct aws_h2_decoder_vtable s_h2_decoder_vtable = {
    .on_headers_begin = s_decoder_on_headers_begin,
    .on_headers_i = s_decoder_on_headers_i,
    .on_headers_end = s_decoder_on_headers_end,
    .on_push_promise_begin = s_decoder_on_push_promise,
    .on_data_begin = s_decoder_on_data_begin,
    .on_data_i = s_decoder_on_data_i,
    .on_end_stream = s_decoder_on_end_stream,
    .on_rst_stream = s_decoder_on_rst_stream,
    .on_ping_ack = s_decoder_on_ping_ack,
    .on_ping = s_decoder_on_ping,
    .on_settings = s_decoder_on_settings,
    .on_settings_ack = s_decoder_on_settings_ack,
    .on_window_update = s_decoder_on_window_update,
    .on_goaway = s_decoder_on_goaway,
};

static void s_lock_synced_data(struct aws_h2_connection *connection) {
    int err = aws_mutex_lock(&connection->synced_data.lock);
    AWS_ASSERT(!err && "lock failed");
    (void)err;
}

static void s_unlock_synced_data(struct aws_h2_connection *connection) {
    int err = aws_mutex_unlock(&connection->synced_data.lock);
    AWS_ASSERT(!err && "unlock failed");
    (void)err;
}

static void s_acquire_stream_and_connection_lock(struct aws_h2_stream *stream, struct aws_h2_connection *connection) {
    int err = aws_mutex_lock(&stream->synced_data.lock);
    err |= aws_mutex_lock(&connection->synced_data.lock);
    AWS_ASSERT(!err && "lock connection and stream failed");
    (void)err;
}

static void s_release_stream_and_connection_lock(struct aws_h2_stream *stream, struct aws_h2_connection *connection) {
    int err = aws_mutex_unlock(&connection->synced_data.lock);
    err |= aws_mutex_unlock(&stream->synced_data.lock);
    AWS_ASSERT(!err && "unlock connection and stream failed");
    (void)err;
}

static void s_add_time_measurement_to_stats(uint64_t start_ns, uint64_t end_ns, uint64_t *output_ms) {
    if (end_ns > start_ns) {
        *output_ms += aws_timestamp_convert(end_ns - start_ns, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL);
    } else {
        *output_ms = 0;
    }
}

/**
 * Internal function for bringing connection to a stop.
 * Invoked multiple times, including when:
 * - Channel is shutting down in the read direction.
 * - Channel is shutting down in the write direction.
 * - An error occurs that will shutdown the channel.
 * - User wishes to close the connection (this is the only case where the function may run off-thread).
 */
static void s_stop(
    struct aws_h2_connection *connection,
    bool stop_reading,
    bool stop_writing,
    bool schedule_shutdown,
    int error_code) {

    AWS_ASSERT(stop_reading || stop_writing || schedule_shutdown); /* You are required to stop at least 1 thing */

    if (stop_reading) {
        AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
        connection->thread_data.is_reading_stopped = true;
    }

    if (stop_writing) {
        AWS_ASSERT(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
        connection->thread_data.is_writing_stopped = true;
    }

    /* Even if we're not scheduling shutdown just yet (ex: sent final request but waiting to read final response)
     * we don't consider the connection "open" anymore so user can't create more streams */
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
        connection->synced_data.is_open = false;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (schedule_shutdown) {
        AWS_LOGF_INFO(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Shutting down connection with error code %d (%s).",
            (void *)&connection->base,
            error_code,
            aws_error_name(error_code));

        aws_channel_shutdown(connection->base.channel_slot->channel, error_code);
    }
}

void aws_h2_connection_shutdown_due_to_write_err(struct aws_h2_connection *connection, int error_code) {
    AWS_PRECONDITION(error_code);
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written) {
        /* If shutdown is waiting for writes to complete, but writes are now broken,
         * then we must finish shutdown now */
        s_finish_shutdown(connection);
    } else {
        s_stop(connection, false /*stop_reading*/, true /*stop_writing*/, true /*schedule_shutdown*/, error_code);
    }
}

/* Common new() logic for server & client */
static struct aws_h2_connection *s_connection_new(
    struct aws_allocator *alloc,
    bool manual_window_management,
    const struct aws_http2_connection_options *http2_options,
    bool server) {

    AWS_PRECONDITION(http2_options);

    struct aws_h2_connection *connection = aws_mem_calloc(alloc, 1, sizeof(struct aws_h2_connection));
    if (!connection) {
        return NULL;
    }
    connection->base.vtable = &s_h2_connection_vtable;
    connection->base.alloc = alloc;
    connection->base.channel_handler.vtable = &s_h2_connection_vtable.channel_handler_vtable;
    connection->base.channel_handler.alloc = alloc;
    connection->base.channel_handler.impl = connection;
    connection->base.http_version = AWS_HTTP_VERSION_2;
    /* Init the next stream id (server must use even ids, client odd [RFC 7540 5.1.1])*/
    connection->base.next_stream_id = (server ? 2 : 1);
    /* Stream window management */
    connection->base.stream_manual_window_management = manual_window_management;

    /* Connection window management */
    connection->conn_manual_window_management = http2_options->conn_manual_window_management;
    connection->on_goaway_received = http2_options->on_goaway_received;
    connection->on_remote_settings_change = http2_options->on_remote_settings_change;

    aws_channel_task_init(
        &connection->cross_thread_work_task, s_cross_thread_work_task, connection, "HTTP/2 cross-thread work");

    aws_channel_task_init(
        &connection->outgoing_frames_task, s_outgoing_frames_task, connection, "HTTP/2 outgoing frames");

    /* 1 refcount for user */
    aws_atomic_init_int(&connection->base.refcount, 1);
    uint32_t max_stream_id = AWS_H2_STREAM_ID_MAX;
    connection->synced_data.goaway_sent_last_stream_id = max_stream_id + 1;
    connection->synced_data.goaway_received_last_stream_id = max_stream_id + 1;

    aws_linked_list_init(&connection->synced_data.pending_stream_list);
    aws_linked_list_init(&connection->synced_data.pending_frame_list);
    aws_linked_list_init(&connection->synced_data.pending_settings_list);
    aws_linked_list_init(&connection->synced_data.pending_ping_list);
    aws_linked_list_init(&connection->synced_data.pending_goaway_list);

    aws_linked_list_init(&connection->thread_data.outgoing_streams_list);
    aws_linked_list_init(&connection->thread_data.pending_settings_queue);
    aws_linked_list_init(&connection->thread_data.pending_ping_queue);
    aws_linked_list_init(&connection->thread_data.stalled_window_streams_list);
    aws_linked_list_init(&connection->thread_data.waiting_streams_list);
    aws_linked_list_init(&connection->thread_data.outgoing_frames_queue);

    if (aws_mutex_init(&connection->synced_data.lock)) {
        CONNECTION_LOGF(
            ERROR, connection, "Mutex init error %d (%s).", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }

    if (aws_hash_table_init(
            &connection->thread_data.active_streams_map, alloc, 8, aws_hash_ptr, aws_ptr_eq, NULL, NULL)) {

        CONNECTION_LOGF(
            ERROR, connection, "Hashtable init error %d (%s).", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }
    size_t max_closed_streams = AWS_HTTP2_DEFAULT_MAX_CLOSED_STREAMS;
    if (http2_options->max_closed_streams) {
        max_closed_streams = http2_options->max_closed_streams;
    }

    connection->thread_data.closed_streams =
        aws_cache_new_fifo(alloc, aws_hash_ptr, aws_ptr_eq, NULL, NULL, max_closed_streams);
    if (!connection->thread_data.closed_streams) {
        CONNECTION_LOGF(
            ERROR, connection, "FIFO cache init error %d (%s).", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }

    /* Initialize the value of settings */
    memcpy(connection->thread_data.settings_peer, aws_h2_settings_initial, sizeof(aws_h2_settings_initial));
    memcpy(connection->thread_data.settings_self, aws_h2_settings_initial, sizeof(aws_h2_settings_initial));

    memcpy(connection->synced_data.settings_peer, aws_h2_settings_initial, sizeof(aws_h2_settings_initial));
    memcpy(connection->synced_data.settings_self, aws_h2_settings_initial, sizeof(aws_h2_settings_initial));

    connection->thread_data.window_size_peer = AWS_H2_INIT_WINDOW_SIZE;
    connection->thread_data.window_size_self = AWS_H2_INIT_WINDOW_SIZE;

    connection->thread_data.goaway_received_last_stream_id = AWS_H2_STREAM_ID_MAX;
    connection->thread_data.goaway_sent_last_stream_id = AWS_H2_STREAM_ID_MAX;

    aws_crt_statistics_http2_channel_init(&connection->thread_data.stats);
    connection->thread_data.stats.was_inactive = true; /* Start with non active streams */

    connection->synced_data.is_open = true;
    connection->synced_data.new_stream_error_code = AWS_ERROR_SUCCESS;

    /* Create a new decoder */
    struct aws_h2_decoder_params params = {
        .alloc = alloc,
        .vtable = &s_h2_decoder_vtable,
        .userdata = connection,
        .logging_id = connection,
        .is_server = server,
    };
    connection->thread_data.decoder = aws_h2_decoder_new(&params);
    if (!connection->thread_data.decoder) {
        CONNECTION_LOGF(
            ERROR, connection, "Decoder init error %d (%s)", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }

    if (aws_h2_frame_encoder_init(&connection->thread_data.encoder, alloc, &connection->base)) {
        CONNECTION_LOGF(
            ERROR, connection, "Encoder init error %d (%s)", aws_last_error(), aws_error_name(aws_last_error()));
        goto error;
    }
    /* User data from connection base is not ready until the handler installed */
    connection->thread_data.init_pending_settings = s_new_pending_settings(
        connection->base.alloc,
        http2_options->initial_settings_array,
        http2_options->num_initial_settings,
        http2_options->on_initial_settings_completed,
        NULL /* user_data is set later... */);
    if (!connection->thread_data.init_pending_settings) {
        goto error;
    }
    /* We enqueue the inital settings when handler get installed */
    return connection;

error:
    s_handler_destroy(&connection->base.channel_handler);

    return NULL;
}

struct aws_http_connection *aws_http_connection_new_http2_server(
    struct aws_allocator *allocator,
    bool manual_window_management,
    const struct aws_http2_connection_options *http2_options) {

    struct aws_h2_connection *connection = s_connection_new(allocator, manual_window_management, http2_options, true);
    if (!connection) {
        return NULL;
    }

    connection->base.server_data = &connection->base.client_or_server_data.server;

    return &connection->base;
}

struct aws_http_connection *aws_http_connection_new_http2_client(
    struct aws_allocator *allocator,
    bool manual_window_management,
    const struct aws_http2_connection_options *http2_options) {

    struct aws_h2_connection *connection = s_connection_new(allocator, manual_window_management, http2_options, false);
    if (!connection) {
        return NULL;
    }

    connection->base.client_data = &connection->base.client_or_server_data.client;

    return &connection->base;
}

static void s_handler_destroy(struct aws_channel_handler *handler) {
    struct aws_h2_connection *connection = handler->impl;
    CONNECTION_LOG(TRACE, connection, "Destroying connection");

    /* No streams should be left in internal datastructures */
    AWS_ASSERT(
        !aws_hash_table_is_valid(&connection->thread_data.active_streams_map) ||
        aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) == 0);

    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.waiting_streams_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.stalled_window_streams_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.outgoing_streams_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.pending_stream_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.pending_frame_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.pending_settings_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.pending_ping_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->synced_data.pending_goaway_list));
    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.pending_ping_queue));
    AWS_ASSERT(aws_linked_list_empty(&connection->thread_data.pending_settings_queue));

    /* Clean up any unsent frames and structures */
    struct aws_linked_list *outgoing_frames_queue = &connection->thread_data.outgoing_frames_queue;
    while (!aws_linked_list_empty(outgoing_frames_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(outgoing_frames_queue);
        struct aws_h2_frame *frame = AWS_CONTAINER_OF(node, struct aws_h2_frame, node);
        aws_h2_frame_destroy(frame);
    }
    if (connection->thread_data.init_pending_settings) {
        /* if initial settings were never sent, we need to clear the memory here */
        aws_mem_release(connection->base.alloc, connection->thread_data.init_pending_settings);
    }
    aws_h2_decoder_destroy(connection->thread_data.decoder);
    aws_h2_frame_encoder_clean_up(&connection->thread_data.encoder);
    aws_hash_table_clean_up(&connection->thread_data.active_streams_map);
    aws_cache_destroy(connection->thread_data.closed_streams);
    aws_mutex_clean_up(&connection->synced_data.lock);
    aws_mem_release(connection->base.alloc, connection);
}

static struct aws_h2_pending_settings *s_new_pending_settings(
    struct aws_allocator *allocator,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data) {

    size_t settings_storage_size = sizeof(struct aws_http2_setting) * num_settings;
    struct aws_h2_pending_settings *pending_settings;
    void *settings_storage;
    if (!aws_mem_acquire_many(
            allocator,
            2,
            &pending_settings,
            sizeof(struct aws_h2_pending_settings),
            &settings_storage,
            settings_storage_size)) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*pending_settings);
    /* We buffer the settings up, incase the caller has freed them when the ACK arrives */
    pending_settings->settings_array = settings_storage;
    if (settings_array) {
        memcpy(pending_settings->settings_array, settings_array, num_settings * sizeof(struct aws_http2_setting));
    }
    pending_settings->num_settings = num_settings;
    pending_settings->on_completed = on_completed;
    pending_settings->user_data = user_data;

    return pending_settings;
}

static struct aws_h2_pending_ping *s_new_pending_ping(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *optional_opaque_data,
    const uint64_t started_time,
    void *user_data,
    aws_http2_on_ping_complete_fn *on_completed) {

    struct aws_h2_pending_ping *pending_ping = aws_mem_calloc(allocator, 1, sizeof(struct aws_h2_pending_ping));
    if (!pending_ping) {
        return NULL;
    }
    if (optional_opaque_data) {
        memcpy(pending_ping->opaque_data, optional_opaque_data->ptr, AWS_HTTP2_PING_DATA_SIZE);
    }
    pending_ping->started_time = started_time;
    pending_ping->on_completed = on_completed;
    pending_ping->user_data = user_data;
    return pending_ping;
}

static struct aws_h2_pending_goaway *s_new_pending_goaway(
    struct aws_allocator *allocator,
    uint32_t http2_error,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data) {

    struct aws_byte_cursor debug_data;
    AWS_ZERO_STRUCT(debug_data);
    if (optional_debug_data) {
        debug_data = *optional_debug_data;
    }
    struct aws_h2_pending_goaway *pending_goaway;
    void *debug_data_storage;
    /* mem acquire cannot fail anymore */
    aws_mem_acquire_many(
        allocator, 2, &pending_goaway, sizeof(struct aws_h2_pending_goaway), &debug_data_storage, debug_data.len);
    if (debug_data.len) {
        memcpy(debug_data_storage, debug_data.ptr, debug_data.len);
        debug_data.ptr = debug_data_storage;
    }
    pending_goaway->debug_data = debug_data;
    pending_goaway->http2_error = http2_error;
    pending_goaway->allow_more_streams = allow_more_streams;
    return pending_goaway;
}

void aws_h2_connection_enqueue_outgoing_frame(struct aws_h2_connection *connection, struct aws_h2_frame *frame) {
    AWS_PRECONDITION(frame->type != AWS_H2_FRAME_T_DATA);
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (frame->high_priority) {
        /* Check from the head of the queue, and find a node with normal priority, and insert before it */
        struct aws_linked_list_node *iter = aws_linked_list_begin(&connection->thread_data.outgoing_frames_queue);
        /* one past the last element */
        const struct aws_linked_list_node *end = aws_linked_list_end(&connection->thread_data.outgoing_frames_queue);
        while (iter != end) {
            struct aws_h2_frame *frame_i = AWS_CONTAINER_OF(iter, struct aws_h2_frame, node);
            if (connection->thread_data.current_outgoing_frame == frame_i) {
                iter = iter->next;
                continue;
            }
            if (!frame_i->high_priority) {
                break;
            }
            iter = iter->next;
        }
        aws_linked_list_insert_before(iter, &frame->node);
    } else {
        aws_linked_list_push_back(&connection->thread_data.outgoing_frames_queue, &frame->node);
    }
}

static void s_on_channel_write_complete(
    struct aws_channel *channel,
    struct aws_io_message *message,
    int err_code,
    void *user_data) {

    (void)message;
    struct aws_h2_connection *connection = user_data;

    if (err_code) {
        CONNECTION_LOGF(ERROR, connection, "Message did not write to network, error %s", aws_error_name(err_code));
        aws_h2_connection_shutdown_due_to_write_err(connection, err_code);
        return;
    }

    CONNECTION_LOG(TRACE, connection, "Message finished writing to network. Rescheduling outgoing frame task");

    /* To avoid wasting memory, we only want ONE of our written aws_io_messages in the channel at a time.
     * Therefore, we wait until it's written to the network before trying to send another
     * by running the outgoing-frame-task again.
     *
     * We also want to share the network with other channels.
     * Therefore, when the write completes, we SCHEDULE the outgoing-frame-task
     * to run again instead of calling the function directly.
     * This way, if the message completes synchronously,
     * we're not hogging the network by writing message after message in a tight loop */
    aws_channel_schedule_task_now(channel, &connection->outgoing_frames_task);
}

static void s_outgoing_frames_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_h2_connection *connection = arg;
    s_write_outgoing_frames(connection, false /*first_try*/);
}

static void s_write_outgoing_frames(struct aws_h2_connection *connection, bool first_try) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_PRECONDITION(connection->thread_data.is_outgoing_frames_task_active);

    struct aws_channel_slot *channel_slot = connection->base.channel_slot;
    struct aws_linked_list *outgoing_frames_queue = &connection->thread_data.outgoing_frames_queue;
    struct aws_linked_list *outgoing_streams_list = &connection->thread_data.outgoing_streams_list;

    if (connection->thread_data.is_writing_stopped) {
        return;
    }

    /* Determine whether there's work to do, and end task immediately if there's not.
     * Note that we stop writing DATA frames if the channel is trying to shut down */
    bool has_control_frames = !aws_linked_list_empty(outgoing_frames_queue);
    bool has_data_frames = !aws_linked_list_empty(outgoing_streams_list);
    bool may_write_data_frames = (connection->thread_data.window_size_peer > AWS_H2_MIN_WINDOW_SIZE) &&
                                 !connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written;
    bool will_write = has_control_frames || (has_data_frames && may_write_data_frames);

    if (!will_write) {
        if (!first_try) {
            CONNECTION_LOGF(
                TRACE,
                connection,
                "Outgoing frames task stopped. has_control_frames:%d has_data_frames:%d may_write_data_frames:%d",
                has_control_frames,
                has_data_frames,
                may_write_data_frames);
        }

        connection->thread_data.is_outgoing_frames_task_active = false;

        if (connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written) {
            s_finish_shutdown(connection);
        }

        return;
    }

    if (first_try) {
        CONNECTION_LOG(TRACE, connection, "Starting outgoing frames task");
    }

    /* Acquire aws_io_message, that we will attempt to fill up */
    struct aws_io_message *msg = aws_channel_slot_acquire_max_message_for_write(channel_slot);
    if (AWS_UNLIKELY(!msg)) {
        CONNECTION_LOG(ERROR, connection, "Failed to acquire message from pool, closing connection.");
        goto error;
    }

    /* Set up callback so we can send another message when this one completes */
    msg->on_completion = s_on_channel_write_complete;
    msg->user_data = connection;

    CONNECTION_LOGF(
        TRACE,
        connection,
        "Outgoing frames task acquired message with %zu bytes available",
        msg->message_data.capacity - msg->message_data.len);

    /* Write as many frames from outgoing_frames_queue as possible. */
    if (s_encode_outgoing_frames_queue(connection, &msg->message_data)) {
        goto error;
    }

    /* If outgoing_frames_queue emptied, and connection is running normally,
     * then write as many DATA frames from outgoing_streams_list as possible. */
    if (aws_linked_list_empty(outgoing_frames_queue) && may_write_data_frames) {
        if (s_encode_data_from_outgoing_streams(connection, &msg->message_data)) {
            goto error;
        }
    }

    if (msg->message_data.len) {
        /* Write message to channel.
         * outgoing_frames_task will resume when message completes. */
        CONNECTION_LOGF(TRACE, connection, "Outgoing frames task sending message of size %zu", msg->message_data.len);

        if (aws_channel_slot_send_message(channel_slot, msg, AWS_CHANNEL_DIR_WRITE)) {
            CONNECTION_LOGF(
                ERROR,
                connection,
                "Failed to send channel message: %s. Closing connection.",
                aws_error_name(aws_last_error()));

            goto error;
        }
    } else {
        /* Message is empty, warn that no work is being done and reschedule the task to try again next tick.
         * It's likely that body isn't ready, so body streaming function has no data to write yet.
         * If this scenario turns out to be common we should implement a "pause" feature. */
        CONNECTION_LOG(WARN, connection, "Outgoing frames task sent no data, will try again next tick.");

        aws_mem_release(msg->allocator, msg);

        aws_channel_schedule_task_now(channel_slot->channel, &connection->outgoing_frames_task);
    }
    return;

error:;
    int error_code = aws_last_error();

    if (msg) {
        aws_mem_release(msg->allocator, msg);
    }

    aws_h2_connection_shutdown_due_to_write_err(connection, error_code);
}

/* Write as many frames from outgoing_frames_queue as possible (contains all non-DATA frames) */
static int s_encode_outgoing_frames_queue(struct aws_h2_connection *connection, struct aws_byte_buf *output) {

    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    struct aws_linked_list *outgoing_frames_queue = &connection->thread_data.outgoing_frames_queue;

    /* Write as many frames from outgoing_frames_queue as possible. */
    while (!aws_linked_list_empty(outgoing_frames_queue)) {
        struct aws_linked_list_node *frame_node = aws_linked_list_front(outgoing_frames_queue);
        struct aws_h2_frame *frame = AWS_CONTAINER_OF(frame_node, struct aws_h2_frame, node);
        connection->thread_data.current_outgoing_frame = frame;
        bool frame_complete;
        if (aws_h2_encode_frame(&connection->thread_data.encoder, frame, output, &frame_complete)) {
            CONNECTION_LOGF(
                ERROR,
                connection,
                "Error encoding frame: type=%s stream=%" PRIu32 " error=%s",
                aws_h2_frame_type_to_str(frame->type),
                frame->stream_id,
                aws_error_name(aws_last_error()));
            return AWS_OP_ERR;
        }

        if (!frame_complete) {
            if (output->len == 0) {
                /* We're in trouble if an empty message isn't big enough for this frame to do any work with */
                CONNECTION_LOGF(
                    ERROR,
                    connection,
                    "Message is too small for encoder. frame-type=%s stream=%" PRIu32 " available-space=%zu",
                    aws_h2_frame_type_to_str(frame->type),
                    frame->stream_id,
                    output->capacity);
                aws_raise_error(AWS_ERROR_INVALID_STATE);
                return AWS_OP_ERR;
            }

            CONNECTION_LOG(TRACE, connection, "Outgoing frames task filled message, and has more frames to send later");
            break;
        }

        /* Done encoding frame, pop from queue and cleanup*/
        aws_linked_list_remove(frame_node);
        aws_h2_frame_destroy(frame);
        connection->thread_data.current_outgoing_frame = NULL;
    }

    return AWS_OP_SUCCESS;
}

/* Write as many DATA frames from outgoing_streams_list as possible. */
static int s_encode_data_from_outgoing_streams(struct aws_h2_connection *connection, struct aws_byte_buf *output) {

    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    struct aws_linked_list *outgoing_streams_list = &connection->thread_data.outgoing_streams_list;
    if (aws_linked_list_empty(outgoing_streams_list)) {
        return AWS_OP_SUCCESS;
    }
    struct aws_linked_list *stalled_window_streams_list = &connection->thread_data.stalled_window_streams_list;
    struct aws_linked_list *waiting_streams_list = &connection->thread_data.waiting_streams_list;

    /* If a stream stalls, put it in this list until the function ends so we don't keep trying to read from it.
     * We put it back at the end of function. */
    struct aws_linked_list stalled_streams_list;
    aws_linked_list_init(&stalled_streams_list);

    int aws_error_code = 0;

    /* We simply round-robin through streams, instead of using stream priority.
     * Respecting priority is not required (RFC-7540 5.3), so we're ignoring it for now. This also keeps use safe
     * from priority DOS attacks: https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2019-9513 */
    while (!aws_linked_list_empty(outgoing_streams_list)) {
        if (connection->thread_data.window_size_peer <= AWS_H2_MIN_WINDOW_SIZE) {
            CONNECTION_LOGF(
                DEBUG,
                connection,
                "Peer connection's flow-control window is too small now %zu. Connection will stop sending DATA until "
                "WINDOW_UPDATE is received.",
                connection->thread_data.window_size_peer);
            goto done;
        }

        /* Stop looping if message is so full it's not worth the bother */
        size_t space_available = output->capacity - output->len;
        size_t worth_trying_threshold = AWS_H2_FRAME_PREFIX_SIZE * 2;
        if (space_available < worth_trying_threshold) {
            CONNECTION_LOG(TRACE, connection, "Outgoing frames task filled message, and has more frames to send later");
            goto done;
        }

        struct aws_linked_list_node *node = aws_linked_list_pop_front(outgoing_streams_list);
        struct aws_h2_stream *stream = AWS_CONTAINER_OF(node, struct aws_h2_stream, node);

        /* Ask stream to encode a data frame.
         * Stream may complete itself as a result of encoding its data,
         * in which case it will vanish from the connection's datastructures as a side-effect of this call.
         * But if stream has more data to send, push it back into the appropriate list. */
        int data_encode_status;
        if (aws_h2_stream_encode_data_frame(stream, &connection->thread_data.encoder, output, &data_encode_status)) {

            aws_error_code = aws_last_error();
            CONNECTION_LOGF(
                ERROR,
                connection,
                "Connection error while encoding DATA on stream %" PRIu32 ", %s",
                stream->base.id,
                aws_error_name(aws_error_code));
            goto done;
        }

        /* If stream has more data, push it into the appropriate list. */
        switch (data_encode_status) {
            case AWS_H2_DATA_ENCODE_COMPLETE:
                break;
            case AWS_H2_DATA_ENCODE_ONGOING:
                aws_linked_list_push_back(outgoing_streams_list, node);
                break;
            case AWS_H2_DATA_ENCODE_ONGOING_BODY_STREAM_STALLED:
                aws_linked_list_push_back(&stalled_streams_list, node);
                break;
            case AWS_H2_DATA_ENCODE_ONGOING_WAITING_FOR_WRITES:
                stream->thread_data.waiting_for_writes = true;
                aws_linked_list_push_back(waiting_streams_list, node);
                break;
            case AWS_H2_DATA_ENCODE_ONGOING_WINDOW_STALLED:
                aws_linked_list_push_back(stalled_window_streams_list, node);
                AWS_H2_STREAM_LOG(
                    DEBUG,
                    stream,
                    "Peer stream's flow-control window is too small. Data frames on this stream will not be sent until "
                    "WINDOW_UPDATE. ");
                break;
            default:
                CONNECTION_LOG(ERROR, connection, "Data encode status is invalid.");
                aws_error_code = AWS_ERROR_INVALID_STATE;
        }
    }

done:
    /* Return any stalled streams to outgoing_streams_list */
    while (!aws_linked_list_empty(&stalled_streams_list)) {
        aws_linked_list_push_back(outgoing_streams_list, aws_linked_list_pop_front(&stalled_streams_list));
    }

    if (aws_error_code) {
        return aws_raise_error(aws_error_code);
    }

    if (aws_linked_list_empty(outgoing_streams_list)) {
        /* transition from something to write -> nothing to write */
        uint64_t now_ns = 0;
        aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns);
        s_add_time_measurement_to_stats(
            connection->thread_data.outgoing_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_outgoing_stream_ms);
    }

    return AWS_OP_SUCCESS;
}

/* If the outgoing-frames-task isn't scheduled, run it immediately. */
void aws_h2_try_write_outgoing_frames(struct aws_h2_connection *connection) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (connection->thread_data.is_outgoing_frames_task_active) {
        return;
    }

    connection->thread_data.is_outgoing_frames_task_active = true;
    s_write_outgoing_frames(connection, true /*first_try*/);
}

/**
 * Returns successfully and sets `out_stream` if stream is currently active.
 * Returns successfully and sets `out_stream` to NULL if the frame should be ignored.
 * Returns failed aws_h2err if it is a connection error to receive this frame.
 */
struct aws_h2err s_get_active_stream_for_incoming_frame(
    struct aws_h2_connection *connection,
    uint32_t stream_id,
    enum aws_h2_frame_type frame_type,
    struct aws_h2_stream **out_stream) {

    *out_stream = NULL;

    /* Check active streams */
    struct aws_hash_element *found = NULL;
    const void *stream_id_key = (void *)(size_t)stream_id;
    aws_hash_table_find(&connection->thread_data.active_streams_map, stream_id_key, &found);
    if (found) {
        /* Found it! return */
        *out_stream = found->value;
        return AWS_H2ERR_SUCCESS;
    }

    bool client_initiated = (stream_id % 2) == 1;
    bool self_initiated_stream = client_initiated && (connection->base.client_data != NULL);
    bool peer_initiated_stream = !self_initiated_stream;

    if ((self_initiated_stream && stream_id >= connection->base.next_stream_id) ||
        (peer_initiated_stream && stream_id > connection->thread_data.latest_peer_initiated_stream_id)) {
        /* Illegal to receive frames for a stream in the idle state (stream doesn't exist yet)
         * (except server receiving HEADERS to start a stream, but that's handled elsewhere) */
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Illegal to receive %s frame on stream id=%" PRIu32 " state=IDLE",
            aws_h2_frame_type_to_str(frame_type),
            stream_id);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }

    if (peer_initiated_stream && stream_id > connection->thread_data.goaway_sent_last_stream_id) {
        /* Once GOAWAY sent, ignore frames for peer-initiated streams whose id > last-stream-id */
        CONNECTION_LOGF(
            TRACE,
            connection,
            "Ignoring %s frame on stream id=%" PRIu32 " because GOAWAY sent with last-stream-id=%" PRIu32,
            aws_h2_frame_type_to_str(frame_type),
            stream_id,
            connection->thread_data.goaway_sent_last_stream_id);

        return AWS_H2ERR_SUCCESS;
    }

    void *cached_value = NULL;
    /* Stream is closed, check whether it's legal for a few more frames to trickle in */
    if (aws_cache_find(connection->thread_data.closed_streams, stream_id_key, &cached_value)) {
        return aws_h2err_from_last_error();
    }
    if (cached_value) {
        if (frame_type == AWS_H2_FRAME_T_PRIORITY) {
            /* If we support PRIORITY, do something here. Right now just ignore it */
            return AWS_H2ERR_SUCCESS;
        }
        enum aws_h2_stream_closed_when closed_when = (enum aws_h2_stream_closed_when)(size_t)cached_value;
        switch (closed_when) {
            case AWS_H2_STREAM_CLOSED_WHEN_BOTH_SIDES_END_STREAM:
                /* WINDOW_UPDATE or RST_STREAM frames can be received ... for a short period after
                 * a DATA or HEADERS frame containing an END_STREAM flag is sent.
                 * Endpoints MUST ignore WINDOW_UPDATE or RST_STREAM frames received in this state */
                if (frame_type == AWS_H2_FRAME_T_WINDOW_UPDATE || frame_type == AWS_H2_FRAME_T_RST_STREAM) {
                    CONNECTION_LOGF(
                        TRACE,
                        connection,
                        "Ignoring %s frame on stream id=%" PRIu32 " because END_STREAM flag was recently sent.",
                        aws_h2_frame_type_to_str(frame_type),
                        stream_id);

                    return AWS_H2ERR_SUCCESS;
                } else {
                    CONNECTION_LOGF(
                        ERROR,
                        connection,
                        "Illegal to receive %s frame on stream id=%" PRIu32 " after END_STREAM has been received.",
                        aws_h2_frame_type_to_str(frame_type),
                        stream_id);

                    return aws_h2err_from_h2_code(AWS_HTTP2_ERR_STREAM_CLOSED);
                }
                break;
            case AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_RECEIVED:
                /* An endpoint that receives any frame other than PRIORITY after receiving a RST_STREAM
                 * MUST treat that as a stream error (Section 5.4.2) of type STREAM_CLOSED */
                CONNECTION_LOGF(
                    ERROR,
                    connection,
                    "Illegal to receive %s frame on stream id=%" PRIu32 " after RST_STREAM has been received",
                    aws_h2_frame_type_to_str(frame_type),
                    stream_id);
                struct aws_h2_frame *rst_stream =
                    aws_h2_frame_new_rst_stream(connection->base.alloc, stream_id, AWS_HTTP2_ERR_STREAM_CLOSED);
                if (!rst_stream) {
                    CONNECTION_LOGF(
                        ERROR, connection, "Error creating RST_STREAM frame, %s", aws_error_name(aws_last_error()));
                    return aws_h2err_from_last_error();
                }
                aws_h2_connection_enqueue_outgoing_frame(connection, rst_stream);
                return AWS_H2ERR_SUCCESS;
            case AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_SENT:
                /* An endpoint MUST ignore frames that it receives on closed streams after it has sent a RST_STREAM
                 * frame */
                CONNECTION_LOGF(
                    TRACE,
                    connection,
                    "Ignoring %s frame on stream id=%" PRIu32 " because RST_STREAM was recently sent.",
                    aws_h2_frame_type_to_str(frame_type),
                    stream_id);

                return AWS_H2ERR_SUCCESS;
                break;
            default:
                CONNECTION_LOGF(
                    ERROR, connection, "Invalid state fo cached closed stream, stream id=%" PRIu32, stream_id);
                return aws_h2err_from_h2_code(AWS_HTTP2_ERR_INTERNAL_ERROR);
                break;
        }
    }
    if (frame_type == AWS_H2_FRAME_T_PRIORITY) {
        /* ignored if the stream has been removed from the dependency tree */
        return AWS_H2ERR_SUCCESS;
    }

    /* Stream closed (purged from closed_streams, or implicitly closed when its ID was skipped) */
    CONNECTION_LOGF(
        ERROR,
        connection,
        "Illegal to receive %s frame on stream id=%" PRIu32
        ", no memory of closed stream (ID skipped, or removed from cache)",
        aws_h2_frame_type_to_str(frame_type),
        stream_id);

    return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
}

/* Decoder callbacks */

struct aws_h2err s_decoder_on_headers_begin(uint32_t stream_id, void *userdata) {
    struct aws_h2_connection *connection = userdata;

    if (connection->base.server_data) {
        /* Server would create new request-handler stream... */
        return aws_h2err_from_aws_code(AWS_ERROR_UNIMPLEMENTED);
    }

    struct aws_h2_stream *stream;
    struct aws_h2err err =
        s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_HEADERS, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_headers_begin(stream);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_headers_i(
    uint32_t stream_id,
    const struct aws_http_header *header,
    enum aws_http_header_name name_enum,
    enum aws_http_header_block block_type,
    void *userdata) {

    struct aws_h2_connection *connection = userdata;
    struct aws_h2_stream *stream;
    struct aws_h2err err =
        s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_HEADERS, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_headers_i(stream, header, name_enum, block_type);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_headers_end(
    uint32_t stream_id,
    bool malformed,
    enum aws_http_header_block block_type,
    void *userdata) {

    struct aws_h2_connection *connection = userdata;
    struct aws_h2_stream *stream;
    struct aws_h2err err =
        s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_HEADERS, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_headers_end(stream, malformed, block_type);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_push_promise(uint32_t stream_id, uint32_t promised_stream_id, void *userdata) {
    struct aws_h2_connection *connection = userdata;
    AWS_ASSERT(connection->base.client_data); /* decoder has already enforced this */
    AWS_ASSERT(promised_stream_id % 2 == 0);  /* decoder has already enforced this  */

    /* The identifier of a newly established stream MUST be numerically greater
     * than all streams that the initiating endpoint has opened or reserved (RFC-7540 5.1.1) */
    if (promised_stream_id <= connection->thread_data.latest_peer_initiated_stream_id) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Newly promised stream ID %" PRIu32 " must be higher than previously established ID %" PRIu32,
            promised_stream_id,
            connection->thread_data.latest_peer_initiated_stream_id);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }
    connection->thread_data.latest_peer_initiated_stream_id = promised_stream_id;

    /* If we ever fully support PUSH_PROMISE, this is where we'd add the
     * promised_stream_id to some reserved_streams datastructure */

    struct aws_h2_stream *stream;
    struct aws_h2err err =
        s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_PUSH_PROMISE, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_push_promise(stream, promised_stream_id);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

static int s_connection_send_update_window(struct aws_h2_connection *connection, uint32_t window_size) {
    struct aws_h2_frame *connection_window_update_frame =
        aws_h2_frame_new_window_update(connection->base.alloc, 0, window_size);
    if (!connection_window_update_frame) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "WINDOW_UPDATE frame on connection failed to be sent, error %s",
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }
    aws_h2_connection_enqueue_outgoing_frame(connection, connection_window_update_frame);
    connection->thread_data.window_size_self += window_size;
    return AWS_OP_SUCCESS;
}

struct aws_h2err s_decoder_on_data_begin(
    uint32_t stream_id,
    uint32_t payload_len,
    uint32_t total_padding_bytes,
    bool end_stream,
    void *userdata) {
    struct aws_h2_connection *connection = userdata;

    /* A receiver that receives a flow-controlled frame MUST always account for its contribution against the connection
     * flow-control window, unless the receiver treats this as a connection error */
    if (aws_sub_size_checked(
            connection->thread_data.window_size_self, payload_len, &connection->thread_data.window_size_self)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "DATA length %" PRIu32 " exceeds flow-control window %zu",
            payload_len,
            connection->thread_data.window_size_self);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR);
    }

    struct aws_h2_stream *stream;
    struct aws_h2err err = s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_DATA, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_data_begin(stream, payload_len, total_padding_bytes, end_stream);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }
    /* Handle automatic updates of the connection flow window */
    uint32_t auto_window_update;
    if (connection->conn_manual_window_management) {
        /* Automatically update the flow-window to account for padding, even though "manual window management"
         * is enabled. We do this because the current API doesn't have any way to inform the user about padding,
         * so we can't expect them to manage it themselves. */
        auto_window_update = total_padding_bytes;
    } else {
        /* Automatically update the full amount we just received */
        auto_window_update = payload_len;
    }

    if (auto_window_update != 0) {
        if (s_connection_send_update_window(connection, auto_window_update)) {
            return aws_h2err_from_last_error();
        }
        CONNECTION_LOGF(
            TRACE,
            connection,
            "Automatically updating connection window by %" PRIu32 "(%" PRIu32 " due to padding).",
            auto_window_update,
            total_padding_bytes);
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_data_i(uint32_t stream_id, struct aws_byte_cursor data, void *userdata) {
    struct aws_h2_connection *connection = userdata;

    /* Pass data to stream */
    struct aws_h2_stream *stream;
    struct aws_h2err err = s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_DATA, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_data_i(stream, data);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_end_stream(uint32_t stream_id, void *userdata) {
    struct aws_h2_connection *connection = userdata;

    /* Not calling s_get_active_stream_for_incoming_frame() here because END_STREAM
     * isn't an actual frame type. It's a flag on DATA or HEADERS frames, and we
     * already checked the legality of those frames in their respective callbacks. */

    struct aws_hash_element *found = NULL;
    aws_hash_table_find(&connection->thread_data.active_streams_map, (void *)(size_t)stream_id, &found);
    if (found) {
        struct aws_h2_stream *stream = found->value;
        struct aws_h2err err = aws_h2_stream_on_decoder_end_stream(stream);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

static struct aws_h2err s_decoder_on_rst_stream(uint32_t stream_id, uint32_t h2_error_code, void *userdata) {
    struct aws_h2_connection *connection = userdata;

    /* Pass RST_STREAM to stream */
    struct aws_h2_stream *stream;
    struct aws_h2err err =
        s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_RST_STREAM, &stream);
    if (aws_h2err_failed(err)) {
        return err;
    }

    if (stream) {
        err = aws_h2_stream_on_decoder_rst_stream(stream, h2_error_code);
        if (aws_h2err_failed(err)) {
            return err;
        }
    }

    return AWS_H2ERR_SUCCESS;
}

static struct aws_h2err s_decoder_on_ping_ack(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata) {
    struct aws_h2_connection *connection = userdata;
    if (aws_linked_list_empty(&connection->thread_data.pending_ping_queue)) {
        CONNECTION_LOG(ERROR, connection, "Received extraneous PING ACK.");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }
    struct aws_h2err err;
    struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->thread_data.pending_ping_queue);
    struct aws_h2_pending_ping *pending_ping = AWS_CONTAINER_OF(node, struct aws_h2_pending_ping, node);
    /* Check the payload */
    if (!aws_array_eq(opaque_data, AWS_HTTP2_PING_DATA_SIZE, pending_ping->opaque_data, AWS_HTTP2_PING_DATA_SIZE)) {
        CONNECTION_LOG(ERROR, connection, "Received PING ACK with mismatched opaque-data.");
        err = aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        goto error;
    }
    uint64_t time_stamp;
    if (aws_high_res_clock_get_ticks(&time_stamp)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failed getting the time stamp when PING ACK received, error %s",
            aws_error_name(aws_last_error()));
        err = aws_h2err_from_last_error();
        goto error;
    }
    uint64_t rtt;
    if (aws_sub_u64_checked(time_stamp, pending_ping->started_time, &rtt)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Overflow from time stamp when PING ACK received, error %s",
            aws_error_name(aws_last_error()));
        err = aws_h2err_from_last_error();
        goto error;
    }
    CONNECTION_LOGF(TRACE, connection, "Round trip time is %lf ms, approximately", (double)rtt / 1000000);
    /* fire the callback */
    if (pending_ping->on_completed) {
        pending_ping->on_completed(&connection->base, rtt, AWS_ERROR_SUCCESS, pending_ping->user_data);
    }
    aws_mem_release(connection->base.alloc, pending_ping);
    return AWS_H2ERR_SUCCESS;
error:
    if (pending_ping->on_completed) {
        pending_ping->on_completed(&connection->base, 0 /* fake rtt */, err.aws_code, pending_ping->user_data);
    }
    aws_mem_release(connection->base.alloc, pending_ping);
    return err;
}

static struct aws_h2err s_decoder_on_ping(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata) {
    struct aws_h2_connection *connection = userdata;

    /* send a PING frame with the ACK flag set in response, with an identical payload. */
    struct aws_h2_frame *ping_ack_frame = aws_h2_frame_new_ping(connection->base.alloc, true, opaque_data);
    if (!ping_ack_frame) {
        CONNECTION_LOGF(
            ERROR, connection, "Ping ACK frame failed to be sent, error %s", aws_error_name(aws_last_error()));
        return aws_h2err_from_last_error();
    }

    aws_h2_connection_enqueue_outgoing_frame(connection, ping_ack_frame);
    return AWS_H2ERR_SUCCESS;
}

static struct aws_h2err s_decoder_on_settings(
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    void *userdata) {
    struct aws_h2_connection *connection = userdata;
    struct aws_h2err err;
    /* Once all values have been processed, the recipient MUST immediately emit a SETTINGS frame with the ACK flag
     * set.(RFC-7540 6.5.3) */
    CONNECTION_LOG(TRACE, connection, "Setting frame processing ends");
    struct aws_h2_frame *settings_ack_frame = aws_h2_frame_new_settings(connection->base.alloc, NULL, 0, true);
    if (!settings_ack_frame) {
        CONNECTION_LOGF(
            ERROR, connection, "Settings ACK frame failed to be sent, error %s", aws_error_name(aws_last_error()));
        return aws_h2err_from_last_error();
    }
    aws_h2_connection_enqueue_outgoing_frame(connection, settings_ack_frame);

    /* Allocate a block of memory for settings_array in callback, which will only includes the settings we changed,
     * freed once the callback finished */
    struct aws_http2_setting *callback_array = NULL;
    if (num_settings) {
        callback_array = aws_mem_acquire(connection->base.alloc, num_settings * sizeof(struct aws_http2_setting));
        if (!callback_array) {
            return aws_h2err_from_last_error();
        }
    }
    size_t callback_array_num = 0;

    /* Apply the change to encoder and connection */
    struct aws_h2_frame_encoder *encoder = &connection->thread_data.encoder;
    for (size_t i = 0; i < num_settings; i++) {
        if (connection->thread_data.settings_peer[settings_array[i].id] == settings_array[i].value) {
            /* No change, don't do any work */
            continue;
        }
        switch (settings_array[i].id) {
            case AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE: {
                aws_h2_frame_encoder_set_setting_header_table_size(encoder, settings_array[i].value);
            } break;
            case AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE: {
                /* When the value of SETTINGS_INITIAL_WINDOW_SIZE changes, a receiver MUST adjust the size of all stream
                 * flow-control windows that it maintains by the difference between the new value and the old value. */
                int32_t size_changed =
                    settings_array[i].value - connection->thread_data.settings_peer[settings_array[i].id];
                struct aws_hash_iter stream_iter = aws_hash_iter_begin(&connection->thread_data.active_streams_map);
                while (!aws_hash_iter_done(&stream_iter)) {
                    struct aws_h2_stream *stream = stream_iter.element.value;
                    aws_hash_iter_next(&stream_iter);
                    err = aws_h2_stream_window_size_change(stream, size_changed, false /*self*/);
                    if (aws_h2err_failed(err)) {
                        CONNECTION_LOG(
                            ERROR,
                            connection,
                            "Connection error, change to SETTINGS_INITIAL_WINDOW_SIZE caused a stream's flow-control "
                            "window to exceed the maximum size");
                        goto error;
                    }
                }
            } break;
            case AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE: {
                aws_h2_frame_encoder_set_setting_max_frame_size(encoder, settings_array[i].value);
            } break;
            default:
                break;
        }
        connection->thread_data.settings_peer[settings_array[i].id] = settings_array[i].value;
        callback_array[callback_array_num++] = settings_array[i];
    }
    if (connection->on_remote_settings_change) {
        connection->on_remote_settings_change(
            &connection->base, callback_array, callback_array_num, connection->base.user_data);
    }
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        memcpy(
            connection->synced_data.settings_peer,
            connection->thread_data.settings_peer,
            sizeof(connection->thread_data.settings_peer));

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    aws_mem_release(connection->base.alloc, callback_array);
    return AWS_H2ERR_SUCCESS;
error:
    aws_mem_release(connection->base.alloc, callback_array);
    return err;
}

static struct aws_h2err s_decoder_on_settings_ack(void *userdata) {
    struct aws_h2_connection *connection = userdata;
    if (aws_linked_list_empty(&connection->thread_data.pending_settings_queue)) {
        CONNECTION_LOG(ERROR, connection, "Received a malicious extra SETTINGS acknowledgment");
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }
    struct aws_h2err err;
    struct aws_h2_pending_settings *pending_settings = NULL;
    struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->thread_data.pending_settings_queue);
    pending_settings = AWS_CONTAINER_OF(node, struct aws_h2_pending_settings, node);

    struct aws_http2_setting *settings_array = pending_settings->settings_array;
    /* Apply the settings */
    struct aws_h2_decoder *decoder = connection->thread_data.decoder;
    for (size_t i = 0; i < pending_settings->num_settings; i++) {
        if (connection->thread_data.settings_self[settings_array[i].id] == settings_array[i].value) {
            /* No change, don't do any work */
            continue;
        }
        switch (settings_array[i].id) {
            case AWS_HTTP2_SETTINGS_HEADER_TABLE_SIZE: {
                aws_h2_decoder_set_setting_header_table_size(decoder, settings_array[i].value);
            } break;
            case AWS_HTTP2_SETTINGS_ENABLE_PUSH: {
                aws_h2_decoder_set_setting_enable_push(decoder, settings_array[i].value);
            } break;
            case AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE: {
                /* When the value of SETTINGS_INITIAL_WINDOW_SIZE changes, a receiver MUST adjust the size of all stream
                 * flow-control windows that it maintains by the difference between the new value and the old value. */
                int32_t size_changed =
                    settings_array[i].value - connection->thread_data.settings_self[settings_array[i].id];
                struct aws_hash_iter stream_iter = aws_hash_iter_begin(&connection->thread_data.active_streams_map);
                while (!aws_hash_iter_done(&stream_iter)) {
                    struct aws_h2_stream *stream = stream_iter.element.value;
                    aws_hash_iter_next(&stream_iter);
                    err = aws_h2_stream_window_size_change(stream, size_changed, true /*self*/);
                    if (aws_h2err_failed(err)) {
                        CONNECTION_LOG(
                            ERROR,
                            connection,
                            "Connection error, change to SETTINGS_INITIAL_WINDOW_SIZE from internal caused a stream's "
                            "flow-control window to exceed the maximum size");
                        goto error;
                    }
                }
            } break;
            case AWS_HTTP2_SETTINGS_MAX_FRAME_SIZE: {
                aws_h2_decoder_set_setting_max_frame_size(decoder, settings_array[i].value);
            } break;
            default:
                break;
        }
        connection->thread_data.settings_self[settings_array[i].id] = settings_array[i].value;
    }
    /* invoke the change settings completed user callback */
    if (pending_settings->on_completed) {
        pending_settings->on_completed(&connection->base, AWS_ERROR_SUCCESS, pending_settings->user_data);
    }
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        memcpy(
            connection->synced_data.settings_self,
            connection->thread_data.settings_self,
            sizeof(connection->thread_data.settings_self));

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    /* clean up the pending_settings */
    aws_mem_release(connection->base.alloc, pending_settings);
    return AWS_H2ERR_SUCCESS;
error:
    /* invoke the user callback with error code */
    if (pending_settings->on_completed) {
        pending_settings->on_completed(&connection->base, err.aws_code, pending_settings->user_data);
    }
    /* clean up the pending settings here */
    aws_mem_release(connection->base.alloc, pending_settings);
    return err;
}

static struct aws_h2err s_decoder_on_window_update(uint32_t stream_id, uint32_t window_size_increment, void *userdata) {
    struct aws_h2_connection *connection = userdata;

    if (stream_id == 0) {
        /* Let's update the connection flow-control window size */
        if (window_size_increment == 0) {
            /* flow-control window increment of 0 MUST be treated as error (RFC7540 6.9.1) */
            CONNECTION_LOG(ERROR, connection, "Window update frame with 0 increment size");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
        }
        if (connection->thread_data.window_size_peer + window_size_increment > AWS_H2_WINDOW_UPDATE_MAX) {
            /* We MUST NOT allow a flow-control window to exceed the max */
            CONNECTION_LOG(
                ERROR,
                connection,
                "Window update frame causes the connection flow-control window exceeding the maximum size");
            return aws_h2err_from_h2_code(AWS_HTTP2_ERR_FLOW_CONTROL_ERROR);
        }
        if (connection->thread_data.window_size_peer <= AWS_H2_MIN_WINDOW_SIZE) {
            CONNECTION_LOGF(
                DEBUG,
                connection,
                "Peer connection's flow-control window is resumed from too small to %" PRIu32
                ". Connection will resume sending DATA.",
                window_size_increment);
        }
        connection->thread_data.window_size_peer += window_size_increment;
        return AWS_H2ERR_SUCCESS;
    } else {
        /* Update the flow-control window size for stream */
        struct aws_h2_stream *stream;
        bool window_resume;
        struct aws_h2err err =
            s_get_active_stream_for_incoming_frame(connection, stream_id, AWS_H2_FRAME_T_WINDOW_UPDATE, &stream);
        if (aws_h2err_failed(err)) {
            return err;
        }
        if (stream) {
            err = aws_h2_stream_on_decoder_window_update(stream, window_size_increment, &window_resume);
            if (aws_h2err_failed(err)) {
                return err;
            }
            if (window_resume) {
                /* Set the stream free from stalled list */
                AWS_H2_STREAM_LOGF(
                    DEBUG,
                    stream,
                    "Peer stream's flow-control window is resumed from 0 or negative to %" PRIu32
                    " Stream will resume sending data.",
                    stream->thread_data.window_size_peer);
                aws_linked_list_remove(&stream->node);
                aws_linked_list_push_back(&connection->thread_data.outgoing_streams_list, &stream->node);
            }
        }
    }
    return AWS_H2ERR_SUCCESS;
}

struct aws_h2err s_decoder_on_goaway(
    uint32_t last_stream,
    uint32_t error_code,
    struct aws_byte_cursor debug_data,
    void *userdata) {
    struct aws_h2_connection *connection = userdata;

    if (last_stream > connection->thread_data.goaway_received_last_stream_id) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Received GOAWAY with invalid last-stream-id=%" PRIu32 ", must not exceed previous last-stream-id=%" PRIu32,
            last_stream,
            connection->thread_data.goaway_received_last_stream_id);
        return aws_h2err_from_h2_code(AWS_HTTP2_ERR_PROTOCOL_ERROR);
    }
    /* stop sending any new stream and making new request */
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_GOAWAY_RECEIVED;
        connection->synced_data.goaway_received_last_stream_id = last_stream;
        connection->synced_data.goaway_received_http2_error_code = error_code;

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    connection->thread_data.goaway_received_last_stream_id = last_stream;
    CONNECTION_LOGF(
        DEBUG,
        connection,
        "Received GOAWAY error-code=%s(0x%x) last-stream-id=%" PRIu32,
        aws_http2_error_code_to_str(error_code),
        error_code,
        last_stream);
    /* Complete activated streams whose id is higher than last_stream, since they will not process by peer. We should
     * treat them as they had never been created at all.
     * This would be more efficient if we could iterate streams in reverse-id order */
    struct aws_hash_iter stream_iter = aws_hash_iter_begin(&connection->thread_data.active_streams_map);
    while (!aws_hash_iter_done(&stream_iter)) {
        struct aws_h2_stream *stream = stream_iter.element.value;
        aws_hash_iter_next(&stream_iter);
        if (stream->base.id > last_stream) {
            AWS_H2_STREAM_LOG(
                DEBUG,
                stream,
                "stream ID is higher than GOAWAY last stream ID, please retry this stream on a new connection.");
            s_stream_complete(connection, stream, AWS_ERROR_HTTP_GOAWAY_RECEIVED);
        }
    }
    if (connection->on_goaway_received) {
        /* Inform user about goaway received and the error code. */
        connection->on_goaway_received(
            &connection->base, last_stream, error_code, debug_data, connection->base.user_data);
    }

    return AWS_H2ERR_SUCCESS;
}

/* End decoder callbacks */

static int s_send_connection_preface_client_string(struct aws_h2_connection *connection) {

    /* Just send the magic string on its own aws_io_message. */
    struct aws_io_message *msg = aws_channel_acquire_message_from_pool(
        connection->base.channel_slot->channel,
        AWS_IO_MESSAGE_APPLICATION_DATA,
        aws_h2_connection_preface_client_string.len);
    if (!msg) {
        goto error;
    }

    if (!aws_byte_buf_write_from_whole_cursor(&msg->message_data, aws_h2_connection_preface_client_string)) {
        aws_raise_error(AWS_ERROR_INVALID_STATE);
        goto error;
    }

    if (aws_channel_slot_send_message(connection->base.channel_slot, msg, AWS_CHANNEL_DIR_WRITE)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    if (msg) {
        aws_mem_release(msg->allocator, msg);
    }
    return AWS_OP_ERR;
}

static void s_handler_installed(struct aws_channel_handler *handler, struct aws_channel_slot *slot) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(slot->channel));
    struct aws_h2_connection *connection = handler->impl;

    connection->base.channel_slot = slot;

    /* Acquire a hold on the channel to prevent its destruction until the user has
     * given the go-ahead via aws_http_connection_release() */
    aws_channel_acquire_hold(slot->channel);

    /* Send HTTP/2 connection preface (RFC-7540 3.5)
     * - clients must send magic string
     * - both client and server must send SETTINGS frame */

    if (connection->base.client_data) {
        if (s_send_connection_preface_client_string(connection)) {
            CONNECTION_LOGF(
                ERROR,
                connection,
                "Failed to send client connection preface string, %s",
                aws_error_name(aws_last_error()));
            goto error;
        }
    }
    struct aws_h2_pending_settings *init_pending_settings = connection->thread_data.init_pending_settings;
    aws_linked_list_push_back(&connection->thread_data.pending_settings_queue, &init_pending_settings->node);
    connection->thread_data.init_pending_settings = NULL;
    /* Set user_data here, the user_data is valid now */
    init_pending_settings->user_data = connection->base.user_data;

    struct aws_h2_frame *init_settings_frame = aws_h2_frame_new_settings(
        connection->base.alloc,
        init_pending_settings->settings_array,
        init_pending_settings->num_settings,
        false /*ACK*/);
    if (!init_settings_frame) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failed to create the initial settings frame, error %s",
            aws_error_name(aws_last_error()));
        aws_mem_release(connection->base.alloc, init_pending_settings);
        goto error;
    }
    /* enqueue the initial settings frame here */
    aws_linked_list_push_back(&connection->thread_data.outgoing_frames_queue, &init_settings_frame->node);

    /* If not manual connection window management, update the connection window to max. */
    if (!connection->conn_manual_window_management) {
        uint32_t initial_window_update_size = AWS_H2_WINDOW_UPDATE_MAX - AWS_H2_INIT_WINDOW_SIZE;
        struct aws_h2_frame *connection_window_update_frame =
            aws_h2_frame_new_window_update(connection->base.alloc, 0 /* stream_id */, initial_window_update_size);
        AWS_ASSERT(connection_window_update_frame);
        /* enqueue the windows update frame here */
        aws_linked_list_push_back(
            &connection->thread_data.outgoing_frames_queue, &connection_window_update_frame->node);
        connection->thread_data.window_size_self += initial_window_update_size;
    }
    aws_h2_try_write_outgoing_frames(connection);
    return;

error:
    aws_h2_connection_shutdown_due_to_write_err(connection, aws_last_error());
}

static void s_stream_complete(struct aws_h2_connection *connection, struct aws_h2_stream *stream, int error_code) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    /* Nice logging */
    if (error_code) {
        AWS_H2_STREAM_LOGF(
            ERROR, stream, "Stream completed with error %d (%s).", error_code, aws_error_name(error_code));
    } else if (stream->base.client_data) {
        int status = stream->base.client_data->response_status;
        AWS_H2_STREAM_LOGF(
            DEBUG, stream, "Client stream complete, response status %d (%s)", status, aws_http_status_text(status));
    } else {
        AWS_H2_STREAM_LOG(DEBUG, stream, "Server stream complete");
    }

    /* Remove stream from active_streams_map and outgoing_stream_list (if it was in them at all) */
    aws_hash_table_remove(&connection->thread_data.active_streams_map, (void *)(size_t)stream->base.id, NULL, NULL);
    if (stream->node.next) {
        aws_linked_list_remove(&stream->node);
    }

    if (aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) == 0 &&
        connection->thread_data.incoming_timestamp_ns != 0) {
        uint64_t now_ns = 0;
        aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns);
        /* transition from something to read -> nothing to read and nothing to write */
        s_add_time_measurement_to_stats(
            connection->thread_data.incoming_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_incoming_stream_ms);
        connection->thread_data.stats.was_inactive = true;
        connection->thread_data.incoming_timestamp_ns = 0;
    }

    aws_h2_stream_complete(stream, error_code);

    /* release connection's hold on stream */
    aws_http_stream_release(&stream->base);
}

int aws_h2_connection_on_stream_closed(
    struct aws_h2_connection *connection,
    struct aws_h2_stream *stream,
    enum aws_h2_stream_closed_when closed_when,
    int aws_error_code) {

    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_PRECONDITION(stream->thread_data.state == AWS_H2_STREAM_STATE_CLOSED);
    AWS_PRECONDITION(stream->base.id != 0);

    uint32_t stream_id = stream->base.id;

    /* Mark stream complete. This removes the stream from any "active" datastructures,
     * invokes its completion callback, and releases its refcount. */
    s_stream_complete(connection, stream, aws_error_code);
    stream = NULL; /* Reference released, do not touch again */

    if (s_record_closed_stream(connection, stream_id, closed_when)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_record_closed_stream(
    struct aws_h2_connection *connection,
    uint32_t stream_id,
    enum aws_h2_stream_closed_when closed_when) {

    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (aws_cache_put(connection->thread_data.closed_streams, (void *)(size_t)stream_id, (void *)(size_t)closed_when)) {
        CONNECTION_LOG(ERROR, connection, "Failed inserting ID into cache of recently closed streams");
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_h2_connection_send_rst_and_close_reserved_stream(
    struct aws_h2_connection *connection,
    uint32_t stream_id,
    uint32_t h2_error_code) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    struct aws_h2_frame *rst_stream = aws_h2_frame_new_rst_stream(connection->base.alloc, stream_id, h2_error_code);
    if (!rst_stream) {
        CONNECTION_LOGF(ERROR, connection, "Error creating RST_STREAM frame, %s", aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }
    aws_h2_connection_enqueue_outgoing_frame(connection, rst_stream);

    /* If we ever fully support PUSH_PROMISE, this is where we'd remove the
     * promised_stream_id from some reserved_streams datastructure */

    return s_record_closed_stream(connection, stream_id, AWS_H2_STREAM_CLOSED_WHEN_RST_STREAM_SENT);
}

/* Move stream into "active" datastructures and notify stream that it can send frames now */
static void s_move_stream_to_thread(
    struct aws_h2_connection *connection,
    struct aws_h2_stream *stream,
    int new_stream_error_code) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    if (new_stream_error_code) {
        aws_raise_error(new_stream_error_code);
        AWS_H2_STREAM_LOGF(
            ERROR,
            stream,
            "Failed activating stream, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    uint32_t max_concurrent_streams = connection->thread_data.settings_peer[AWS_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS];
    if (aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) >= max_concurrent_streams) {
        AWS_H2_STREAM_LOG(ERROR, stream, "Failed activating stream, max concurrent streams are reached");
        aws_raise_error(AWS_ERROR_HTTP_MAX_CONCURRENT_STREAMS_EXCEEDED);
        goto error;
    }

    if (aws_hash_table_put(
            &connection->thread_data.active_streams_map, (void *)(size_t)stream->base.id, stream, NULL)) {
        AWS_H2_STREAM_LOG(ERROR, stream, "Failed inserting stream into map");
        goto error;
    }

    enum aws_h2_stream_body_state body_state = AWS_H2_STREAM_BODY_STATE_NONE;
    if (aws_h2_stream_on_activated(stream, &body_state)) {
        goto error;
    }

    if (aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) == 1) {
        /* transition from nothing to read -> something to read */
        uint64_t now_ns = 0;
        aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns);
        connection->thread_data.incoming_timestamp_ns = now_ns;
    }

    switch (body_state) {
        case AWS_H2_STREAM_BODY_STATE_WAITING_WRITES:
            aws_linked_list_push_back(&connection->thread_data.waiting_streams_list, &stream->node);
            break;
        case AWS_H2_STREAM_BODY_STATE_ONGOING:
            aws_linked_list_push_back(&connection->thread_data.outgoing_streams_list, &stream->node);
            break;
        default:
            break;
    }
    return;
error:
    /* If the stream got into any datastructures, s_stream_complete() will remove it */
    s_stream_complete(connection, stream, aws_last_error());
}

/* Perform on-thread work that is triggered by calls to the connection/stream API */
static void s_cross_thread_work_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_h2_connection *connection = arg;

    struct aws_linked_list pending_frames;
    aws_linked_list_init(&pending_frames);

    struct aws_linked_list pending_streams;
    aws_linked_list_init(&pending_streams);

    struct aws_linked_list pending_settings;
    aws_linked_list_init(&pending_settings);

    struct aws_linked_list pending_ping;
    aws_linked_list_init(&pending_ping);

    struct aws_linked_list pending_goaway;
    aws_linked_list_init(&pending_goaway);

    size_t window_update_size;
    int new_stream_error_code;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        connection->synced_data.is_cross_thread_work_task_scheduled = false;

        aws_linked_list_swap_contents(&connection->synced_data.pending_frame_list, &pending_frames);
        aws_linked_list_swap_contents(&connection->synced_data.pending_stream_list, &pending_streams);
        aws_linked_list_swap_contents(&connection->synced_data.pending_settings_list, &pending_settings);
        aws_linked_list_swap_contents(&connection->synced_data.pending_ping_list, &pending_ping);
        aws_linked_list_swap_contents(&connection->synced_data.pending_goaway_list, &pending_goaway);
        window_update_size = connection->synced_data.window_update_size;
        connection->synced_data.window_update_size = 0;
        new_stream_error_code = connection->synced_data.new_stream_error_code;

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    /* Enqueue new pending control frames */
    while (!aws_linked_list_empty(&pending_frames)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&pending_frames);
        struct aws_h2_frame *frame = AWS_CONTAINER_OF(node, struct aws_h2_frame, node);
        aws_h2_connection_enqueue_outgoing_frame(connection, frame);
    }

    /* We already enqueued the window_update frame, just apply the change and let our peer check this value, no matter
     * overflow happens or not. Peer will detect it for us. */
    connection->thread_data.window_size_self =
        aws_add_size_saturating(connection->thread_data.window_size_self, window_update_size);

    /* Process new pending_streams */
    while (!aws_linked_list_empty(&pending_streams)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&pending_streams);
        struct aws_h2_stream *stream = AWS_CONTAINER_OF(node, struct aws_h2_stream, node);
        s_move_stream_to_thread(connection, stream, new_stream_error_code);
    }

    /* Move pending settings to thread data */
    while (!aws_linked_list_empty(&pending_settings)) {
        aws_linked_list_push_back(
            &connection->thread_data.pending_settings_queue, aws_linked_list_pop_front(&pending_settings));
    }

    /* Move pending PING to thread data */
    while (!aws_linked_list_empty(&pending_ping)) {
        aws_linked_list_push_back(
            &connection->thread_data.pending_ping_queue, aws_linked_list_pop_front(&pending_ping));
    }

    /* Send user requested goaways */
    while (!aws_linked_list_empty(&pending_goaway)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&pending_goaway);
        struct aws_h2_pending_goaway *goaway = AWS_CONTAINER_OF(node, struct aws_h2_pending_goaway, node);
        s_send_goaway(connection, goaway->http2_error, goaway->allow_more_streams, &goaway->debug_data);
        aws_mem_release(connection->base.alloc, goaway);
    }

    /* It's likely that frames were queued while processing cross-thread work.
     * If so, try writing them now */
    aws_h2_try_write_outgoing_frames(connection);
}

int aws_h2_stream_activate(struct aws_http_stream *stream) {
    struct aws_h2_stream *h2_stream = AWS_CONTAINER_OF(stream, struct aws_h2_stream, base);

    struct aws_http_connection *base_connection = stream->owning_connection;
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(base_connection, struct aws_h2_connection, base);

    int err;
    bool was_cross_thread_work_scheduled = false;
    { /* BEGIN CRITICAL SECTION */
        s_acquire_stream_and_connection_lock(h2_stream, connection);

        if (stream->id) {
            /* stream has already been activated. */
            s_release_stream_and_connection_lock(h2_stream, connection);
            return AWS_OP_SUCCESS;
        }

        err = connection->synced_data.new_stream_error_code;
        if (err) {
            s_release_stream_and_connection_lock(h2_stream, connection);
            goto error;
        }

        stream->id = aws_http_connection_get_next_stream_id(base_connection);

        if (stream->id) {
            /* success */
            was_cross_thread_work_scheduled = connection->synced_data.is_cross_thread_work_task_scheduled;
            connection->synced_data.is_cross_thread_work_task_scheduled = true;

            aws_linked_list_push_back(&connection->synced_data.pending_stream_list, &h2_stream->node);
            h2_stream->synced_data.api_state = AWS_H2_STREAM_API_STATE_ACTIVE;
        }

        s_release_stream_and_connection_lock(h2_stream, connection);
    } /* END CRITICAL SECTION */

    if (!stream->id) {
        /* aws_http_connection_get_next_stream_id() raises its own error. */
        return AWS_OP_ERR;
    }

    /* connection keeps activated stream alive until stream completes */
    aws_atomic_fetch_add(&stream->refcount, 1);
    stream->metrics.stream_id = stream->id;

    if (!was_cross_thread_work_scheduled) {
        CONNECTION_LOG(TRACE, connection, "Scheduling cross-thread work task");
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    }

    return AWS_OP_SUCCESS;

error:
    CONNECTION_LOGF(
        ERROR,
        connection,
        "Failed to activate the stream id=%p, new streams are not allowed now. error %d (%s)",
        (void *)stream,
        err,
        aws_error_name(err));
    return aws_raise_error(err);
}

static struct aws_http_stream *s_connection_make_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(client_connection, struct aws_h2_connection, base);

    /* #TODO: http/2-ify the request (ex: add ":method" header). Should we mutate a copy or the original? Validate?
     *  Or just pass pointer to headers struct and let encoder transform it while encoding? */

    struct aws_h2_stream *stream = aws_h2_stream_new_request(client_connection, options);
    if (!stream) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failed to create stream, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        return NULL;
    }

    int new_stream_error_code;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        new_stream_error_code = connection->synced_data.new_stream_error_code;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    if (new_stream_error_code) {
        aws_raise_error(new_stream_error_code);
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Cannot create request stream, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    AWS_H2_STREAM_LOG(DEBUG, stream, "Created HTTP/2 request stream"); /* #TODO: print method & path */
    return &stream->base;

error:
    /* Force destruction of the stream, avoiding ref counting */
    stream->base.vtable->destroy(&stream->base);
    return NULL;
}

static void s_connection_close(struct aws_http_connection *connection_base) {
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);

    /* Don't stop reading/writing immediately, let that happen naturally during the channel shutdown process. */
    s_stop(connection, false /*stop_reading*/, false /*stop_writing*/, true /*schedule_shutdown*/, AWS_ERROR_SUCCESS);
}

static void s_connection_stop_new_request(struct aws_http_connection *connection_base) {
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);

    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        if (!connection->synced_data.new_stream_error_code) {
            connection->synced_data.new_stream_error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
        }
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
}

static bool s_connection_is_open(const struct aws_http_connection *connection_base) {
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    bool is_open;

    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        is_open = connection->synced_data.is_open;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    return is_open;
}

static bool s_connection_new_requests_allowed(const struct aws_http_connection *connection_base) {
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    int new_stream_error_code;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        new_stream_error_code = connection->synced_data.new_stream_error_code;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    return new_stream_error_code == 0;
}

static void s_connection_update_window(struct aws_http_connection *connection_base, uint32_t increment_size) {
    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    if (!increment_size) {
        /* Silently do nothing. */
        return;
    }
    if (!connection->conn_manual_window_management) {
        /* auto-mode, manual update window is not supported, silently do nothing with warning log. */
        CONNECTION_LOG(
            DEBUG,
            connection,
            "Connection manual window management is off, update window operations are not supported.");
        return;
    }
    struct aws_h2_frame *connection_window_update_frame =
        aws_h2_frame_new_window_update(connection->base.alloc, 0, increment_size);
    if (!connection_window_update_frame) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failed to create WINDOW_UPDATE frame on connection, error %s",
            aws_error_name(aws_last_error()));
        /* OOM should result in a crash. And the increment size is too huge is the only other failure case, which will
         * result in overflow. */
        goto overflow;
    }

    int err = 0;
    bool cross_thread_work_should_schedule = false;
    bool connection_open = false;
    size_t sum_size = 0;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        err |= aws_add_size_checked(connection->synced_data.window_update_size, increment_size, &sum_size);
        err |= sum_size > AWS_H2_WINDOW_UPDATE_MAX;
        connection_open = connection->synced_data.is_open;

        if (!err && connection_open) {
            cross_thread_work_should_schedule = !connection->synced_data.is_cross_thread_work_task_scheduled;
            connection->synced_data.is_cross_thread_work_task_scheduled = true;
            aws_linked_list_push_back(
                &connection->synced_data.pending_frame_list, &connection_window_update_frame->node);
            connection->synced_data.window_update_size = sum_size;
        }
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    if (err) {
        CONNECTION_LOG(
            ERROR,
            connection,
            "The connection's flow-control windows has been incremented beyond 2**31 -1, the max for HTTP/2. The ");
        aws_h2_frame_destroy(connection_window_update_frame);
        goto overflow;
    }

    if (cross_thread_work_should_schedule) {
        CONNECTION_LOG(TRACE, connection, "Scheduling cross-thread work task");
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    }

    if (!connection_open) {
        /* connection already closed, just do nothing */
        aws_h2_frame_destroy(connection_window_update_frame);
        return;
    }
    CONNECTION_LOGF(
        TRACE,
        connection,
        "User requested to update the HTTP/2 connection's flow-control windows by %" PRIu32 ".",
        increment_size);
    return;
overflow:
    /* Shutdown the connection as overflow detected */
    s_stop(
        connection,
        false /*stop_reading*/,
        false /*stop_writing*/,
        true /*schedule_shutdown*/,
        AWS_ERROR_OVERFLOW_DETECTED);
}

static int s_connection_change_settings(
    struct aws_http_connection *connection_base,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    aws_http2_on_change_settings_complete_fn *on_completed,
    void *user_data) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);

    if (!settings_array && num_settings) {
        CONNECTION_LOG(ERROR, connection, "Settings_array is NULL and num_settings is not zero.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_h2_pending_settings *pending_settings =
        s_new_pending_settings(connection->base.alloc, settings_array, num_settings, on_completed, user_data);
    if (!pending_settings) {
        return AWS_OP_ERR;
    }
    struct aws_h2_frame *settings_frame =
        aws_h2_frame_new_settings(connection->base.alloc, settings_array, num_settings, false /*ACK*/);
    if (!settings_frame) {
        CONNECTION_LOGF(
            ERROR, connection, "Failed to create settings frame, error %s", aws_error_name(aws_last_error()));
        aws_mem_release(connection->base.alloc, pending_settings);
        return AWS_OP_ERR;
    }

    bool was_cross_thread_work_scheduled = false;
    bool connection_open;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        connection_open = connection->synced_data.is_open;
        if (!connection_open) {
            s_unlock_synced_data(connection);
            goto closed;
        }
        was_cross_thread_work_scheduled = connection->synced_data.is_cross_thread_work_task_scheduled;
        connection->synced_data.is_cross_thread_work_task_scheduled = true;
        aws_linked_list_push_back(&connection->synced_data.pending_frame_list, &settings_frame->node);
        aws_linked_list_push_back(&connection->synced_data.pending_settings_list, &pending_settings->node);

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (!was_cross_thread_work_scheduled) {
        CONNECTION_LOG(TRACE, connection, "Scheduling cross-thread work task");
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    }

    return AWS_OP_SUCCESS;
closed:
    CONNECTION_LOG(ERROR, connection, "Failed to change settings, connection is closed or closing.");
    aws_h2_frame_destroy(settings_frame);
    aws_mem_release(connection->base.alloc, pending_settings);
    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

static int s_connection_send_ping(
    struct aws_http_connection *connection_base,
    const struct aws_byte_cursor *optional_opaque_data,
    aws_http2_on_ping_complete_fn *on_completed,
    void *user_data) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    if (optional_opaque_data && optional_opaque_data->len != 8) {
        CONNECTION_LOG(ERROR, connection, "Only 8 bytes opaque data supported for PING in HTTP/2");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }
    uint64_t time_stamp;
    if (aws_high_res_clock_get_ticks(&time_stamp)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failed getting the time stamp to start PING, error %s",
            aws_error_name(aws_last_error()));
        return AWS_OP_ERR;
    }
    struct aws_h2_pending_ping *pending_ping =
        s_new_pending_ping(connection->base.alloc, optional_opaque_data, time_stamp, user_data, on_completed);
    if (!pending_ping) {
        return AWS_OP_ERR;
    }
    struct aws_h2_frame *ping_frame =
        aws_h2_frame_new_ping(connection->base.alloc, false /*ACK*/, pending_ping->opaque_data);
    if (!ping_frame) {
        CONNECTION_LOGF(ERROR, connection, "Failed to create PING frame, error %s", aws_error_name(aws_last_error()));
        aws_mem_release(connection->base.alloc, pending_ping);
        return AWS_OP_ERR;
    }

    bool was_cross_thread_work_scheduled = false;
    bool connection_open;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        connection_open = connection->synced_data.is_open;
        if (!connection_open) {
            s_unlock_synced_data(connection);
            goto closed;
        }
        was_cross_thread_work_scheduled = connection->synced_data.is_cross_thread_work_task_scheduled;
        connection->synced_data.is_cross_thread_work_task_scheduled = true;
        aws_linked_list_push_back(&connection->synced_data.pending_frame_list, &ping_frame->node);
        aws_linked_list_push_back(&connection->synced_data.pending_ping_list, &pending_ping->node);

        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (!was_cross_thread_work_scheduled) {
        CONNECTION_LOG(TRACE, connection, "Scheduling cross-thread work task");
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    }

    return AWS_OP_SUCCESS;

closed:
    CONNECTION_LOG(ERROR, connection, "Failed to send ping, connection is closed or closing.");
    aws_h2_frame_destroy(ping_frame);
    aws_mem_release(connection->base.alloc, pending_ping);
    return aws_raise_error(AWS_ERROR_INVALID_STATE);
}

static void s_connection_send_goaway(
    struct aws_http_connection *connection_base,
    uint32_t http2_error,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    struct aws_h2_pending_goaway *pending_goaway =
        s_new_pending_goaway(connection->base.alloc, http2_error, allow_more_streams, optional_debug_data);

    bool was_cross_thread_work_scheduled = false;
    bool connection_open;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);

        connection_open = connection->synced_data.is_open;
        if (!connection_open) {
            s_unlock_synced_data(connection);
            CONNECTION_LOG(DEBUG, connection, "Goaway not sent, connection is closed or closing.");
            aws_mem_release(connection->base.alloc, pending_goaway);
            return;
        }
        was_cross_thread_work_scheduled = connection->synced_data.is_cross_thread_work_task_scheduled;
        connection->synced_data.is_cross_thread_work_task_scheduled = true;
        aws_linked_list_push_back(&connection->synced_data.pending_goaway_list, &pending_goaway->node);
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (allow_more_streams && (http2_error != AWS_HTTP2_ERR_NO_ERROR)) {
        CONNECTION_LOGF(
            DEBUG,
            connection,
            "Send goaway with allow more streams on and non-zero error code %s(0x%x)",
            aws_http2_error_code_to_str(http2_error),
            http2_error);
    }

    if (!was_cross_thread_work_scheduled) {
        CONNECTION_LOG(TRACE, connection, "Scheduling cross-thread work task");
        aws_channel_schedule_task_now(connection->base.channel_slot->channel, &connection->cross_thread_work_task);
    }
}

static void s_get_settings_general(
    const struct aws_http_connection *connection_base,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT],
    bool local) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    uint32_t synced_settings[AWS_HTTP2_SETTINGS_END_RANGE];
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        if (local) {
            memcpy(
                synced_settings, connection->synced_data.settings_self, sizeof(connection->synced_data.settings_self));
        } else {
            memcpy(
                synced_settings, connection->synced_data.settings_peer, sizeof(connection->synced_data.settings_peer));
        }
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    for (int i = AWS_HTTP2_SETTINGS_BEGIN_RANGE; i < AWS_HTTP2_SETTINGS_END_RANGE; i++) {
        /* settings range begin with 1, store them into 0-based array of aws_http2_setting */
        out_settings[i - 1].id = i;
        out_settings[i - 1].value = synced_settings[i];
    }
    return;
}

static void s_connection_get_local_settings(
    const struct aws_http_connection *connection_base,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]) {
    s_get_settings_general(connection_base, out_settings, true /*local*/);
}

static void s_connection_get_remote_settings(
    const struct aws_http_connection *connection_base,
    struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]) {
    s_get_settings_general(connection_base, out_settings, false /*local*/);
}

/* Send a GOAWAY with the lowest possible last-stream-id or graceful shutdown warning */
static void s_send_goaway(
    struct aws_h2_connection *connection,
    uint32_t h2_error_code,
    bool allow_more_streams,
    const struct aws_byte_cursor *optional_debug_data) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    uint32_t last_stream_id = allow_more_streams ? AWS_H2_STREAM_ID_MAX
                                                 : aws_min_u32(
                                                       connection->thread_data.latest_peer_initiated_stream_id,
                                                       connection->thread_data.goaway_sent_last_stream_id);

    if (last_stream_id > connection->thread_data.goaway_sent_last_stream_id) {
        CONNECTION_LOG(
            DEBUG,
            connection,
            "GOAWAY frame with lower last stream id has been sent, ignoring sending graceful shutdown warning.");
        return;
    }

    struct aws_byte_cursor debug_data;
    AWS_ZERO_STRUCT(debug_data);
    if (optional_debug_data) {
        debug_data = *optional_debug_data;
    }

    struct aws_h2_frame *goaway =
        aws_h2_frame_new_goaway(connection->base.alloc, last_stream_id, h2_error_code, debug_data);
    if (!goaway) {
        CONNECTION_LOGF(ERROR, connection, "Error creating GOAWAY frame, %s", aws_error_name(aws_last_error()));
        goto error;
    }

    connection->thread_data.goaway_sent_last_stream_id = last_stream_id;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        connection->synced_data.goaway_sent_last_stream_id = last_stream_id;
        connection->synced_data.goaway_sent_http2_error_code = h2_error_code;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    aws_h2_connection_enqueue_outgoing_frame(connection, goaway);
    return;

error:
    aws_h2_connection_shutdown_due_to_write_err(connection, aws_last_error());
}

static int s_connection_get_sent_goaway(
    struct aws_http_connection *connection_base,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    uint32_t sent_last_stream_id;
    uint32_t sent_http2_error;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        sent_last_stream_id = connection->synced_data.goaway_sent_last_stream_id;
        sent_http2_error = connection->synced_data.goaway_sent_http2_error_code;
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    uint32_t max_stream_id = AWS_H2_STREAM_ID_MAX;
    if (sent_last_stream_id == max_stream_id + 1) {
        CONNECTION_LOG(ERROR, connection, "No GOAWAY has been sent so far.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    *out_http2_error = sent_http2_error;
    *out_last_stream_id = sent_last_stream_id;
    return AWS_OP_SUCCESS;
}

static int s_connection_get_received_goaway(
    struct aws_http_connection *connection_base,
    uint32_t *out_http2_error,
    uint32_t *out_last_stream_id) {

    struct aws_h2_connection *connection = AWS_CONTAINER_OF(connection_base, struct aws_h2_connection, base);
    uint32_t received_last_stream_id = 0;
    uint32_t received_http2_error = 0;
    bool goaway_not_ready = false;
    uint32_t max_stream_id = AWS_H2_STREAM_ID_MAX;
    { /* BEGIN CRITICAL SECTION */
        s_lock_synced_data(connection);
        if (connection->synced_data.goaway_received_last_stream_id == max_stream_id + 1) {
            goaway_not_ready = true;
        } else {
            received_last_stream_id = connection->synced_data.goaway_received_last_stream_id;
            received_http2_error = connection->synced_data.goaway_received_http2_error_code;
        }
        s_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (goaway_not_ready) {
        CONNECTION_LOG(ERROR, connection, "No GOAWAY has been received so far.");
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    *out_http2_error = received_http2_error;
    *out_last_stream_id = received_last_stream_id;
    return AWS_OP_SUCCESS;
}

static int s_handler_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)slot;
    struct aws_h2_connection *connection = handler->impl;

    CONNECTION_LOGF(TRACE, connection, "Begin processing message of size %zu.", message->message_data.len);

    if (connection->thread_data.is_reading_stopped) {
        CONNECTION_LOG(ERROR, connection, "Cannot process message because connection is shutting down.");
        goto clean_up;
    }

    /* Any error that bubbles up from the decoder or its callbacks is treated as
     * a Connection Error (a GOAWAY frames is sent, and the connection is closed) */
    struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&message->message_data);
    struct aws_h2err err = aws_h2_decode(connection->thread_data.decoder, &message_cursor);
    if (aws_h2err_failed(err)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Failure while receiving frames, %s. Sending GOAWAY %s(0x%x) and closing connection",
            aws_error_name(err.aws_code),
            aws_http2_error_code_to_str(err.h2_code),
            err.h2_code);
        goto shutdown;
    }

    /* HTTP/2 protocol uses WINDOW_UPDATE frames to coordinate data rates with peer,
     * so we can just keep the aws_channel's read-window wide open */
    if (aws_channel_slot_increment_read_window(slot, message->message_data.len)) {
        CONNECTION_LOGF(
            ERROR,
            connection,
            "Incrementing read window failed, error %d (%s). Closing connection",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        err = aws_h2err_from_last_error();
        goto shutdown;
    }

    goto clean_up;

shutdown:
    s_send_goaway(connection, err.h2_code, false /*allow_more_streams*/, NULL /*optional_debug_data*/);
    aws_h2_try_write_outgoing_frames(connection);
    s_stop(connection, true /*stop_reading*/, false /*stop_writing*/, true /*schedule_shutdown*/, err.aws_code);

clean_up:
    aws_mem_release(message->allocator, message);

    /* Flush any outgoing frames that might have been queued as a result of decoder callbacks. */
    aws_h2_try_write_outgoing_frames(connection);

    return AWS_OP_SUCCESS;
}

static int s_handler_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    (void)handler;
    (void)slot;
    (void)message;
    return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
}

static int s_handler_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {

    (void)handler;
    (void)slot;
    (void)size;
    return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
}

static int s_handler_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {

    struct aws_h2_connection *connection = handler->impl;
    CONNECTION_LOGF(
        TRACE,
        connection,
        "Channel shutting down in %s direction with error code %d (%s).",
        (dir == AWS_CHANNEL_DIR_READ) ? "read" : "write",
        error_code,
        aws_error_name(error_code));

    if (dir == AWS_CHANNEL_DIR_READ) {
        /* This call ensures that no further streams will be created. */
        s_stop(connection, true /*stop_reading*/, false /*stop_writing*/, false /*schedule_shutdown*/, error_code);
        /* Send user requested GOAWAY, if they haven't been sent before. It's OK to access
         * synced_data.pending_goaway_list without holding the lock because no more user_requested GOAWAY can be added
         * after s_stop() has been invoked. */
        if (!aws_linked_list_empty(&connection->synced_data.pending_goaway_list)) {
            while (!aws_linked_list_empty(&connection->synced_data.pending_goaway_list)) {
                struct aws_linked_list_node *node =
                    aws_linked_list_pop_front(&connection->synced_data.pending_goaway_list);
                struct aws_h2_pending_goaway *goaway = AWS_CONTAINER_OF(node, struct aws_h2_pending_goaway, node);
                s_send_goaway(connection, goaway->http2_error, goaway->allow_more_streams, &goaway->debug_data);
                aws_mem_release(connection->base.alloc, goaway);
            }
            aws_h2_try_write_outgoing_frames(connection);
        }

        /* Send GOAWAY if none have been sent so far,
         * or if we've only sent a "graceful shutdown warning" that didn't name a last-stream-id */
        if (connection->thread_data.goaway_sent_last_stream_id == AWS_H2_STREAM_ID_MAX) {
            s_send_goaway(
                connection,
                error_code ? AWS_HTTP2_ERR_INTERNAL_ERROR : AWS_HTTP2_ERR_NO_ERROR,
                false /*allow_more_streams*/,
                NULL /*optional_debug_data*/);
            aws_h2_try_write_outgoing_frames(connection);
        }
        aws_channel_slot_on_handler_shutdown_complete(
            slot, AWS_CHANNEL_DIR_READ, error_code, free_scarce_resources_immediately);

    } else /* AWS_CHANNEL_DIR_WRITE */ {
        connection->thread_data.channel_shutdown_error_code = error_code;
        connection->thread_data.channel_shutdown_immediately = free_scarce_resources_immediately;
        connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written = true;

        /* We'd prefer to wait until we know GOAWAY has been written, but don't wait if... */
        if (free_scarce_resources_immediately /* we must finish ASAP */ ||
            connection->thread_data.is_writing_stopped /* write will never complete */ ||
            !connection->thread_data.is_outgoing_frames_task_active /* write is already complete */) {

            s_finish_shutdown(connection);
        } else {
            CONNECTION_LOG(TRACE, connection, "HTTP/2 handler will finish shutdown once GOAWAY frame is written");
        }
    }

    return AWS_OP_SUCCESS;
}

static void s_finish_shutdown(struct aws_h2_connection *connection) {
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));
    AWS_PRECONDITION(connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written);

    CONNECTION_LOG(TRACE, connection, "Finishing HTTP/2 handler shutdown");

    connection->thread_data.channel_shutdown_waiting_for_goaway_to_be_written = false;

    s_stop(
        connection,
        false /*stop_reading*/,
        true /*stop_writing*/,
        false /*schedule_shutdown*/,
        connection->thread_data.channel_shutdown_error_code);

    /* Remove remaining streams from internal datastructures and mark them as complete. */

    struct aws_hash_iter stream_iter = aws_hash_iter_begin(&connection->thread_data.active_streams_map);
    while (!aws_hash_iter_done(&stream_iter)) {
        struct aws_h2_stream *stream = stream_iter.element.value;
        aws_hash_iter_delete(&stream_iter, true);
        aws_hash_iter_next(&stream_iter);

        s_stream_complete(connection, stream, AWS_ERROR_HTTP_CONNECTION_CLOSED);
    }

    /* It's OK to access synced_data without holding the lock because
     * no more streams or user-requested control frames can be added after s_stop() has been invoked. */
    while (!aws_linked_list_empty(&connection->synced_data.pending_stream_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->synced_data.pending_stream_list);
        struct aws_h2_stream *stream = AWS_CONTAINER_OF(node, struct aws_h2_stream, node);
        s_stream_complete(connection, stream, AWS_ERROR_HTTP_CONNECTION_CLOSED);
    }

    while (!aws_linked_list_empty(&connection->synced_data.pending_frame_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->synced_data.pending_frame_list);
        struct aws_h2_frame *frame = AWS_CONTAINER_OF(node, struct aws_h2_frame, node);
        aws_h2_frame_destroy(frame);
    }

    /* invoke pending callbacks haven't moved into thread, and clean up the data */
    while (!aws_linked_list_empty(&connection->synced_data.pending_settings_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->synced_data.pending_settings_list);
        struct aws_h2_pending_settings *settings = AWS_CONTAINER_OF(node, struct aws_h2_pending_settings, node);
        if (settings->on_completed) {
            settings->on_completed(&connection->base, AWS_ERROR_HTTP_CONNECTION_CLOSED, settings->user_data);
        }
        aws_mem_release(connection->base.alloc, settings);
    }
    while (!aws_linked_list_empty(&connection->synced_data.pending_ping_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->synced_data.pending_ping_list);
        struct aws_h2_pending_ping *ping = AWS_CONTAINER_OF(node, struct aws_h2_pending_ping, node);
        if (ping->on_completed) {
            ping->on_completed(&connection->base, 0 /*fake rtt*/, AWS_ERROR_HTTP_CONNECTION_CLOSED, ping->user_data);
        }
        aws_mem_release(connection->base.alloc, ping);
    }

    /* invoke pending callbacks moved into thread, and clean up the data */
    while (!aws_linked_list_empty(&connection->thread_data.pending_settings_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->thread_data.pending_settings_queue);
        struct aws_h2_pending_settings *pending_settings = AWS_CONTAINER_OF(node, struct aws_h2_pending_settings, node);
        /* fire the user callback with error */
        if (pending_settings->on_completed) {
            pending_settings->on_completed(
                &connection->base, AWS_ERROR_HTTP_CONNECTION_CLOSED, pending_settings->user_data);
        }
        aws_mem_release(connection->base.alloc, pending_settings);
    }
    while (!aws_linked_list_empty(&connection->thread_data.pending_ping_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&connection->thread_data.pending_ping_queue);
        struct aws_h2_pending_ping *pending_ping = AWS_CONTAINER_OF(node, struct aws_h2_pending_ping, node);
        /* fire the user callback with error */
        if (pending_ping->on_completed) {
            pending_ping->on_completed(
                &connection->base, 0 /*fake rtt*/, AWS_ERROR_HTTP_CONNECTION_CLOSED, pending_ping->user_data);
        }
        aws_mem_release(connection->base.alloc, pending_ping);
    }
    aws_channel_slot_on_handler_shutdown_complete(
        connection->base.channel_slot,
        AWS_CHANNEL_DIR_WRITE,
        connection->thread_data.channel_shutdown_error_code,
        connection->thread_data.channel_shutdown_immediately);
}

static size_t s_handler_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;

    /* HTTP/2 protocol uses WINDOW_UPDATE frames to coordinate data rates with peer,
     * so we can just keep the aws_channel's read-window wide open */
    return SIZE_MAX;
}

static size_t s_handler_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;

    /* "All frames begin with a fixed 9-octet header followed by a variable-length payload" (RFC-7540 4.1) */
    return 9;
}

static void s_reset_statistics(struct aws_channel_handler *handler) {
    struct aws_h2_connection *connection = handler->impl;
    aws_crt_statistics_http2_channel_reset(&connection->thread_data.stats);
    if (aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) == 0) {
        /* Check the current state */
        connection->thread_data.stats.was_inactive = true;
    }
    return;
}

static void s_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats) {

    struct aws_h2_connection *connection = handler->impl;
    AWS_PRECONDITION(aws_channel_thread_is_callers_thread(connection->base.channel_slot->channel));

    /* TODO: Need update the way we calculate statistics, to account for user-controlled pauses.
     * If user is adding chunks 1 by 1, there can naturally be a gap in the upload.
     * If the user lets the stream-window go to zero, there can naturally be a gap in the download. */
    uint64_t now_ns = 0;
    if (aws_channel_current_clock_time(connection->base.channel_slot->channel, &now_ns)) {
        return;
    }

    if (!aws_linked_list_empty(&connection->thread_data.outgoing_streams_list)) {
        s_add_time_measurement_to_stats(
            connection->thread_data.outgoing_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_outgoing_stream_ms);

        connection->thread_data.outgoing_timestamp_ns = now_ns;
    }
    if (aws_hash_table_get_entry_count(&connection->thread_data.active_streams_map) != 0) {
        s_add_time_measurement_to_stats(
            connection->thread_data.incoming_timestamp_ns,
            now_ns,
            &connection->thread_data.stats.pending_incoming_stream_ms);

        connection->thread_data.incoming_timestamp_ns = now_ns;
    } else {
        connection->thread_data.stats.was_inactive = true;
    }

    void *stats_base = &connection->thread_data.stats;
    aws_array_list_push_back(stats, &stats_base);
}
