#ifndef AWS_HTTP_WEBSOCKET_IMPL_H
#define AWS_HTTP_WEBSOCKET_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/websocket.h>

struct aws_http_client_connection_options;
struct aws_http_connection;
struct aws_http_make_request_options;

/* RFC-6455 Section 5.2 Base Framing Protocol
 * Payload length:  7 bits, 7+16 bits, or 7+64 bits
 *
 * The length of the "Payload data", in bytes: if 0-125, that is the
 * payload length.  If 126, the following 2 bytes interpreted as a
 * 16-bit unsigned integer are the payload length.  If 127, the
 * following 8 bytes interpreted as a 64-bit unsigned integer (the
 * most significant bit MUST be 0) are the payload length.  Multibyte
 * length quantities are expressed in network byte order.  Note that
 * in all cases, the minimal number of bytes MUST be used to encode
 * the length, for example, the length of a 124-byte-long string
 * can't be encoded as the sequence 126, 0, 124.  The payload length
 * is the length of the "Extension data" + the length of the
 * "Application data".  The length of the "Extension data" may be
 * zero, in which case the payload length is the length of the
 * "Application data".
 */
#define AWS_WEBSOCKET_7BIT_VALUE_FOR_2BYTE_EXTENDED_LENGTH 126
#define AWS_WEBSOCKET_7BIT_VALUE_FOR_8BYTE_EXTENDED_LENGTH 127

#define AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MIN_VALUE AWS_WEBSOCKET_7BIT_VALUE_FOR_2BYTE_EXTENDED_LENGTH
#define AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MAX_VALUE 0x000000000000FFFF

#define AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MIN_VALUE 0x0000000000010000
#define AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MAX_VALUE 0x7FFFFFFFFFFFFFFF

/* Max bytes necessary to send non-payload parts of a frame */
#define AWS_WEBSOCKET_MAX_FRAME_OVERHEAD (2 + 8 + 4) /* base + extended-length + masking-key */

/**
 * Full contents of a websocket frame, excluding the payload.
 */
struct aws_websocket_frame {
    bool fin;
    bool rsv[3];
    bool masked;
    uint8_t opcode;
    uint64_t payload_length;
    uint8_t masking_key[4];
};

struct aws_websocket_handler_options {
    struct aws_allocator *allocator;
    struct aws_channel *channel;
    size_t initial_window_size;

    void *user_data;
    aws_websocket_on_incoming_frame_begin_fn *on_incoming_frame_begin;
    aws_websocket_on_incoming_frame_payload_fn *on_incoming_frame_payload;
    aws_websocket_on_incoming_frame_complete_fn *on_incoming_frame_complete;

    bool is_server;
    bool manual_window_update;
};

struct aws_websocket_client_bootstrap_system_vtable {
    int (*aws_http_client_connect)(const struct aws_http_client_connection_options *options);
    void (*aws_http_connection_release)(struct aws_http_connection *connection);
    void (*aws_http_connection_close)(struct aws_http_connection *connection);
    struct aws_channel *(*aws_http_connection_get_channel)(struct aws_http_connection *connection);
    struct aws_http_stream *(*aws_http_connection_make_request)(
        struct aws_http_connection *client_connection,
        const struct aws_http_make_request_options *options);
    int (*aws_http_stream_activate)(struct aws_http_stream *stream);
    void (*aws_http_stream_release)(struct aws_http_stream *stream);
    struct aws_http_connection *(*aws_http_stream_get_connection)(const struct aws_http_stream *stream);
    void (*aws_http_stream_update_window)(struct aws_http_stream *stream, size_t increment_size);
    int (*aws_http_stream_get_incoming_response_status)(const struct aws_http_stream *stream, int *out_status);
    struct aws_websocket *(*aws_websocket_handler_new)(const struct aws_websocket_handler_options *options);
};

AWS_EXTERN_C_BEGIN

/**
 * Returns printable name for opcode as c-string.
 */
AWS_HTTP_API
const char *aws_websocket_opcode_str(uint8_t opcode);

/**
 * Return total number of bytes needed to encode frame and its payload
 */
AWS_HTTP_API
uint64_t aws_websocket_frame_encoded_size(const struct aws_websocket_frame *frame);

/**
 * Create a websocket channel-handler and insert it into the channel.
 */
AWS_HTTP_API
struct aws_websocket *aws_websocket_handler_new(const struct aws_websocket_handler_options *options);

/**
 * Override the functions that websocket bootstrap uses to interact with external systems.
 * Used for unit testing.
 */
AWS_HTTP_API
void aws_websocket_client_bootstrap_set_system_vtable(
    const struct aws_websocket_client_bootstrap_system_vtable *system_vtable);

AWS_EXTERN_C_END
#endif /* AWS_HTTP_WEBSOCKET_IMPL_H */
