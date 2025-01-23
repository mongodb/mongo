/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cal/hash.h>
#include <aws/common/encoding.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/private/http_impl.h>
#include <aws/http/private/strutil.h>
#include <aws/http/private/websocket_impl.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/uri.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

/**
 * Allow unit-tests to mock interactions with external systems.
 */
static const struct aws_websocket_client_bootstrap_system_vtable s_default_system_vtable = {
    .aws_http_client_connect = aws_http_client_connect,
    .aws_http_connection_release = aws_http_connection_release,
    .aws_http_connection_close = aws_http_connection_close,
    .aws_http_connection_get_channel = aws_http_connection_get_channel,
    .aws_http_connection_make_request = aws_http_connection_make_request,
    .aws_http_stream_activate = aws_http_stream_activate,
    .aws_http_stream_release = aws_http_stream_release,
    .aws_http_stream_get_connection = aws_http_stream_get_connection,
    .aws_http_stream_update_window = aws_http_stream_update_window,
    .aws_http_stream_get_incoming_response_status = aws_http_stream_get_incoming_response_status,
    .aws_websocket_handler_new = aws_websocket_handler_new,
};

static const struct aws_websocket_client_bootstrap_system_vtable *s_system_vtable = &s_default_system_vtable;

void aws_websocket_client_bootstrap_set_system_vtable(
    const struct aws_websocket_client_bootstrap_system_vtable *system_vtable) {

    s_system_vtable = system_vtable;
}

/**
 * The websocket bootstrap brings a websocket connection into this world, and sees it out again.
 * Spins up an HTTP client, performs the opening handshake (HTTP Upgrade request),
 * creates the websocket handler, and inserts it into the channel.
 * The bootstrap is responsible for firing the on_connection_setup and on_connection_shutdown callbacks.
 */
struct aws_websocket_client_bootstrap {
    /* Settings copied in from aws_websocket_client_connection_options */
    struct aws_allocator *alloc;
    size_t initial_window_size;
    bool manual_window_update;
    void *user_data;
    /* Setup callback will be set NULL once it's invoked.
     * This is used to determine whether setup or shutdown should be invoked
     * from the HTTP-shutdown callback. */
    aws_websocket_on_connection_setup_fn *websocket_setup_callback;
    aws_websocket_on_connection_shutdown_fn *websocket_shutdown_callback;
    aws_websocket_on_incoming_frame_begin_fn *websocket_frame_begin_callback;
    aws_websocket_on_incoming_frame_payload_fn *websocket_frame_payload_callback;
    aws_websocket_on_incoming_frame_complete_fn *websocket_frame_complete_callback;

    /* Handshake request data */
    struct aws_http_message *handshake_request;

    /* Given the "Sec-WebSocket-Key" from the request,
     * this is what we expect the response's "Sec-WebSocket-Accept" to be */
    struct aws_byte_buf expected_sec_websocket_accept;

    /* Comma-separated values from the request's "Sec-WebSocket-Protocol" (or NULL if none)  */
    struct aws_string *expected_sec_websocket_protocols;

    /* Handshake response data */
    int response_status;
    struct aws_http_headers *response_headers;
    bool got_full_response_headers;
    struct aws_byte_buf response_body;
    bool got_full_response_body;

    int setup_error_code;
    struct aws_websocket *websocket;
};

static void s_ws_bootstrap_destroy(struct aws_websocket_client_bootstrap *ws_bootstrap);
static int s_ws_bootstrap_calculate_sec_websocket_accept(
    struct aws_byte_cursor sec_websocket_key,
    struct aws_byte_buf *out_buf,
    struct aws_allocator *alloc);
static void s_ws_bootstrap_cancel_setup_due_to_err(
    struct aws_websocket_client_bootstrap *ws_bootstrap,
    struct aws_http_connection *http_connection,
    int error_code);
static void s_ws_bootstrap_on_http_setup(struct aws_http_connection *http_connection, int error_code, void *user_data);
static void s_ws_bootstrap_on_http_shutdown(
    struct aws_http_connection *http_connection,
    int error_code,
    void *user_data);
static int s_ws_bootstrap_on_handshake_response_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data);
static int s_ws_bootstrap_on_handshake_response_header_block_done(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data);
static int s_ws_bootstrap_on_handshake_response_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data);
static void s_ws_bootstrap_on_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data);

int aws_websocket_client_connect(const struct aws_websocket_client_connection_options *options) {
    aws_http_fatal_assert_library_initialized();
    AWS_ASSERT(options);

    /* Validate options */
    struct aws_byte_cursor path;
    aws_http_message_get_request_path(options->handshake_request, &path);
    if (!options->allocator || !options->bootstrap || !options->socket_options || !options->host.len || !path.len ||
        !options->on_connection_setup) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_WEBSOCKET_SETUP, "id=static: Missing required websocket connection options.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_byte_cursor method;
    aws_http_message_get_request_method(options->handshake_request, &method);
    if (aws_http_str_to_method(method) != AWS_HTTP_METHOD_GET) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_WEBSOCKET_SETUP, "id=static: Websocket request must have method be 'GET'.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!options->handshake_request) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Invalid connection options, missing required request for websocket client handshake.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    const struct aws_http_headers *request_headers = aws_http_message_get_headers(options->handshake_request);
    struct aws_byte_cursor sec_websocket_key;
    if (aws_http_headers_get(request_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Key"), &sec_websocket_key)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Websocket handshake request is missing required 'Sec-WebSocket-Key' header");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* Extensions are not currently supported */
    if (aws_http_headers_has(request_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Extensions"))) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP, "id=static: 'Sec-WebSocket-Extensions' are not currently supported");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* Create bootstrap */
    struct aws_websocket_client_bootstrap *ws_bootstrap =
        aws_mem_calloc(options->allocator, 1, sizeof(struct aws_websocket_client_bootstrap));

    ws_bootstrap->alloc = options->allocator;
    ws_bootstrap->initial_window_size = options->initial_window_size;
    ws_bootstrap->manual_window_update = options->manual_window_management;
    ws_bootstrap->user_data = options->user_data;
    ws_bootstrap->websocket_setup_callback = options->on_connection_setup;
    ws_bootstrap->websocket_shutdown_callback = options->on_connection_shutdown;
    ws_bootstrap->websocket_frame_begin_callback = options->on_incoming_frame_begin;
    ws_bootstrap->websocket_frame_payload_callback = options->on_incoming_frame_payload;
    ws_bootstrap->websocket_frame_complete_callback = options->on_incoming_frame_complete;
    ws_bootstrap->handshake_request = aws_http_message_acquire(options->handshake_request);
    ws_bootstrap->response_status = AWS_HTTP_STATUS_CODE_UNKNOWN;
    ws_bootstrap->response_headers = aws_http_headers_new(ws_bootstrap->alloc);
    aws_byte_buf_init(&ws_bootstrap->response_body, ws_bootstrap->alloc, 0);

    if (s_ws_bootstrap_calculate_sec_websocket_accept(
            sec_websocket_key, &ws_bootstrap->expected_sec_websocket_accept, ws_bootstrap->alloc)) {
        goto error;
    }

    ws_bootstrap->expected_sec_websocket_protocols =
        aws_http_headers_get_all(request_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Protocol"));

    /* Initiate HTTP connection */
    struct aws_http_client_connection_options http_options = AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT;
    http_options.allocator = ws_bootstrap->alloc;
    http_options.bootstrap = options->bootstrap;
    http_options.host_name = options->host;
    http_options.socket_options = options->socket_options;
    http_options.tls_options = options->tls_options;
    http_options.proxy_options = options->proxy_options;

    if (options->manual_window_management) {
        http_options.manual_window_management = true;

        /* Give HTTP handler enough window to comfortably receive the handshake response.
         *
         * If the upgrade is unsuccessful, the HTTP window will shrink as the response body is received.
         * In this case, we'll keep incrementing the window back to its original size so data keeps arriving.
         *
         * If the upgrade is successful, then the websocket handler is installed, and
         * the HTTP handler will take over its own window management. */
        http_options.initial_window_size = 1024;
    }

    http_options.user_data = ws_bootstrap;
    http_options.on_setup = s_ws_bootstrap_on_http_setup;
    http_options.on_shutdown = s_ws_bootstrap_on_http_shutdown;
    http_options.requested_event_loop = options->requested_event_loop;
    http_options.host_resolution_config = options->host_resolution_config;

    /* Infer port, if not explicitly specified in URI */
    http_options.port = options->port;
    if (!http_options.port) {
        http_options.port = options->tls_options ? 443 : 80;
    }

    if (s_system_vtable->aws_http_client_connect(&http_options)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Websocket failed to initiate HTTP connection, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    /* Success! (so far) */
    AWS_LOGF_TRACE(
        AWS_LS_HTTP_WEBSOCKET_SETUP,
        "id=%p: Websocket setup begun, connecting to " PRInSTR ":%" PRIu32 PRInSTR,
        (void *)ws_bootstrap,
        AWS_BYTE_CURSOR_PRI(options->host),
        options->port,
        AWS_BYTE_CURSOR_PRI(path));

    return AWS_OP_SUCCESS;

error:
    s_ws_bootstrap_destroy(ws_bootstrap);
    return AWS_OP_ERR;
}

static void s_ws_bootstrap_destroy(struct aws_websocket_client_bootstrap *ws_bootstrap) {
    if (!ws_bootstrap) {
        return;
    }

    aws_http_message_release(ws_bootstrap->handshake_request);
    aws_http_headers_release(ws_bootstrap->response_headers);
    aws_byte_buf_clean_up(&ws_bootstrap->expected_sec_websocket_accept);
    aws_string_destroy(ws_bootstrap->expected_sec_websocket_protocols);
    aws_byte_buf_clean_up(&ws_bootstrap->response_body);

    aws_mem_release(ws_bootstrap->alloc, ws_bootstrap);
}

/* Given the handshake request's "Sec-WebSocket-Key" value,
 * calculate the expected value for the response's "Sec-WebSocket-Accept".
 * RFC-6455 Section 4.1:
 *      base64-encoded SHA-1 of the concatenation of the |Sec-WebSocket-Key|
 *      (as a string, not base64-decoded) with the string
 *      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" but ignoring any leading and
 *      trailing whitespace
 */
static int s_ws_bootstrap_calculate_sec_websocket_accept(
    struct aws_byte_cursor sec_websocket_key,
    struct aws_byte_buf *out_buf,
    struct aws_allocator *alloc) {

    AWS_ASSERT(out_buf && !out_buf->allocator && out_buf->len == 0); /* expect buf to be uninitialized */

    /* note: leading and trailing whitespace was already trimmed by aws_http_headers */

    /* optimization: skip concatenating Sec-WebSocket-Key and the magic string.
     * just run the SHA1 over the first string, and then the 2nd. */

    bool success = false;
    struct aws_byte_cursor magic_string = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

    /* SHA-1 */
    struct aws_hash *sha1 = aws_sha1_new(alloc);
    if (!sha1) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Failed to initiate SHA1, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto cleanup;
    }

    if (aws_hash_update(sha1, &sec_websocket_key) || aws_hash_update(sha1, &magic_string)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Failed to update SHA1, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto cleanup;
    }

    uint8_t sha1_storage[AWS_SHA1_LEN];
    struct aws_byte_buf sha1_buf = aws_byte_buf_from_empty_array(sha1_storage, sizeof(sha1_storage));
    if (aws_hash_finalize(sha1, &sha1_buf, 0)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Failed to finalize SHA1, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto cleanup;
    }

    /* base64-encoded SHA-1 (clear out_buf, and write to it again) */
    size_t base64_encode_sha1_len;
    if (aws_base64_compute_encoded_len(sha1_buf.len, &base64_encode_sha1_len)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Failed to determine Base64-encoded length, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto cleanup;
    }
    aws_byte_buf_init(out_buf, alloc, base64_encode_sha1_len);

    struct aws_byte_cursor sha1_cursor = aws_byte_cursor_from_buf(&sha1_buf);
    if (aws_base64_encode(&sha1_cursor, out_buf)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=static: Failed to Base64-encode, error %d (%s)",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto cleanup;
    }

    success = true;
cleanup:
    if (sha1) {
        aws_hash_destroy(sha1);
    }
    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

/* Called if something goes wrong after an HTTP connection is established.
 * The HTTP connection is closed.
 * We must wait for its shutdown to complete before informing user of the failed websocket setup. */
static void s_ws_bootstrap_cancel_setup_due_to_err(
    struct aws_websocket_client_bootstrap *ws_bootstrap,
    struct aws_http_connection *http_connection,
    int error_code) {

    AWS_ASSERT(error_code);
    AWS_ASSERT(http_connection);

    if (!ws_bootstrap->setup_error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Canceling websocket setup due to error %d (%s).",
            (void *)ws_bootstrap,
            error_code,
            aws_error_name(error_code));

        ws_bootstrap->setup_error_code = error_code;

        s_system_vtable->aws_http_connection_close(http_connection);
    }
}

static void s_ws_bootstrap_invoke_setup_callback(struct aws_websocket_client_bootstrap *ws_bootstrap, int error_code) {

    /* sanity check: websocket XOR error_code is set. both cannot be set. both cannot be unset */
    AWS_FATAL_ASSERT((error_code != 0) ^ (ws_bootstrap->websocket != NULL));

    /* Report things about the response, if we received them */
    int *response_status_ptr = NULL;
    struct aws_http_header *response_header_array = NULL;
    size_t num_response_headers = 0;
    struct aws_byte_cursor *response_body_ptr = NULL;
    struct aws_byte_cursor response_body_cursor = {.len = 0};

    if (ws_bootstrap->got_full_response_headers) {
        response_status_ptr = &ws_bootstrap->response_status;

        num_response_headers = aws_http_headers_count(ws_bootstrap->response_headers);

        response_header_array =
            aws_mem_calloc(ws_bootstrap->alloc, aws_max_size(1, num_response_headers), sizeof(struct aws_http_header));

        for (size_t i = 0; i < num_response_headers; ++i) {
            aws_http_headers_get_index(ws_bootstrap->response_headers, i, &response_header_array[i]);
        }

        if (ws_bootstrap->got_full_response_body) {
            response_body_cursor = aws_byte_cursor_from_buf(&ws_bootstrap->response_body);
            response_body_ptr = &response_body_cursor;
        }
    }

    struct aws_websocket_on_connection_setup_data setup_data = {
        .error_code = error_code,
        .websocket = ws_bootstrap->websocket,
        .handshake_response_status = response_status_ptr,
        .handshake_response_header_array = response_header_array,
        .num_handshake_response_headers = num_response_headers,
        .handshake_response_body = response_body_ptr,
    };

    ws_bootstrap->websocket_setup_callback(&setup_data, ws_bootstrap->user_data);

    /* Clear setup callback so that we know that it's been invoked. */
    ws_bootstrap->websocket_setup_callback = NULL;

    if (response_header_array) {
        aws_mem_release(ws_bootstrap->alloc, response_header_array);
    }
}

/* Invoked when HTTP connection has been established (or failed to be established) */
static void s_ws_bootstrap_on_http_setup(struct aws_http_connection *http_connection, int error_code, void *user_data) {

    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;

    /* Setup callback contract is: if error_code is non-zero then connection is NULL. */
    AWS_FATAL_ASSERT((error_code != 0) == (http_connection == NULL));

    /* If http connection failed, inform the user immediately and clean up the websocket bootstrapper. */
    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Websocket setup failed to establish HTTP connection, error %d (%s).",
            (void *)ws_bootstrap,
            error_code,
            aws_error_name(error_code));

        s_ws_bootstrap_invoke_setup_callback(ws_bootstrap, error_code);

        s_ws_bootstrap_destroy(ws_bootstrap);
        return;
    }

    /* Connection exists!
     * Note that if anything goes wrong with websocket setup from hereon out, we must close the http connection
     * first and wait for shutdown to complete before informing the user of setup failure. */

    /* Send the handshake request */
    struct aws_http_make_request_options options = {
        .self_size = sizeof(options),
        .request = ws_bootstrap->handshake_request,
        .user_data = ws_bootstrap,
        .on_response_headers = s_ws_bootstrap_on_handshake_response_headers,
        .on_response_header_block_done = s_ws_bootstrap_on_handshake_response_header_block_done,
        .on_response_body = s_ws_bootstrap_on_handshake_response_body,
        .on_complete = s_ws_bootstrap_on_stream_complete,
    };

    struct aws_http_stream *handshake_stream =
        s_system_vtable->aws_http_connection_make_request(http_connection, &options);

    if (!handshake_stream) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Failed to make websocket upgrade request, error %d (%s).",
            (void *)ws_bootstrap,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    if (s_system_vtable->aws_http_stream_activate(handshake_stream)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Failed to activate websocket upgrade request, error %d (%s).",
            (void *)ws_bootstrap,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    /* Success! (so far) */
    AWS_LOGF_TRACE(
        AWS_LS_HTTP_WEBSOCKET_SETUP,
        "id=%p: HTTP connection established, sending websocket upgrade request.",
        (void *)ws_bootstrap);
    return;

error:
    s_system_vtable->aws_http_stream_release(handshake_stream);
    s_ws_bootstrap_cancel_setup_due_to_err(ws_bootstrap, http_connection, aws_last_error());
}

/* Invoked when the HTTP connection has shut down.
 * This is never called if the HTTP connection failed its setup */
static void s_ws_bootstrap_on_http_shutdown(
    struct aws_http_connection *http_connection,
    int error_code,
    void *user_data) {

    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;

    /* Inform user that connection has completely shut down.
     * If setup callback still hasn't fired, invoke it now and indicate failure.
     * Otherwise, invoke shutdown callback. */
    if (ws_bootstrap->websocket_setup_callback) {
        AWS_ASSERT(!ws_bootstrap->websocket);

        /* If there's already a setup_error_code, use that */
        if (ws_bootstrap->setup_error_code) {
            error_code = ws_bootstrap->setup_error_code;
        }

        /* Ensure non-zero error_code is passed */
        if (!error_code) {
            error_code = AWS_ERROR_UNKNOWN;
        }

        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Websocket setup failed, error %d (%s).",
            (void *)ws_bootstrap,
            error_code,
            aws_error_name(error_code));

        s_ws_bootstrap_invoke_setup_callback(ws_bootstrap, error_code);

    } else if (ws_bootstrap->websocket_shutdown_callback) {
        AWS_ASSERT(ws_bootstrap->websocket);

        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_WEBSOCKET,
            "id=%p: Websocket client connection shut down with error %d (%s).",
            (void *)ws_bootstrap->websocket,
            error_code,
            aws_error_name(error_code));

        ws_bootstrap->websocket_shutdown_callback(ws_bootstrap->websocket, error_code, ws_bootstrap->user_data);
    }

    /* Clean up HTTP connection and websocket-bootstrap.
     * It's still up to the user to release the websocket itself. */
    s_system_vtable->aws_http_connection_release(http_connection);

    s_ws_bootstrap_destroy(ws_bootstrap);
}

/* Invoked repeatedly as handshake response headers arrive */
static int s_ws_bootstrap_on_handshake_response_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data) {

    (void)stream;
    (void)header_block;

    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;

    /* Deep-copy headers into ws_bootstrap */
    aws_http_headers_add_array(ws_bootstrap->response_headers, header_array, num_headers);

    /* Don't report a partially-received response */
    ws_bootstrap->got_full_response_headers = false;

    return AWS_OP_SUCCESS;
}

static int s_ws_bootstrap_validate_header(
    struct aws_websocket_client_bootstrap *ws_bootstrap,
    const char *name,
    struct aws_byte_cursor expected_value,
    bool case_sensitive) {

    struct aws_byte_cursor actual_value;
    if (aws_http_headers_get(ws_bootstrap->response_headers, aws_byte_cursor_from_c_str(name), &actual_value)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP, "id=%p: Response lacks required '%s' header", (void *)ws_bootstrap, name);
        return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
    }

    bool matches = case_sensitive ? aws_byte_cursor_eq(&expected_value, &actual_value)
                                  : aws_byte_cursor_eq_ignore_case(&expected_value, &actual_value);
    if (!matches) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Response '%s' header has wrong value. Expected '" PRInSTR "'. Received '" PRInSTR "'",
            (void *)ws_bootstrap,
            name,
            AWS_BYTE_CURSOR_PRI(expected_value),
            AWS_BYTE_CURSOR_PRI(actual_value));
        return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
    }

    return AWS_OP_SUCCESS;
}

static int s_ws_bootstrap_validate_sec_websocket_protocol(const struct aws_websocket_client_bootstrap *ws_bootstrap) {
    /* First handle the easy case:
     * If client requested no protocols, then the response should not pick any */
    if (ws_bootstrap->expected_sec_websocket_protocols == NULL) {
        if (aws_http_headers_has(
                ws_bootstrap->response_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Protocol"))) {

            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET_SETUP,
                "id=%p: Response has 'Sec-WebSocket-Protocol' header, no protocol was requested",
                (void *)ws_bootstrap);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
        } else {
            return AWS_OP_SUCCESS;
        }
    }

    /* Check that server has picked one of the protocols listed in the request */
    struct aws_byte_cursor response_protocol;
    if (aws_http_headers_get(
            ws_bootstrap->response_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Protocol"), &response_protocol)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Response lacks required 'Sec-WebSocket-Protocol' header",
            (void *)ws_bootstrap);
        return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
    }

    struct aws_byte_cursor request_protocols =
        aws_byte_cursor_from_string(ws_bootstrap->expected_sec_websocket_protocols);
    struct aws_byte_cursor request_protocol_i;
    AWS_ZERO_STRUCT(request_protocol_i);
    while (aws_byte_cursor_next_split(&request_protocols, ',', &request_protocol_i)) {
        struct aws_byte_cursor request_protocol = aws_strutil_trim_http_whitespace(request_protocol_i);
        if (aws_byte_cursor_eq(&response_protocol, &request_protocol)) {
            /* Success! */
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_WEBSOCKET_SETUP,
                "id=%p: Server selected Sec-WebSocket-Protocol: " PRInSTR,
                (void *)ws_bootstrap,
                AWS_BYTE_CURSOR_PRI(response_protocol));
            return AWS_OP_SUCCESS;
        }
    }

    AWS_LOGF_ERROR(
        AWS_LS_HTTP_WEBSOCKET_SETUP,
        "id=%p: Response 'Sec-WebSocket-Protocol' header has wrong value. Received '" PRInSTR
        "'. Expected one of '" PRInSTR "'",
        (void *)ws_bootstrap,
        AWS_BYTE_CURSOR_PRI(response_protocol),
        AWS_BYTE_CURSOR_PRI(request_protocols));
    return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
}

/* OK, we've got all the headers for the 101 Switching Protocols response.
 * Validate the handshake response, install the websocket handler into the channel,
 * and invoke the on_connection_setup callback. */
static int s_ws_bootstrap_validate_response_and_install_websocket_handler(
    struct aws_websocket_client_bootstrap *ws_bootstrap,
    struct aws_http_connection *http_connection) {

    /* RFC-6455 Section 4.1 - The client MUST validate the server's response as follows... */

    /* (we already checked step 1, that status code is 101) */
    AWS_FATAL_ASSERT(ws_bootstrap->response_status == AWS_HTTP_STATUS_CODE_101_SWITCHING_PROTOCOLS);

    /* 2.   If the response lacks an |Upgrade| header field or the |Upgrade|
     *      header field contains a value that is not an ASCII case-
     *      insensitive match for the value "websocket", the client MUST
     *      _Fail the WebSocket Connection_. */
    if (s_ws_bootstrap_validate_header(
            ws_bootstrap, "Upgrade", aws_byte_cursor_from_c_str("websocket"), false /*case_sensitive*/)) {
        goto error;
    }

    /* 3.   If the response lacks a |Connection| header field or the
     *      |Connection| header field doesn't contain a token that is an
     *      ASCII case-insensitive match for the value "Upgrade", the client
     *      MUST _Fail the WebSocket Connection_. */
    if (s_ws_bootstrap_validate_header(
            ws_bootstrap, "Connection", aws_byte_cursor_from_c_str("Upgrade"), false /*case_sensitive*/)) {
        goto error;
    }

    /* 4.   If the response lacks a |Sec-WebSocket-Accept| header field or
     *      the |Sec-WebSocket-Accept| contains a value other than the
     *      base64-encoded SHA-1 of the concatenation of the |Sec-WebSocket-
     *      Key| (as a string, not base64-decoded) with the string "258EAFA5-
     *      E914-47DA-95CA-C5AB0DC85B11" but ignoring any leading and
     *      trailing whitespace, the client MUST _Fail the WebSocket
     *      Connection_. */
    if (s_ws_bootstrap_validate_header(
            ws_bootstrap,
            "Sec-WebSocket-Accept",
            aws_byte_cursor_from_buf(&ws_bootstrap->expected_sec_websocket_accept),
            true /*case_sensitive*/)) {
        goto error;
    }

    /* (step 5 is about validating Sec-WebSocket-Extensions, but we don't support extensions) */
    if (aws_http_headers_has(ws_bootstrap->response_headers, aws_byte_cursor_from_c_str("Sec-WebSocket-Extensions"))) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Response has 'Sec-WebSocket-Extensions' header, but client does not support extensions.",
            (void *)ws_bootstrap);
        aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE);
        goto error;
    }

    /* 6.   If the response includes a |Sec-WebSocket-Protocol| header field
     *      and this header field indicates the use of a subprotocol that was
     *      not present in the client's handshake (the server has indicated a
     *      subprotocol not requested by the client), the client MUST _Fail
     *      the WebSocket Connection_. */
    if (s_ws_bootstrap_validate_sec_websocket_protocol(ws_bootstrap)) {
        goto error;
    }

    /* Insert websocket handler into channel */
    struct aws_channel *channel = s_system_vtable->aws_http_connection_get_channel(http_connection);
    AWS_ASSERT(channel);

    struct aws_websocket_handler_options ws_options = {
        .allocator = ws_bootstrap->alloc,
        .channel = channel,
        .initial_window_size = ws_bootstrap->initial_window_size,
        .user_data = ws_bootstrap->user_data,
        .on_incoming_frame_begin = ws_bootstrap->websocket_frame_begin_callback,
        .on_incoming_frame_payload = ws_bootstrap->websocket_frame_payload_callback,
        .on_incoming_frame_complete = ws_bootstrap->websocket_frame_complete_callback,
        .is_server = false,
        .manual_window_update = ws_bootstrap->manual_window_update,
    };

    ws_bootstrap->websocket = s_system_vtable->aws_websocket_handler_new(&ws_options);
    if (!ws_bootstrap->websocket) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET_SETUP,
            "id=%p: Failed to create websocket handler, error %d (%s)",
            (void *)ws_bootstrap,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Success! Setup complete! */
    AWS_LOGF_TRACE(/* Log for tracing setup id to websocket id.  */
                   AWS_LS_HTTP_WEBSOCKET_SETUP,
                   "id=%p: Setup success, created websocket=%p",
                   (void *)ws_bootstrap,
                   (void *)ws_bootstrap->websocket);

    AWS_LOGF_DEBUG(/* Debug log about creation of websocket. */
                   AWS_LS_HTTP_WEBSOCKET,
                   "id=%p: Websocket client connection established.",
                   (void *)ws_bootstrap->websocket);

    s_ws_bootstrap_invoke_setup_callback(ws_bootstrap, 0 /*error_code*/);
    return AWS_OP_SUCCESS;

error:
    s_ws_bootstrap_cancel_setup_due_to_err(ws_bootstrap, http_connection, aws_last_error());
    /* Returning error stops HTTP from processing any further data */
    return AWS_OP_ERR;
}

/**
 * Invoked each time we reach the end of a block of response headers.
 * If we got a valid 101 Switching Protocols response, we insert the websocket handler.
 * Note:
 *      In HTTP, 1xx responses are "interim" responses. So a 101 Switching Protocols
 *      response does not "complete" the stream. Once the connection has switched
 *      protocols, the stream does not end until the whole connection is closed.
 */
static int s_ws_bootstrap_on_handshake_response_header_block_done(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data) {

    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;
    struct aws_http_connection *http_connection = s_system_vtable->aws_http_stream_get_connection(stream);
    AWS_ASSERT(http_connection);

    /* Get status code from stream */
    s_system_vtable->aws_http_stream_get_incoming_response_status(stream, &ws_bootstrap->response_status);

    ws_bootstrap->got_full_response_headers = true;

    if (header_block == AWS_HTTP_HEADER_BLOCK_INFORMATIONAL) {
        if (ws_bootstrap->response_status == AWS_HTTP_STATUS_CODE_101_SWITCHING_PROTOCOLS) {
            /* OK, got 101 response, proceed with upgrade! */
            return s_ws_bootstrap_validate_response_and_install_websocket_handler(ws_bootstrap, http_connection);

        } else {
            /* It would be weird to get any other kind of 1xx response, but anything is possible.
             * Another response should come eventually. Just ignore the headers from this one... */
            AWS_LOGF_DEBUG(
                AWS_LS_HTTP_WEBSOCKET_SETUP,
                "id=%p: Server sent interim response with status code %d",
                (void *)ws_bootstrap,
                ws_bootstrap->response_status);

            aws_http_headers_clear(ws_bootstrap->response_headers);
            ws_bootstrap->got_full_response_headers = false;
            return AWS_OP_SUCCESS;
        }
    }

    /* Otherwise, we got normal headers (from a non-1xx response), or trailing headers.
     * This can only happen if the handshake did not succeed. Keep the connection going.
     * We'll report failed setup to the user after we've received the complete response */
    ws_bootstrap->setup_error_code = AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE;
    return AWS_OP_SUCCESS;
}

/**
 * Invoked as we receive the body of a failed response.
 * This is never invoked if the handshake succeeds.
 */
static int s_ws_bootstrap_on_handshake_response_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;

    aws_byte_buf_append_dynamic(&ws_bootstrap->response_body, data);

    /* If we're managing the read window...
     * bump the HTTP window back to its starting size, so that we keep receiving the whole response. */
    if (ws_bootstrap->manual_window_update) {
        s_system_vtable->aws_http_stream_update_window(stream, data->len);
    }

    return AWS_OP_SUCCESS;
}

/**
 * Invoked when the stream completes.
 *
 * If the handshake succeeded and the websocket was installed,
 * then this is invoked at the end of the websocket connection.
 *
 * If the handshake response was not 101, then this is invoked
 * after we've received the whole response.
 *
 * Or this is invoked because the connection failed unexpectedly before the handshake could complete,
 * (or we killed the connection because the 101 response didn't pass validation).
 */
static void s_ws_bootstrap_on_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct aws_websocket_client_bootstrap *ws_bootstrap = user_data;
    struct aws_http_connection *http_connection = s_system_vtable->aws_http_stream_get_connection(stream);

    /* Only report the body if we received a complete response */
    if (error_code == 0) {
        ws_bootstrap->got_full_response_body = true;
    }

    /* Make sure the connection closes.
     * We'll deal with finishing setup or shutdown from the http-shutdown callback */
    s_system_vtable->aws_http_connection_close(http_connection);

    /* Done with stream, let it be cleaned up */
    s_system_vtable->aws_http_stream_release(stream);
}
