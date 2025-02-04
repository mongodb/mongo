#ifndef AWS_HTTP_REQUEST_RESPONSE_H
#define AWS_HTTP_REQUEST_RESPONSE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

#include <aws/io/future.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http_connection;
struct aws_input_stream;

/**
 * A stream exists for the duration of a request/response exchange.
 * A client creates a stream to send a request and receive a response.
 * A server creates a stream to receive a request and send a response.
 * In http/2, a push-promise stream can be sent by a server and received by a client.
 */
struct aws_http_stream;

/**
 * Controls whether a header's strings may be compressed by encoding the index of
 * strings in a cache, rather than encoding the literal string.
 *
 * This setting has no effect on HTTP/1.x connections.
 * On HTTP/2 connections this controls HPACK behavior.
 * See RFC-7541 Section 7.1 for security considerations.
 */
enum aws_http_header_compression {
    /**
     * Compress header by encoding the cached index of its strings,
     * or by updating the cache to contain these strings for future reference.
     * Best for headers that are sent repeatedly.
     * This is the default setting.
     */
    AWS_HTTP_HEADER_COMPRESSION_USE_CACHE,

    /**
     * Encode header strings literally.
     * If an intermediary re-broadcasts the headers, it is permitted to use cache.
     * Best for unique headers that are unlikely to repeat.
     */
    AWS_HTTP_HEADER_COMPRESSION_NO_CACHE,

    /**
     * Encode header strings literally and forbid all intermediaries from using
     * cache when re-broadcasting.
     * Best for header fields that are highly valuable or sensitive to recovery.
     */
    AWS_HTTP_HEADER_COMPRESSION_NO_FORWARD_CACHE,
};

/**
 * A lightweight HTTP header struct.
 * Note that the underlying strings are not owned by the byte cursors.
 */
struct aws_http_header {
    struct aws_byte_cursor name;
    struct aws_byte_cursor value;

    /* Controls whether the header's strings may be compressed via caching. */
    enum aws_http_header_compression compression;
};

/**
 * A transformable block of HTTP headers.
 * Provides a nice API for getting/setting header names and values.
 *
 * All strings are copied and stored within this datastructure.
 * The index of a given header may change any time headers are modified.
 * When iterating headers, the following ordering rules apply:
 *
 * - Headers with the same name will always be in the same order, relative to one another.
 *   If "A: one" is added before "A: two", then "A: one" will always precede "A: two".
 *
 * - Headers with different names could be in any order, relative to one another.
 *   If "A: one" is seen before "B: bee" in one iteration, you might see "B: bee" before "A: one" on the next.
 */
struct aws_http_headers;

/**
 * Header block type.
 * INFORMATIONAL: Header block for 1xx informational (interim) responses.
 * MAIN: Main header block sent with request or response.
 * TRAILING: Headers sent after the body of a request or response.
 */
enum aws_http_header_block {
    AWS_HTTP_HEADER_BLOCK_MAIN,
    AWS_HTTP_HEADER_BLOCK_INFORMATIONAL,
    AWS_HTTP_HEADER_BLOCK_TRAILING,
};

/**
 * The definition for an outgoing HTTP request or response.
 * The message may be transformed (ex: signing the request) before its data is eventually sent.
 *
 * The message keeps internal copies of its trivial strings (method, path, headers)
 * but does NOT take ownership of its body stream.
 *
 * A language binding would likely present this as an HttpMessage base class with
 * HttpRequest and HttpResponse subclasses.
 */
struct aws_http_message;

/**
 * Function to invoke when a message transformation completes.
 * This function MUST be invoked or the application will soft-lock.
 * `message` and `complete_ctx` must be the same pointers provided to the `aws_http_message_transform_fn`.
 * `error_code` should should be AWS_ERROR_SUCCESS if transformation was successful,
 * otherwise pass a different AWS_ERROR_X value.
 */
typedef void(
    aws_http_message_transform_complete_fn)(struct aws_http_message *message, int error_code, void *complete_ctx);

/**
 * A function that may modify a request or response before it is sent.
 * The transformation may be asynchronous or immediate.
 * The user MUST invoke the `complete_fn` when transformation is complete or the application will soft-lock.
 * When invoking the `complete_fn`, pass along the `message` and `complete_ctx` provided here and an error code.
 * The error code should be AWS_ERROR_SUCCESS if transformation was successful,
 * otherwise pass a different AWS_ERROR_X value.
 */
typedef void(aws_http_message_transform_fn)(
    struct aws_http_message *message,
    void *user_data,
    aws_http_message_transform_complete_fn *complete_fn,
    void *complete_ctx);

/**
 * Invoked repeatedly times as headers are received.
 * At this point, aws_http_stream_get_incoming_response_status() can be called for the client.
 * And aws_http_stream_get_incoming_request_method() and aws_http_stream_get_incoming_request_uri() can be called for
 * the server.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * Return AWS_OP_SUCCESS to continue processing the stream.
 * Return aws_raise_error(E) to indicate failure and cancel the stream.
 * The error you raise will be reflected in the error_code passed to the on_complete callback.
 */
typedef int(aws_http_on_incoming_headers_fn)(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data);

/**
 * Invoked when the incoming header block of this type(informational/main/trailing) has been completely read.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * Return AWS_OP_SUCCESS to continue processing the stream.
 * Return aws_raise_error(E) to indicate failure and cancel the stream.
 * The error you raise will be reflected in the error_code passed to the on_complete callback.
 */
typedef int(aws_http_on_incoming_header_block_done_fn)(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data);

/**
 * Called repeatedly as body data is received.
 * The data must be copied immediately if you wish to preserve it.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * Note that, if the connection is using manual_window_management then the window
 * size has shrunk by the amount of body data received. If the window size
 * reaches 0 no further data will be received. Increment the window size with
 * aws_http_stream_update_window().
 *
 * Return AWS_OP_SUCCESS to continue processing the stream.
 * Return aws_raise_error(E) to indicate failure and cancel the stream.
 * The error you raise will be reflected in the error_code passed to the on_complete callback.
 */
typedef int(
    aws_http_on_incoming_body_fn)(struct aws_http_stream *stream, const struct aws_byte_cursor *data, void *user_data);

/**
 * Invoked when request has been completely read.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * Return AWS_OP_SUCCESS to continue processing the stream.
 * Return aws_raise_error(E) to indicate failure and cancel the stream.
 * The error you raise will be reflected in the error_code passed to the on_complete callback.
 */
typedef int(aws_http_on_incoming_request_done_fn)(struct aws_http_stream *stream, void *user_data);

/**
 * Invoked when a request/response stream is complete, whether successful or unsuccessful
 * This is always invoked on the HTTP connection's event-loop thread.
 * This will not be invoked if the stream is never activated.
 */
typedef void(aws_http_on_stream_complete_fn)(struct aws_http_stream *stream, int error_code, void *user_data);

/**
 * Invoked when request/response stream destroy completely.
 * This can be invoked within the same thead who release the refcount on http stream.
 * This is invoked even if the stream is never activated.
 */
typedef void(aws_http_on_stream_destroy_fn)(void *user_data);

/**
 * Tracing metrics for aws_http_stream.
 * Data maybe not be available if the data of stream was never sent/received before it completes.
 */
struct aws_http_stream_metrics {
    /* The time stamp when the request started to be encoded. -1 means data not available. Timestamp
     * are from `aws_high_res_clock_get_ticks` */
    int64_t send_start_timestamp_ns;
    /* The time stamp when the request finished to be encoded. -1 means data not available.
     * Timestamp are from `aws_high_res_clock_get_ticks` */
    int64_t send_end_timestamp_ns;
    /* The time duration for the request from start encoding to finish encoding (send_end_timestamp_ns -
     * send_start_timestamp_ns). -1 means data not available. */
    int64_t sending_duration_ns;

    /* The time stamp when the response started to be received from the network channel. -1 means data not available.
     * Timestamp are from `aws_high_res_clock_get_ticks` */
    int64_t receive_start_timestamp_ns;
    /* The time stamp when the response finished to be received from the network channel. -1 means data not available.
     * Timestamp are from `aws_high_res_clock_get_ticks` */
    int64_t receive_end_timestamp_ns;
    /* The time duration for the request from start receiving to finish receiving. receive_end_timestamp_ns -
     * receive_start_timestamp_ns. -1 means data not available. */
    int64_t receiving_duration_ns;

    /* The stream-id on the connection when this stream was activated. */
    uint32_t stream_id;
};

/**
 * Invoked right before request/response stream is complete to report the tracing metrics for aws_http_stream.
 * This may be invoked synchronously when aws_http_stream_release() is called.
 * This is invoked even if the stream is never activated.
 * See `aws_http_stream_metrics` for details.
 */
typedef void(aws_http_on_stream_metrics_fn)(
    struct aws_http_stream *stream,
    const struct aws_http_stream_metrics *metrics,
    void *user_data);

/**
 * Options for creating a stream which sends a request from the client and receives a response from the server.
 */
struct aws_http_make_request_options {
    /**
     * The sizeof() this struct, used for versioning.
     * Required.
     */
    size_t self_size;

    /**
     * Definition for outgoing request.
     * Required.
     * The request will be kept alive via refcounting until the request completes.
     */
    struct aws_http_message *request;

    void *user_data;

    /**
     * Invoked repeatedly times as headers are received.
     * Optional.
     * See `aws_http_on_incoming_headers_fn`.
     */
    aws_http_on_incoming_headers_fn *on_response_headers;

    /**
     * Invoked when response header block has been completely read.
     * Optional.
     * See `aws_http_on_incoming_header_block_done_fn`.
     */
    aws_http_on_incoming_header_block_done_fn *on_response_header_block_done;

    /**
     * Invoked repeatedly as body data is received.
     * Optional.
     * See `aws_http_on_incoming_body_fn`.
     */
    aws_http_on_incoming_body_fn *on_response_body;

    /**
     * Invoked right before stream is complete, whether successful or unsuccessful
     * Optional.
     * See `aws_http_on_stream_metrics_fn`
     */
    aws_http_on_stream_metrics_fn *on_metrics;

    /**
     * Invoked when request/response stream is complete, whether successful or unsuccessful
     * Optional.
     * See `aws_http_on_stream_complete_fn`.
     */
    aws_http_on_stream_complete_fn *on_complete;

    /* Callback for when the request/response stream is completely destroyed. */
    aws_http_on_stream_destroy_fn *on_destroy;

    /**
     * When using HTTP/2, request body data will be provided over time. The stream will only be polled for writing
     * when data has been supplied via `aws_http2_stream_write_data`
     */
    bool http2_use_manual_data_writes;

    /**
     * Optional (ignored if 0).
     * After a request is fully sent, if the server does not begin responding within N milliseconds, then fail with
     * AWS_ERROR_HTTP_RESPONSE_FIRST_BYTE_TIMEOUT.
     * It override the connection level settings, when the request completes, the
     * original monitoring options will be applied back to the connection.
     * TODO: Only supported in HTTP/1.1 now, support it in HTTP/2
     */
    uint64_t response_first_byte_timeout_ms;
};

struct aws_http_request_handler_options {
    /* Set to sizeof() this struct, used for versioning. */
    size_t self_size;

    /**
     * Required.
     */
    struct aws_http_connection *server_connection;

    /**
     * user_data passed to callbacks.
     * Optional.
     */
    void *user_data;

    /**
     * Invoked repeatedly times as headers are received.
     * Optional.
     * See `aws_http_on_incoming_headers_fn`.
     */
    aws_http_on_incoming_headers_fn *on_request_headers;

    /**
     * Invoked when the request header block has been completely read.
     * Optional.
     * See `aws_http_on_incoming_header_block_done_fn`.
     */
    aws_http_on_incoming_header_block_done_fn *on_request_header_block_done;

    /**
     * Invoked as body data is received.
     * Optional.
     * See `aws_http_on_incoming_body_fn`.
     */
    aws_http_on_incoming_body_fn *on_request_body;

    /**
     * Invoked when request has been completely read.
     * Optional.
     * See `aws_http_on_incoming_request_done_fn`.
     */
    aws_http_on_incoming_request_done_fn *on_request_done;

    /**
     * Invoked when request/response stream is complete, whether successful or unsuccessful
     * Optional.
     * See `aws_http_on_stream_complete_fn`.
     */
    aws_http_on_stream_complete_fn *on_complete;

    /* Callback for when the request/response stream is completely destroyed. */
    aws_http_on_stream_destroy_fn *on_destroy;
};

/**
 * Invoked when the data stream of an outgoing HTTP write operation is no longer in use.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * @param stream        HTTP-stream this write operation was submitted to.
 * @param error_code    If error_code is AWS_ERROR_SUCCESS (0), the data was successfully sent.
 *                      Any other error_code indicates that the HTTP-stream is in the process of terminating.
 *                      If the error_code is AWS_ERROR_HTTP_STREAM_HAS_COMPLETED,
 *                      the stream's termination has nothing to do with this write operation.
 *                      Any other non-zero error code indicates a problem with this particular write
 *                      operation's data.
 * @param user_data     User data for this write operation.
 */
typedef void aws_http_stream_write_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data);

/**
 * Invoked when the data of an outgoing HTTP/1.1 chunk is no longer in use.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * @param stream        HTTP-stream this chunk was submitted to.
 * @param error_code    If error_code is AWS_ERROR_SUCCESS (0), the data was successfully sent.
 *                      Any other error_code indicates that the HTTP-stream is in the process of terminating.
 *                      If the error_code is AWS_ERROR_HTTP_STREAM_HAS_COMPLETED,
 *                      the stream's termination has nothing to do with this chunk.
 *                      Any other non-zero error code indicates a problem with this particular chunk's data.
 * @param user_data     User data for this chunk.
 */
typedef aws_http_stream_write_complete_fn aws_http1_stream_write_chunk_complete_fn;

/**
 * HTTP/1.1 chunk extension for chunked encoding.
 * Note that the underlying strings are not owned by the byte cursors.
 */
struct aws_http1_chunk_extension {
    struct aws_byte_cursor key;
    struct aws_byte_cursor value;
};

/**
 * Encoding options for an HTTP/1.1 chunked transfer encoding chunk.
 */
struct aws_http1_chunk_options {
    /*
     * The data stream to be sent in a single chunk.
     * The aws_input_stream must remain valid until on_complete is invoked.
     * May be NULL in the final chunk with size 0.
     *
     * Note that, for Transfer-Encodings other than "chunked", the data is
     * expected to already have that encoding applied. For example, if
     * "Transfer-Encoding: gzip, chunked" then the data from aws_input_stream
     * should already be in gzip format.
     */
    struct aws_input_stream *chunk_data;

    /*
     * Size of the chunk_data input stream in bytes.
     */
    uint64_t chunk_data_size;

    /**
     * A pointer to an array of chunked extensions.
     * The num_extensions must match the length of the array.
     * This data is deep-copied by aws_http1_stream_write_chunk(),
     * it does not need to remain valid until on_complete is invoked.
     */
    struct aws_http1_chunk_extension *extensions;

    /**
     * The number of elements defined in the extensions array.
     */
    size_t num_extensions;

    /**
     * Invoked when the chunk data is no longer in use, whether or not it was successfully sent.
     * Optional.
     * See `aws_http1_stream_write_chunk_complete_fn`.
     */
    aws_http1_stream_write_chunk_complete_fn *on_complete;

    /**
     * User provided data passed to the on_complete callback on its invocation.
     */
    void *user_data;
};

/**
 * Invoked when the data of an outgoing HTTP2 data frame is no longer in use.
 * This is always invoked on the HTTP connection's event-loop thread.
 *
 * @param stream        HTTP2-stream this write was submitted to.
 * @param error_code    If error_code is AWS_ERROR_SUCCESS (0), the data was successfully sent.
 *                      Any other error_code indicates that the HTTP-stream is in the process of terminating.
 *                      If the error_code is AWS_ERROR_HTTP_STREAM_HAS_COMPLETED,
 *                      the stream's termination has nothing to do with this write.
 *                      Any other non-zero error code indicates a problem with this particular write's data.
 * @param user_data     User data for this write.
 */
typedef aws_http_stream_write_complete_fn aws_http2_stream_write_data_complete_fn;

/**
 * Encoding options for manual H2 data frame writes
 */
struct aws_http2_stream_write_data_options {
    /**
     * The data to be sent.
     * Optional.
     * If not set, input stream with length 0 will be used.
     */
    struct aws_input_stream *data;

    /**
     * Set true when it's the last chunk to be sent.
     * After a write with end_stream, no more data write will be accepted.
     */
    bool end_stream;

    /**
     * Invoked when the data stream is no longer in use, whether or not it was successfully sent.
     * Optional.
     * See `aws_http2_stream_write_data_complete_fn`.
     */
    aws_http2_stream_write_data_complete_fn *on_complete;

    /**
     * User provided data passed to the on_complete callback on its invocation.
     */
    void *user_data;
};

#define AWS_HTTP_REQUEST_HANDLER_OPTIONS_INIT                                                                          \
    {                                                                                                                  \
        .self_size = sizeof(struct aws_http_request_handler_options),                                                  \
    }

AWS_EXTERN_C_BEGIN

/**
 * Return whether both names are equivalent.
 * This is a case-insensitive string comparison.
 *
 * Example Matches:
 * "Content-Length" == "content-length" // upper or lower case ok

 * Example Mismatches:
 * "Content-Length" != " Content-Length" // leading whitespace bad
 */
AWS_HTTP_API
bool aws_http_header_name_eq(struct aws_byte_cursor name_a, struct aws_byte_cursor name_b);

/**
 * Create a new headers object.
 * The caller has a hold on the object and must call aws_http_headers_release() when they are done with it.
 */
AWS_HTTP_API
struct aws_http_headers *aws_http_headers_new(struct aws_allocator *allocator);

/**
 * Acquire a hold on the object, preventing it from being deleted until
 * aws_http_headers_release() is called by all those with a hold on it.
 */
AWS_HTTP_API
void aws_http_headers_acquire(struct aws_http_headers *headers);

/**
 * Release a hold on the object.
 * The object is deleted when all holds on it are released.
 */
AWS_HTTP_API
void aws_http_headers_release(struct aws_http_headers *headers);

/**
 * Add a header.
 * The underlying strings are copied.
 */
AWS_HTTP_API
int aws_http_headers_add_header(struct aws_http_headers *headers, const struct aws_http_header *header);

/**
 * Add a header.
 * The underlying strings are copied.
 */
AWS_HTTP_API
int aws_http_headers_add(struct aws_http_headers *headers, struct aws_byte_cursor name, struct aws_byte_cursor value);

/**
 * Add an array of headers.
 * The underlying strings are copied.
 */
AWS_HTTP_API
int aws_http_headers_add_array(struct aws_http_headers *headers, const struct aws_http_header *array, size_t count);

/**
 * Set a header value.
 * The header is added if necessary and any existing values for this name are removed.
 * The underlying strings are copied.
 */
AWS_HTTP_API
int aws_http_headers_set(struct aws_http_headers *headers, struct aws_byte_cursor name, struct aws_byte_cursor value);

/**
 * Get the total number of headers.
 */
AWS_HTTP_API
size_t aws_http_headers_count(const struct aws_http_headers *headers);

/**
 * Get the header at the specified index.
 * The index of a given header may change any time headers are modified.
 * When iterating headers, the following ordering rules apply:
 *
 * - Headers with the same name will always be in the same order, relative to one another.
 *   If "A: one" is added before "A: two", then "A: one" will always precede "A: two".
 *
 * - Headers with different names could be in any order, relative to one another.
 *   If "A: one" is seen before "B: bee" in one iteration, you might see "B: bee" before "A: one" on the next.
 *
 * AWS_ERROR_INVALID_INDEX is raised if the index is invalid.
 */
AWS_HTTP_API
int aws_http_headers_get_index(
    const struct aws_http_headers *headers,
    size_t index,
    struct aws_http_header *out_header);

/**
 *
 * Get all values with this name, combined into one new aws_string that you are responsible for destroying.
 * If there are multiple headers with this name, their values are appended with comma-separators.
 * If there are no headers with this name, NULL is returned and AWS_ERROR_HTTP_HEADER_NOT_FOUND is raised.
 */
AWS_HTTP_API
struct aws_string *aws_http_headers_get_all(const struct aws_http_headers *headers, struct aws_byte_cursor name);

/**
 * Get the first value for this name, ignoring any additional values.
 * AWS_ERROR_HTTP_HEADER_NOT_FOUND is raised if the name is not found.
 */
AWS_HTTP_API
int aws_http_headers_get(
    const struct aws_http_headers *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor *out_value);

/**
 * Test if header name exists or not in headers
 */
AWS_HTTP_API
bool aws_http_headers_has(const struct aws_http_headers *headers, struct aws_byte_cursor name);

/**
 * Remove all headers with this name.
 * AWS_ERROR_HTTP_HEADER_NOT_FOUND is raised if no headers with this name are found.
 */
AWS_HTTP_API
int aws_http_headers_erase(struct aws_http_headers *headers, struct aws_byte_cursor name);

/**
 * Remove the first header found with this name and value.
 * AWS_ERROR_HTTP_HEADER_NOT_FOUND is raised if no such header is found.
 */
AWS_HTTP_API
int aws_http_headers_erase_value(
    struct aws_http_headers *headers,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value);

/**
 * Remove the header at the specified index.
 *
 * AWS_ERROR_INVALID_INDEX is raised if the index is invalid.
 */
AWS_HTTP_API
int aws_http_headers_erase_index(struct aws_http_headers *headers, size_t index);

/**
 * Clear all headers.
 */
AWS_HTTP_API
void aws_http_headers_clear(struct aws_http_headers *headers);

/**
 * Get the `:method` value (HTTP/2 headers only).
 */
AWS_HTTP_API
int aws_http2_headers_get_request_method(const struct aws_http_headers *h2_headers, struct aws_byte_cursor *out_method);

/**
 * Set `:method` (HTTP/2 headers only).
 * The headers makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http2_headers_set_request_method(struct aws_http_headers *h2_headers, struct aws_byte_cursor method);

/*
 * Get the `:scheme` value (HTTP/2 headers only).
 */
AWS_HTTP_API
int aws_http2_headers_get_request_scheme(const struct aws_http_headers *h2_headers, struct aws_byte_cursor *out_scheme);

/**
 * Set `:scheme` (request pseudo headers only).
 * The pseudo headers makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http2_headers_set_request_scheme(struct aws_http_headers *h2_headers, struct aws_byte_cursor scheme);

/*
 * Get the `:authority` value (request pseudo headers only).
 */
AWS_HTTP_API
int aws_http2_headers_get_request_authority(
    const struct aws_http_headers *h2_headers,
    struct aws_byte_cursor *out_authority);

/**
 * Set `:authority` (request pseudo headers only).
 * The pseudo headers makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http2_headers_set_request_authority(struct aws_http_headers *h2_headers, struct aws_byte_cursor authority);

/*
 * Get the `:path` value (request pseudo headers only).
 */
AWS_HTTP_API
int aws_http2_headers_get_request_path(const struct aws_http_headers *h2_headers, struct aws_byte_cursor *out_path);

/**
 * Set `:path` (request pseudo headers only).
 * The pseudo headers makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http2_headers_set_request_path(struct aws_http_headers *h2_headers, struct aws_byte_cursor path);

/**
 * Get `:status` (response pseudo headers only).
 * If no status is set, AWS_ERROR_HTTP_DATA_NOT_AVAILABLE is raised.
 */
AWS_HTTP_API
int aws_http2_headers_get_response_status(const struct aws_http_headers *h2_headers, int *out_status_code);

/**
 * Set `:status` (response pseudo headers only).
 */
AWS_HTTP_API
int aws_http2_headers_set_response_status(struct aws_http_headers *h2_headers, int status_code);

/**
 * Create a new HTTP/1.1 request message.
 * The message is blank, all properties (method, path, etc) must be set individually.
 * If HTTP/1.1 message used in HTTP/2 connection, the transformation will be automatically applied.
 * A HTTP/2 message will created and sent based on the HTTP/1.1 message.
 *
 * The caller has a hold on the object and must call aws_http_message_release() when they are done with it.
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_new_request(struct aws_allocator *allocator);

/**
 * Like aws_http_message_new_request(), but uses existing aws_http_headers instead of creating a new one.
 * Acquires a hold on the headers, and releases it when the request is destroyed.
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_new_request_with_headers(
    struct aws_allocator *allocator,
    struct aws_http_headers *existing_headers);

/**
 * Create a new HTTP/1.1 response message.
 * The message is blank, all properties (status, headers, etc) must be set individually.
 *
 * The caller has a hold on the object and must call aws_http_message_release() when they are done with it.
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_new_response(struct aws_allocator *allocator);

/**
 * Create a new HTTP/2 request message.
 * pseudo headers need to be set from aws_http2_headers_set_request_* to the headers of the aws_http_message.
 * Will be errored out if used in HTTP/1.1 connection.
 *
 * The caller has a hold on the object and must call aws_http_message_release() when they are done with it.
 */
AWS_HTTP_API
struct aws_http_message *aws_http2_message_new_request(struct aws_allocator *allocator);

/**
 * Create a new HTTP/2 response message.
 * pseudo headers need to be set from aws_http2_headers_set_response_status to the headers of the aws_http_message.
 * Will be errored out if used in HTTP/1.1 connection.
 *
 * The caller has a hold on the object and must call aws_http_message_release() when they are done with it.
 */
AWS_HTTP_API
struct aws_http_message *aws_http2_message_new_response(struct aws_allocator *allocator);

/**
 * Create an HTTP/2 message from HTTP/1.1 message.
 * pseudo headers will be created from the context and added to the headers of new message.
 * Normal headers will be copied to the headers of new message.
 * Note:
 *  - if `host` exist, it will be removed and `:authority` will be added using the information.
 *  - `:scheme` always defaults to "https". To use a different scheme create the HTTP/2 message directly
 */
AWS_HTTP_API
struct aws_http_message *aws_http2_message_new_from_http1(
    struct aws_allocator *alloc,
    const struct aws_http_message *http1_msg);

/**
 * Acquire a hold on the object, preventing it from being deleted until
 * aws_http_message_release() is called by all those with a hold on it.
 *
 * This function returns the passed in message (possibly NULL) so that acquire-and-assign can be done with a single
 * statement.
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_acquire(struct aws_http_message *message);

/**
 * Release a hold on the object.
 * The object is deleted when all holds on it are released.
 *
 * This function always returns NULL so that release-and-assign-NULL can be done with a single statement.
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_release(struct aws_http_message *message);

/**
 * Deprecated. This is equivalent to aws_http_message_release().
 */
AWS_HTTP_API
void aws_http_message_destroy(struct aws_http_message *message);

AWS_HTTP_API
bool aws_http_message_is_request(const struct aws_http_message *message);

AWS_HTTP_API
bool aws_http_message_is_response(const struct aws_http_message *message);

/**
 * Get the protocol version of the http message.
 */
AWS_HTTP_API
enum aws_http_version aws_http_message_get_protocol_version(const struct aws_http_message *message);

/**
 * Get the method (request messages only).
 */
AWS_HTTP_API
int aws_http_message_get_request_method(
    const struct aws_http_message *request_message,
    struct aws_byte_cursor *out_method);

/**
 * Set the method (request messages only).
 * The request makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http_message_set_request_method(struct aws_http_message *request_message, struct aws_byte_cursor method);

/*
 * Get the path-and-query value (request messages only).
 */
AWS_HTTP_API
int aws_http_message_get_request_path(const struct aws_http_message *request_message, struct aws_byte_cursor *out_path);

/**
 * Set the path-and-query value (request messages only).
 * The request makes its own copy of the underlying string.
 */
AWS_HTTP_API
int aws_http_message_set_request_path(struct aws_http_message *request_message, struct aws_byte_cursor path);

/**
 * Get the status code (response messages only).
 * If no status is set, AWS_ERROR_HTTP_DATA_NOT_AVAILABLE is raised.
 */
AWS_HTTP_API
int aws_http_message_get_response_status(const struct aws_http_message *response_message, int *out_status_code);

/**
 * Set the status code (response messages only).
 */
AWS_HTTP_API
int aws_http_message_set_response_status(struct aws_http_message *response_message, int status_code);

/**
 * Get the body stream.
 * Returns NULL if no body stream is set.
 */
AWS_HTTP_API
struct aws_input_stream *aws_http_message_get_body_stream(const struct aws_http_message *message);

/**
 * Set the body stream.
 * NULL is an acceptable value for messages with no body.
 * Note: The message does NOT take ownership of the body stream.
 * The stream must not be destroyed until the message is complete.
 */
AWS_HTTP_API
void aws_http_message_set_body_stream(struct aws_http_message *message, struct aws_input_stream *body_stream);

/**
 * aws_future<aws_http_message*>
 */
AWS_FUTURE_T_POINTER_WITH_RELEASE_DECLARATION(aws_future_http_message, struct aws_http_message, AWS_HTTP_API)

/**
 * Submit a chunk of data to be sent on an HTTP/1.1 stream.
 * The stream must have specified "chunked" in a "transfer-encoding" header.
 * For client streams, activate() must be called before any chunks are submitted.
 * For server streams, the response must be submitted before any chunks.
 * A final chunk with size 0 must be submitted to successfully complete the HTTP-stream.
 *
 * Returns AWS_OP_SUCCESS if the chunk has been submitted. The chunk's completion
 * callback will be invoked when the HTTP-stream is done with the chunk data,
 * whether or not it was successfully sent (see `aws_http1_stream_write_chunk_complete_fn`).
 * The chunk data must remain valid until the completion callback is invoked.
 *
 * Returns AWS_OP_ERR and raises an error if the chunk could not be submitted.
 * In this case, the chunk's completion callback will never be invoked.
 * Note that it is always possible for the HTTP-stream to terminate unexpectedly
 * prior to this call being made, in which case the error raised is
 * AWS_ERROR_HTTP_STREAM_HAS_COMPLETED.
 */
AWS_HTTP_API int aws_http1_stream_write_chunk(
    struct aws_http_stream *http1_stream,
    const struct aws_http1_chunk_options *options);

/**
 * The stream must have specified `http2_use_manual_data_writes` during request creation.
 * For client streams, activate() must be called before any frames are submitted.
 * For server streams, the response headers must be submitted before any frames.
 * A write with options that has end_stream set to be true will end the stream and prevent any further write.
 *
 * @return AWS_OP_SUCCESS if the write was queued
 *         AWS_OP_ERROR indicating the attempt raised an error code.
 *              AWS_ERROR_INVALID_STATE will be raised for invalid usage.
 *              AWS_ERROR_HTTP_STREAM_HAS_COMPLETED will be raised if the stream ended for reasons behind the scenes.
 *
 * Typical usage will be something like:
 * options.http2_use_manual_data_writes = true;
 * stream = aws_http_connection_make_request(connection, &options);
 * aws_http_stream_activate(stream);
 * ...
 * struct aws_http2_stream_write_data_options write;
 * aws_http2_stream_write_data(stream, &write);
 * ...
 * struct aws_http2_stream_write_data_options last_write;
 * last_write.end_stream = true;
 * aws_http2_stream_write_data(stream, &write);
 * ...
 * aws_http_stream_release(stream);
 */
AWS_HTTP_API int aws_http2_stream_write_data(
    struct aws_http_stream *http2_stream,
    const struct aws_http2_stream_write_data_options *options);

/**
 * Add a list of headers to be added as trailing headers sent after the last chunk is sent.
 * a "Trailer" header field which indicates the fields present in the trailer.
 *
 * Certain headers are forbidden in the trailer (e.g., Transfer-Encoding, Content-Length, Host). See RFC-7541
 * Section 4.1.2 for more details.
 *
 * For client streams, activate() must be called before any chunks are submitted.
 *
 * For server streams, the response must be submitted before the trailer can be added
 *
 * aws_http1_stream_add_chunked_trailer must be called before the final size 0 chunk, and at the moment can only
 * be called once, though this could change if need be.
 *
 * Returns AWS_OP_SUCCESS if the chunk has been submitted.
 */
AWS_HTTP_API int aws_http1_stream_add_chunked_trailer(
    struct aws_http_stream *http1_stream,
    const struct aws_http_headers *trailing_headers);

/**
 *
 * This datastructure has more functions for inspecting and modifying headers than
 * are available on the aws_http_message datastructure.
 */
AWS_HTTP_API
struct aws_http_headers *aws_http_message_get_headers(const struct aws_http_message *message);

/**
 * Get the message's const aws_http_headers.
 */
AWS_HTTP_API
const struct aws_http_headers *aws_http_message_get_const_headers(const struct aws_http_message *message);

/**
 * Get the number of headers.
 */
AWS_HTTP_API
size_t aws_http_message_get_header_count(const struct aws_http_message *message);

/**
 * Get the header at the specified index.
 * This function cannot fail if a valid index is provided.
 * Otherwise, AWS_ERROR_INVALID_INDEX will be raised.
 *
 * The underlying strings are stored within the message.
 */
AWS_HTTP_API
int aws_http_message_get_header(
    const struct aws_http_message *message,
    struct aws_http_header *out_header,
    size_t index);

/**
 * Add a header to the end of the array.
 * The message makes its own copy of the underlying strings.
 */
AWS_HTTP_API
int aws_http_message_add_header(struct aws_http_message *message, struct aws_http_header header);

/**
 * Add an array of headers to the end of the header array.
 * The message makes its own copy of the underlying strings.
 *
 * This is a helper function useful when it's easier to define headers as a stack array, rather than calling add_header
 * repeatedly.
 */
AWS_HTTP_API
int aws_http_message_add_header_array(
    struct aws_http_message *message,
    const struct aws_http_header *headers,
    size_t num_headers);

/**
 * Remove the header at the specified index.
 * Headers after this index are all shifted back one position.
 *
 * This function cannot fail if a valid index is provided.
 * Otherwise, AWS_ERROR_INVALID_INDEX will be raised.
 */
AWS_HTTP_API
int aws_http_message_erase_header(struct aws_http_message *message, size_t index);

/**
 * Create a stream, with a client connection sending a request.
 * The request does not start sending automatically once the stream is created. You must call
 * aws_http_stream_activate to begin execution of the request.
 *
 * The `options` are copied during this call.
 *
 * Tip for language bindings: Do not bind the `options` struct. Use something more natural for your language,
 * such as Builder Pattern in Java, or Python's ability to take many optional arguments by name.
 *
 * Note: The header of the request will be sent as it is when the message to send protocol matches the protocol of the
 * connection.
 *  - No `user-agent` will be added.
 *  - No security check will be enforced. eg: `referer` header privacy should be enforced by the user-agent who adds the
 *      header
 *  - When HTTP/1 message sent on HTTP/2 connection, `aws_http2_message_new_from_http1` will be applied under the hood.
 *  - When HTTP/2 message sent on HTTP/1 connection, no change will be made.
 */
AWS_HTTP_API
struct aws_http_stream *aws_http_connection_make_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options);

/**
 * Create a stream, with a server connection receiving and responding to a request.
 * This function can only be called from the `aws_http_on_incoming_request_fn` callback.
 * aws_http_stream_send_response() should be used to send a response.
 */
AWS_HTTP_API
struct aws_http_stream *aws_http_stream_new_server_request_handler(
    const struct aws_http_request_handler_options *options);

/**
 * Acquire refcount on the stream to prevent it from being cleaned up until it is released.
 */
AWS_HTTP_API
struct aws_http_stream *aws_http_stream_acquire(struct aws_http_stream *stream);

/**
 * Users must release the stream when they are done with it, or its memory will never be cleaned up.
 * This will not cancel the stream, its callbacks will still fire if the stream is still in progress.
 *
 * Tips for language bindings:
 * - Invoke this from the wrapper class's finalizer/destructor.
 * - Do not let the wrapper class be destroyed until on_complete() has fired.
 */
AWS_HTTP_API
void aws_http_stream_release(struct aws_http_stream *stream);

/**
 * Only used for client initiated streams (immediately following a call to aws_http_connection_make_request).
 *
 * Activates the request's outgoing stream processing.
 */
AWS_HTTP_API int aws_http_stream_activate(struct aws_http_stream *stream);

AWS_HTTP_API
struct aws_http_connection *aws_http_stream_get_connection(const struct aws_http_stream *stream);

/* Only valid in "request" streams, once response headers start arriving */
AWS_HTTP_API
int aws_http_stream_get_incoming_response_status(const struct aws_http_stream *stream, int *out_status);

/* Only valid in "request handler" streams, once request headers start arriving */
AWS_HTTP_API
int aws_http_stream_get_incoming_request_method(
    const struct aws_http_stream *stream,
    struct aws_byte_cursor *out_method);

AWS_HTTP_API
int aws_http_stream_get_incoming_request_uri(const struct aws_http_stream *stream, struct aws_byte_cursor *out_uri);

/**
 * Send response (only callable from "request handler" streams)
 * The response object must stay alive at least until the stream's on_complete is called.
 */
AWS_HTTP_API
int aws_http_stream_send_response(struct aws_http_stream *stream, struct aws_http_message *response);

/**
 * Increment the stream's flow-control window to keep data flowing.
 *
 * If the connection was created with `manual_window_management` set true,
 * the flow-control window of each stream will shrink as body data is received
 * (headers, padding, and other metadata do not affect the window).
 * The connection's `initial_window_size` determines the starting size of each stream's window.
 * If a stream's flow-control window reaches 0, no further data will be received.
 *
 * If `manual_window_management` is false, this call will have no effect.
 * The connection maintains its flow-control windows such that
 * no back-pressure is applied and data arrives as fast as possible.
 */
AWS_HTTP_API
void aws_http_stream_update_window(struct aws_http_stream *stream, size_t increment_size);

/**
 * Gets the HTTP/2 id associated with a stream.  Even h1 streams have an id (using the same allocation procedure
 * as http/2) for easier tracking purposes. For client streams, this will only be non-zero after a successful call
 * to aws_http_stream_activate()
 */
AWS_HTTP_API
uint32_t aws_http_stream_get_id(const struct aws_http_stream *stream);

/**
 * Cancel the stream in flight.
 * For HTTP/1.1 streams, it's equivalent to closing the connection.
 * For HTTP/2 streams, it's equivalent to calling reset on the stream with `AWS_HTTP2_ERR_CANCEL`.
 *
 * the stream will complete with the error code provided, unless the stream is
 * already completing for other reasons, or the stream is not activated,
 * in which case this call will have no impact.
 */
AWS_HTTP_API
void aws_http_stream_cancel(struct aws_http_stream *stream, int error_code);

/**
 * Reset the HTTP/2 stream (HTTP/2 only).
 * Note that if the stream closes before this async call is fully processed, the RST_STREAM frame will not be sent.
 *
 * @param http2_stream HTTP/2 stream.
 * @param http2_error aws_http2_error_code. Reason to reset the stream.
 */
AWS_HTTP_API
int aws_http2_stream_reset(struct aws_http_stream *http2_stream, uint32_t http2_error);

/**
 * Get the error code received in rst_stream.
 * Only valid if the stream has completed, and an RST_STREAM frame has received.
 *
 * @param http2_stream HTTP/2 stream.
 * @param out_http2_error Gets to set to HTTP/2 error code received in rst_stream.
 */
AWS_HTTP_API
int aws_http2_stream_get_received_reset_error_code(struct aws_http_stream *http2_stream, uint32_t *out_http2_error);

/**
 * Get the HTTP/2 error code sent in the RST_STREAM frame (HTTP/2 only).
 * Only valid if the stream has completed, and has sent an RST_STREAM frame.
 *
 * @param http2_stream HTTP/2 stream.
 * @param out_http2_error Gets to set to HTTP/2 error code sent in rst_stream.
 */
AWS_HTTP_API
int aws_http2_stream_get_sent_reset_error_code(struct aws_http_stream *http2_stream, uint32_t *out_http2_error);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_REQUEST_RESPONSE_H */
