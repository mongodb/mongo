#ifndef AWS_HTTP_REQUEST_RESPONSE_IMPL_H
#define AWS_HTTP_REQUEST_RESPONSE_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/task_scheduler.h>
#include <aws/http/request_response.h>

#include <aws/http/private/http_impl.h>

#include <aws/common/atomics.h>

struct aws_http_stream_vtable {
    void (*destroy)(struct aws_http_stream *stream);
    void (*update_window)(struct aws_http_stream *stream, size_t increment_size);
    int (*activate)(struct aws_http_stream *stream);
    void (*cancel)(struct aws_http_stream *stream, int error_code);

    int (*http1_write_chunk)(struct aws_http_stream *http1_stream, const struct aws_http1_chunk_options *options);
    int (*http1_add_trailer)(struct aws_http_stream *http1_stream, const struct aws_http_headers *trailing_headers);

    int (*http2_reset_stream)(struct aws_http_stream *http2_stream, uint32_t http2_error);
    int (*http2_get_received_error_code)(struct aws_http_stream *http2_stream, uint32_t *http2_error);
    int (*http2_get_sent_error_code)(struct aws_http_stream *http2_stream, uint32_t *http2_error);
    int (*http2_write_data)(
        struct aws_http_stream *http2_stream,
        const struct aws_http2_stream_write_data_options *options);
};

/**
 * Base class for streams.
 * There are specific implementations for each HTTP version.
 */
struct aws_http_stream {
    const struct aws_http_stream_vtable *vtable;
    struct aws_allocator *alloc;
    struct aws_http_connection *owning_connection;

    uint32_t id;

    void *user_data;
    aws_http_on_incoming_headers_fn *on_incoming_headers;
    aws_http_on_incoming_header_block_done_fn *on_incoming_header_block_done;
    aws_http_on_incoming_body_fn *on_incoming_body;
    aws_http_on_stream_metrics_fn *on_metrics;
    aws_http_on_stream_complete_fn *on_complete;
    aws_http_on_stream_destroy_fn *on_destroy;

    struct aws_atomic_var refcount;
    enum aws_http_method request_method;
    struct aws_http_stream_metrics metrics;

    union {
        struct aws_http_stream_client_data {
            int response_status;
            uint64_t response_first_byte_timeout_ms;
            /* Using aws_task instead of aws_channel_task because, currently, channel-tasks can't be canceled.
             * We only touch this from the connection's thread */
            struct aws_task response_first_byte_timeout_task;
        } client;
        struct aws_http_stream_server_data {
            struct aws_byte_cursor request_method_str;
            struct aws_byte_cursor request_path;
            aws_http_on_incoming_request_done_fn *on_request_done;
        } server;
    } client_or_server_data;

    /* On client connections, `client_data` points to client_or_server_data.client and `server_data` is null.
     * Opposite is true on server connections */
    struct aws_http_stream_client_data *client_data;
    struct aws_http_stream_server_data *server_data;
};

#endif /* AWS_HTTP_REQUEST_RESPONSE_IMPL_H */
