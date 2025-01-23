#ifndef AWS_HTTP_WEBSOCKET_H
#define AWS_HTTP_WEBSOCKET_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http_header;
struct aws_http_message;

/* TODO: Document lifetime stuff */
/* TODO: Document CLOSE frame behavior (when auto-sent during close, when auto-closed) */
/* TODO: Accept payload as aws_input_stream */

/**
 * A websocket connection.
 */
struct aws_websocket;

/**
 * Opcode describing the type of a websocket frame.
 * RFC-6455 Section 5.2
 */
enum aws_websocket_opcode {
    AWS_WEBSOCKET_OPCODE_CONTINUATION = 0x0,
    AWS_WEBSOCKET_OPCODE_TEXT = 0x1,
    AWS_WEBSOCKET_OPCODE_BINARY = 0x2,
    AWS_WEBSOCKET_OPCODE_CLOSE = 0x8,
    AWS_WEBSOCKET_OPCODE_PING = 0x9,
    AWS_WEBSOCKET_OPCODE_PONG = 0xA,
};

#define AWS_WEBSOCKET_MAX_PAYLOAD_LENGTH 0x7FFFFFFFFFFFFFFF
#define AWS_WEBSOCKET_MAX_HANDSHAKE_KEY_LENGTH 25
#define AWS_WEBSOCKET_CLOSE_TIMEOUT 1000000000 // nanos -> 1 sec

/**
 * Data passed to the websocket on_connection_setup callback.
 *
 * An error_code of zero indicates that setup was completely successful.
 * You own the websocket pointer now and must call aws_websocket_release() when you are done with it.
 * You can inspect the response headers, if you're interested.
 *
 * A non-zero error_code indicates that setup failed.
 * The websocket pointer will be NULL.
 * If the server sent a response, you can inspect its status-code, headers, and body,
 * but this data will NULL if setup failed before a full response could be received.
 * If you wish to persist data from the response make a deep copy.
 * The response data becomes invalid once the callback completes.
 */
struct aws_websocket_on_connection_setup_data {
    int error_code;
    struct aws_websocket *websocket;
    const int *handshake_response_status;
    const struct aws_http_header *handshake_response_header_array;
    size_t num_handshake_response_headers;
    const struct aws_byte_cursor *handshake_response_body;
};

/**
 * Called when websocket setup is complete.
 * Called exactly once on the websocket's event-loop thread.
 * See `aws_websocket_on_connection_setup_data`.
 */
typedef void(
    aws_websocket_on_connection_setup_fn)(const struct aws_websocket_on_connection_setup_data *setup, void *user_data);

/**
 * Called when the websocket has finished shutting down.
 * Called once on the websocket's event-loop thread if setup succeeded.
 * If setup failed, this is never called.
 */
typedef void(aws_websocket_on_connection_shutdown_fn)(struct aws_websocket *websocket, int error_code, void *user_data);

/**
 * Data about an incoming frame.
 * See RFC-6455 Section 5.2.
 */
struct aws_websocket_incoming_frame {
    uint64_t payload_length;
    uint8_t opcode;
    bool fin;
};

/**
 * Called when a new frame arrives.
 * Invoked once per frame on the websocket's event-loop thread.
 * Each incoming-frame-begin call will eventually be followed by an incoming-frame-complete call,
 * before the next frame begins and before the websocket shuts down.
 *
 * Return true to proceed normally. If false is returned, the websocket will read no further data,
 * the frame will complete with an error-code, and the connection will close.
 */
typedef bool(aws_websocket_on_incoming_frame_begin_fn)(
    struct aws_websocket *websocket,
    const struct aws_websocket_incoming_frame *frame,
    void *user_data);

/**
 * Called repeatedly as payload data arrives.
 * Invoked 0 or more times on the websocket's event-loop thread.
 * Payload data will not be valid after this call, so copy if necessary.
 * The payload data is always unmasked at this point.
 *
 * NOTE: If you created the websocket with `manual_window_management` set true, you must maintain the read window.
 * Whenever the read window reaches 0, you will stop receiving anything.
 * The websocket's `initial_window_size` determines the starting size of the read window.
 * The read window shrinks as you receive the payload from "data" frames (TEXT, BINARY, and CONTINUATION).
 * Use aws_websocket_increment_read_window() to increment the window again and keep frames flowing.
 * Maintain a larger window to keep up high throughput.
 * You only need to worry about the payload from "data" frames.
 * The websocket automatically increments the window to account for any
 * other incoming bytes, including other parts of a frame (opcode, payload-length, etc)
 * and the payload of other frame types (PING, PONG, CLOSE).
 *
 * Return true to proceed normally. If false is returned, the websocket will read no further data,
 * the frame will complete with an error-code, and the connection will close.
 */
typedef bool(aws_websocket_on_incoming_frame_payload_fn)(
    struct aws_websocket *websocket,
    const struct aws_websocket_incoming_frame *frame,
    struct aws_byte_cursor data,
    void *user_data);

/**
 * Called when done processing an incoming frame.
 * If error_code is non-zero, an error occurred and the payload may not have been completely received.
 * Invoked once per frame on the websocket's event-loop thread.
 *
 * Return true to proceed normally. If false is returned, the websocket will read no further data
 * and the connection will close.
 */
typedef bool(aws_websocket_on_incoming_frame_complete_fn)(
    struct aws_websocket *websocket,
    const struct aws_websocket_incoming_frame *frame,
    int error_code,
    void *user_data);

/**
 * Options for creating a websocket client connection.
 */
struct aws_websocket_client_connection_options {
    /**
     * Required.
     * Must outlive the connection.
     */
    struct aws_allocator *allocator;

    /**
     * Required.
     * The connection keeps the bootstrap alive via ref-counting.
     */
    struct aws_client_bootstrap *bootstrap;

    /**
     * Required.
     * aws_websocket_client_connect() makes a copy.
     */
    const struct aws_socket_options *socket_options;

    /**
     * Optional.
     * aws_websocket_client_connect() deep-copies all contents,
     * and keeps the `aws_tls_ctx` alive via ref-counting.
     */
    const struct aws_tls_connection_options *tls_options;

    /**
     * Optional
     * Configuration options related to http proxy usage.
     */
    const struct aws_http_proxy_options *proxy_options;

    /**
     * Required.
     * aws_websocket_client_connect() makes a copy.
     */
    struct aws_byte_cursor host;

    /**
     * Optional.
     * Defaults to 443 if tls_options is present, 80 if it is not.
     */
    uint32_t port;

    /**
     * Required.
     * The request will be kept alive via ref-counting until the handshake completes.
     * Suggestion: create via aws_http_message_new_websocket_handshake_request()
     *
     * The method MUST be set to GET.
     * The following headers are required (replace values in []):
     *
     * Host: [server.example.com]
     * Upgrade: websocket
     * Connection: Upgrade
     * Sec-WebSocket-Key: [dGhlIHNhbXBsZSBub25jZQ==]
     * Sec-WebSocket-Version: 13
     *
     * Sec-Websocket-Key should be a random 16 bytes value, Base64 encoded.
     */
    struct aws_http_message *handshake_request;

    /**
     * Initial size of the websocket's read window.
     * Ignored unless `manual_window_management` is true.
     * Set to 0 to prevent any incoming websocket frames until aws_websocket_increment_read_window() is called.
     */
    size_t initial_window_size;

    /**
     * User data for callbacks.
     * Optional.
     */
    void *user_data;

    /**
     * Called when connect completes.
     * Required.
     * If unsuccessful, error_code will be set, connection will be NULL,
     * and the on_connection_shutdown callback will never be called.
     * If successful, the user is now responsible for the websocket and must
     * call aws_websocket_release() when they are done with it.
     */
    aws_websocket_on_connection_setup_fn *on_connection_setup;

    /**
     * Called when connection has finished shutting down.
     * Optional.
     * Never called if `on_connection_setup` reported failure.
     * Note that the connection is not completely done until `on_connection_shutdown` has been called
     * AND aws_websocket_release() has been called.
     */
    aws_websocket_on_connection_shutdown_fn *on_connection_shutdown;

    /**
     * Called when each new frame arrives.
     * Optional.
     * See `aws_websocket_on_incoming_frame_begin_fn`.
     */
    aws_websocket_on_incoming_frame_begin_fn *on_incoming_frame_begin;

    /**
     * Called repeatedly as payload data arrives.
     * Optional.
     * See `aws_websocket_on_incoming_frame_payload_fn`.
     */
    aws_websocket_on_incoming_frame_payload_fn *on_incoming_frame_payload;

    /**
     * Called when done processing an incoming frame.
     * Optional.
     * See `aws_websocket_on_incoming_frame_complete_fn`.
     */
    aws_websocket_on_incoming_frame_complete_fn *on_incoming_frame_complete;

    /**
     * Set to true to manually manage the read window size.
     *
     * If this is false, no backpressure is applied and frames will arrive as fast as possible.
     *
     * If this is true, then whenever the read window reaches 0 you will stop receiving anything.
     * The websocket's `initial_window_size` determines the starting size of the read window.
     * The read window shrinks as you receive the payload from "data" frames (TEXT, BINARY, and CONTINUATION).
     * Use aws_websocket_increment_read_window() to increment the window again and keep frames flowing.
     * Maintain a larger window to keep up high throughput.
     * You only need to worry about the payload from "data" frames.
     * The websocket automatically increments the window to account for any
     * other incoming bytes, including other parts of a frame (opcode, payload-length, etc)
     * and the payload of other frame types (PING, PONG, CLOSE).
     */
    bool manual_window_management;

    /**
     * Optional
     * If set, requests that a specific event loop be used to seat the connection, rather than the next one
     * in the event loop group.  Useful for serializing all io and external events related to a client onto
     * a single thread.
     */
    struct aws_event_loop *requested_event_loop;

    /**
     * Optional
     * Host resolution override that allows the user to override DNS behavior for this particular connection.
     */
    const struct aws_host_resolution_config *host_resolution_config;
};

/**
 * Called repeatedly as the websocket's payload is streamed out.
 * The user should write payload data to out_buf, up to available capacity.
 * The websocket will mask this data for you, if necessary.
 * Invoked repeatedly on the websocket's event-loop thread.
 *
 * Return true to proceed normally. If false is returned, the websocket will send no further data,
 * the frame will complete with an error-code, and the connection will close.
 */
typedef bool(aws_websocket_stream_outgoing_payload_fn)(
    struct aws_websocket *websocket,
    struct aws_byte_buf *out_buf,
    void *user_data);

/**
 * Called when a aws_websocket_send_frame() operation completes.
 * error_code will be zero if the operation was successful.
 * "Success" does not guarantee that the peer actually received or processed the frame.
 * Invoked exactly once per sent frame on the websocket's event-loop thread.
 */
typedef void(
    aws_websocket_outgoing_frame_complete_fn)(struct aws_websocket *websocket, int error_code, void *user_data);

/**
 * Options for sending a websocket frame.
 * This structure is copied immediately by aws_websocket_send().
 * For descriptions of opcode, fin, and payload_length see in RFC-6455 Section 5.2.
 */
struct aws_websocket_send_frame_options {
    /**
     * Size of payload to be sent via `stream_outgoing_payload` callback.
     */
    uint64_t payload_length;

    /**
     * User data passed to callbacks.
     */
    void *user_data;

    /**
     * Callback for sending payload data.
     * See `aws_websocket_stream_outgoing_payload_fn`.
     * Required if `payload_length` is non-zero.
     */
    aws_websocket_stream_outgoing_payload_fn *stream_outgoing_payload;

    /**
     * Callback for completion of send operation.
     * See `aws_websocket_outgoing_frame_complete_fn`.
     * Optional.
     */
    aws_websocket_outgoing_frame_complete_fn *on_complete;

    /**
     * Frame type.
     * `aws_websocket_opcode` enum provides standard values.
     */
    uint8_t opcode;

    /**
     * Indicates that this is the final fragment in a message. The first fragment MAY also be the final fragment.
     */
    bool fin;
};

AWS_EXTERN_C_BEGIN

/**
 * Return true if opcode is for a data frame, false if opcode if for a control frame.
 */
AWS_HTTP_API
bool aws_websocket_is_data_frame(uint8_t opcode);

/**
 * Asynchronously establish a client websocket connection.
 * The on_connection_setup callback is invoked when the operation has finished creating a connection, or failed.
 */
AWS_HTTP_API
int aws_websocket_client_connect(const struct aws_websocket_client_connection_options *options);

/**
 * Increment the websocket's ref-count, preventing it from being destroyed.
 * @return Always returns the same pointer that is passed in.
 */
AWS_HTTP_API
struct aws_websocket *aws_websocket_acquire(struct aws_websocket *websocket);

/**
 * Decrement the websocket's ref-count.
 * When the ref-count reaches zero, the connection will shut down, if it hasn't already.
 * Users must release the websocket when they are done with it.
 * The websocket's memory cannot be reclaimed until this is done.
 * Callbacks may continue firing after this is called, with "shutdown" being the final callback.
 * This function may be called from any thread.
 *
 * It is safe to pass NULL, nothing will happen.
 */
AWS_HTTP_API
void aws_websocket_release(struct aws_websocket *websocket);

/**
 * Close the websocket connection.
 * It is safe to call this, even if the connection is already closed or closing.
 * The websocket will attempt to send a CLOSE frame during normal shutdown.
 * If `free_scarce_resources_immediately` is true, the connection will be torn down as quickly as possible.
 * This function may be called from any thread.
 */
AWS_HTTP_API
void aws_websocket_close(struct aws_websocket *websocket, bool free_scarce_resources_immediately);

/**
 * Send a websocket frame.
 * The `options` struct is copied.
 * A callback will be invoked when the operation completes.
 * This function may be called from any thread.
 */
AWS_HTTP_API
int aws_websocket_send_frame(struct aws_websocket *websocket, const struct aws_websocket_send_frame_options *options);

/**
 * Manually increment the read window to keep frames flowing.
 *
 * If the websocket was created with `manual_window_management` set true,
 * then whenever the read window reaches 0 you will stop receiving data.
 * The websocket's `initial_window_size` determines the starting size of the read window.
 * The read window shrinks as you receive the payload from "data" frames (TEXT, BINARY, and CONTINUATION).
 * Use aws_websocket_increment_read_window() to increment the window again and keep frames flowing.
 * Maintain a larger window to keep up high throughput.
 * You only need to worry about the payload from "data" frames.
 * The websocket automatically increments the window to account for any
 * other incoming bytes, including other parts of a frame (opcode, payload-length, etc)
 * and the payload of other frame types (PING, PONG, CLOSE).
 *
 * If the websocket was created with `manual_window_management` set false, this function does nothing.
 *
 * This function may be called from any thread.
 */
AWS_HTTP_API
void aws_websocket_increment_read_window(struct aws_websocket *websocket, size_t size);

/**
 * Convert the websocket into a mid-channel handler.
 * The websocket will stop being usable via its public API and become just another handler in the channel.
 * The caller will likely install a channel handler to the right.
 * This must not be called in the middle of an incoming frame (between "frame begin" and "frame complete" callbacks).
 * This MUST be called from the websocket's thread.
 *
 * If successful:
 * - Other than aws_websocket_release(), all calls to aws_websocket_x() functions are ignored.
 * - The websocket will no longer invoke any "incoming frame" callbacks.
 * - aws_io_messages written by a downstream handler will be wrapped in binary data frames and sent upstream.
 *   The data may be split/combined as it is sent along.
 * - aws_io_messages read from upstream handlers will be scanned for binary data frames.
 *   The payloads of these frames will be sent downstream.
 *   The payloads may be split/combined as they are sent along.
 * - An incoming close frame will automatically result in channel-shutdown.
 * - aws_websocket_release() must still be called or the websocket and its channel will never be cleaned up.
 * - The websocket will still invoke its "on connection shutdown" callback when channel shutdown completes.
 *
 * If unsuccessful, NULL is returned and the websocket is unchanged.
 */
AWS_HTTP_API
int aws_websocket_convert_to_midchannel_handler(struct aws_websocket *websocket);

/**
 * Returns the websocket's underlying I/O channel.
 */
AWS_HTTP_API
struct aws_channel *aws_websocket_get_channel(const struct aws_websocket *websocket);

/**
 * Generate value for a Sec-WebSocket-Key header and write it into `dst` buffer.
 * The buffer should have at least AWS_WEBSOCKET_MAX_HANDSHAKE_KEY_LENGTH space available.
 *
 * This value is the base64 encoding of a random 16-byte value.
 * RFC-6455 Section 4.1
 */
AWS_HTTP_API
int aws_websocket_random_handshake_key(struct aws_byte_buf *dst);

/**
 * Create request with all required fields for a websocket upgrade request.
 * The method and path are set, and the the following headers are added:
 *
 * Host: <host>
 * Upgrade: websocket
 * Connection: Upgrade
 * Sec-WebSocket-Key: <base64 encoding of 16 random bytes>
 * Sec-WebSocket-Version: 13
 */
AWS_HTTP_API
struct aws_http_message *aws_http_message_new_websocket_handshake_request(
    struct aws_allocator *allocator,
    struct aws_byte_cursor path,
    struct aws_byte_cursor host);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_WEBSOCKET_H */
