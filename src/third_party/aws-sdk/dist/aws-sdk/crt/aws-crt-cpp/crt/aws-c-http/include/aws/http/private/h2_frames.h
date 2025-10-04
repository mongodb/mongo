#ifndef AWS_HTTP_H2_FRAMES_H
#define AWS_HTTP_H2_FRAMES_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/connection.h>
#include <aws/http/private/hpack.h>
#include <aws/http/request_response.h>

#include <aws/common/byte_buf.h>

/* Ids for each frame type (RFC-7540 6) */
enum aws_h2_frame_type {
    AWS_H2_FRAME_T_DATA = 0x00,
    AWS_H2_FRAME_T_HEADERS = 0x01,
    AWS_H2_FRAME_T_PRIORITY = 0x02,
    AWS_H2_FRAME_T_RST_STREAM = 0x03,
    AWS_H2_FRAME_T_SETTINGS = 0x04,
    AWS_H2_FRAME_T_PUSH_PROMISE = 0x05,
    AWS_H2_FRAME_T_PING = 0x06,
    AWS_H2_FRAME_T_GOAWAY = 0x07,
    AWS_H2_FRAME_T_WINDOW_UPDATE = 0x08,
    AWS_H2_FRAME_T_CONTINUATION = 0x09,
    AWS_H2_FRAME_T_UNKNOWN,
    AWS_H2_FRAME_TYPE_COUNT,
};

/* Represents flags that may be set on a frame (RFC-7540 6) */
enum aws_h2_frame_flag {
    AWS_H2_FRAME_F_ACK = 0x01,
    AWS_H2_FRAME_F_END_STREAM = 0x01,
    AWS_H2_FRAME_F_END_HEADERS = 0x04,
    AWS_H2_FRAME_F_PADDED = 0x08,
    AWS_H2_FRAME_F_PRIORITY = 0x20,
};

/* Pairs the AWS_ERROR_* to show our API user,
 * along with the AWS_HTTP2_ERR_* that should
 * be sent to the peer via RST_STREAM or GOAWAY.
 *
 * Used in place of normal error handling in functions that may result
 * in an HTTP/2 Connection Error or Stream Error.
 */
struct aws_h2err {
    enum aws_http2_error_code h2_code;
    int aws_code;
};

#define AWS_H2ERR_SUCCESS                                                                                              \
    (struct aws_h2err) {                                                                                               \
        .h2_code = 0, .aws_code = 0                                                                                    \
    }

#define AWS_H2_PAYLOAD_MAX (0x00FFFFFF)       /* must fit in 3 bytes */
#define AWS_H2_WINDOW_UPDATE_MAX (0x7FFFFFFF) /* cannot use high bit */
#define AWS_H2_STREAM_ID_MAX (0x7FFFFFFF)     /* cannot use high bit */
#define AWS_H2_FRAME_PREFIX_SIZE (9)
#define AWS_H2_INIT_WINDOW_SIZE (65535) /* Defined initial window size */

/* Legal min(inclusive) and max(inclusive) for each setting */
extern const uint32_t aws_h2_settings_bounds[AWS_HTTP2_SETTINGS_END_RANGE][2];

/* Initial values for settings RFC-7540 6.5.2 */
AWS_HTTP_API
extern const uint32_t aws_h2_settings_initial[AWS_HTTP2_SETTINGS_END_RANGE];

/* This magic string must be the very first thing a client sends to the server.
 * See RFC-7540 3.5 - HTTP/2 Connection Preface.
 * Exported for tests */
AWS_HTTP_API
extern const struct aws_byte_cursor aws_h2_connection_preface_client_string;

/**
 * Present in all frames that may have set AWS_H2_FRAME_F_PRIORITY
 *
 * Encoded as:
 * +-+-------------------------------------------------------------+
 * |E|                  Stream Dependency (31)                     |
 * +-+-------------+-----------------------------------------------+
 * |   Weight (8)  |
 * +-+-------------+
 */
struct aws_h2_frame_priority_settings {
    uint32_t stream_dependency;
    bool stream_dependency_exclusive;
    uint8_t weight;
};

/**
 * A frame to be encoded.
 * (in the case of HEADERS and PUSH_PROMISE, it might turn into multiple frames due to CONTINUATION)
 */
struct aws_h2_frame {
    const struct aws_h2_frame_vtable *vtable;
    struct aws_allocator *alloc;
    struct aws_linked_list_node node;
    enum aws_h2_frame_type type;
    uint32_t stream_id;

    /* If true, frame will be sent before those with normal priority.
     * Useful for frames like PING ACK where low latency is important. */
    bool high_priority;
};

/* Used to encode a frame */
struct aws_h2_frame_encoder {
    struct aws_allocator *allocator;
    const void *logging_id;
    struct aws_hpack_encoder hpack;
    struct aws_h2_frame *current_frame;

    /* Settings for frame encoder, which is based on the settings received from peer */
    struct {
        /*  the size of the largest frame payload */
        uint32_t max_frame_size;
    } settings;

    bool has_errored;
};

typedef void aws_h2_frame_destroy_fn(struct aws_h2_frame *frame_base);
typedef int aws_h2_frame_encode_fn(
    struct aws_h2_frame *frame_base,
    struct aws_h2_frame_encoder *encoder,
    struct aws_byte_buf *output,
    bool *complete);

struct aws_h2_frame_vtable {
    aws_h2_frame_destroy_fn *destroy;
    aws_h2_frame_encode_fn *encode;
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
const char *aws_h2_frame_type_to_str(enum aws_h2_frame_type type);

AWS_HTTP_API
const char *aws_http2_error_code_to_str(enum aws_http2_error_code h2_error_code);

/**
 * Specify which HTTP/2 error-code will be sent to the peer in a GOAWAY or RST_STREAM frame.
 *
 * The AWS_ERROR reported to the API user will be AWS_ERROR_HTTP_PROTOCOL_ERROR.
 */
AWS_HTTP_API
struct aws_h2err aws_h2err_from_h2_code(enum aws_http2_error_code h2_error_code);

/**
 * Specify which AWS_ERROR will be reported to the API user.
 *
 * The peer will be sent a GOAWAY or RST_STREAM with the INTERNAL_ERROR HTTP/2 error-code.
 */
AWS_HTTP_API
struct aws_h2err aws_h2err_from_aws_code(int aws_error_code);

AWS_HTTP_API
struct aws_h2err aws_h2err_from_last_error(void);

AWS_HTTP_API
bool aws_h2err_success(struct aws_h2err err);

AWS_HTTP_API
bool aws_h2err_failed(struct aws_h2err err);

/* Raises AWS_ERROR_INVALID_ARGUMENT if stream_id is 0 or exceeds AWS_H2_MAX_STREAM_ID */
AWS_HTTP_API
int aws_h2_validate_stream_id(uint32_t stream_id);

/**
 * The process of encoding a frame looks like:
 * 1. Create a encoder object on the stack and initialize with aws_h2_frame_encoder_init
 * 2. Encode the frame using aws_h2_encode_frame()
 */
AWS_HTTP_API
int aws_h2_frame_encoder_init(
    struct aws_h2_frame_encoder *encoder,
    struct aws_allocator *allocator,
    const void *logging_id);

AWS_HTTP_API
void aws_h2_frame_encoder_clean_up(struct aws_h2_frame_encoder *encoder);

/**
 * Attempt to encode frame into output buffer.
 * AWS_OP_ERR is returned if encoder encounters an unrecoverable error.
 * frame_complete will be set true if the frame finished encoding.
 *
 * If frame_complete is false then we MUST call aws_h2_encode_frame() again
 * with all the same inputs, when we have a fresh buffer (it would be illegal
 * to encode a different frame).
 */
AWS_HTTP_API
int aws_h2_encode_frame(
    struct aws_h2_frame_encoder *encoder,
    struct aws_h2_frame *frame,
    struct aws_byte_buf *output,
    bool *frame_complete);

/**
 * Attempt to encode a DATA frame into the output buffer.
 * The body_stream will be read into the available space (up to MAX_FRAME_SIZE).
 * AWS_OP_ERR is returned if encoder encounters an unrecoverable error.
 * body_complete will be set true if encoder reaches the end of the body_stream.
 * body_stalled will be true if aws_input_stream_read() stopped early (didn't
 * complete, though more space was available).
 *
 * Each call to this function encodes a complete DATA frame, or nothing at all,
 * so it's always safe to encode a different frame type or the body of a different stream
 * after calling this.
 */
AWS_HTTP_API
int aws_h2_encode_data_frame(
    struct aws_h2_frame_encoder *encoder,
    uint32_t stream_id,
    struct aws_input_stream *body_stream,
    bool body_ends_stream,
    uint8_t pad_length,
    int32_t *stream_window_size_peer,
    size_t *connection_window_size_peer,
    struct aws_byte_buf *output,
    bool *body_complete,
    bool *body_stalled);

AWS_HTTP_API
void aws_h2_frame_destroy(struct aws_h2_frame *frame);

/**
 * This frame type may actually end up encoding multiple frames
 * (HEADERS followed by 0 or more CONTINUATION frames).
 */
AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_headers(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    const struct aws_http_headers *headers,
    bool end_stream,
    uint8_t pad_length,
    const struct aws_h2_frame_priority_settings *optional_priority);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_priority(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    const struct aws_h2_frame_priority_settings *priority);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_rst_stream(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t error_code);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_settings(
    struct aws_allocator *allocator,
    const struct aws_http2_setting *settings_array,
    size_t num_settings,
    bool ack);

/**
 * This frame type may actually end up encoding multiple frames
 * (PUSH_PROMISE followed 0 or more CONTINUATION frames).
 */
AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_push_promise(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t promised_stream_id,
    const struct aws_http_headers *headers,
    uint8_t pad_length);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_ping(
    struct aws_allocator *allocator,
    bool ack,
    const uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE]);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_goaway(
    struct aws_allocator *allocator,
    uint32_t last_stream_id,
    uint32_t error_code,
    struct aws_byte_cursor debug_data);

AWS_HTTP_API
struct aws_h2_frame *aws_h2_frame_new_window_update(
    struct aws_allocator *allocator,
    uint32_t stream_id,
    uint32_t window_size_increment);

AWS_HTTP_API void aws_h2_frame_encoder_set_setting_header_table_size(
    struct aws_h2_frame_encoder *encoder,
    uint32_t data);
AWS_HTTP_API void aws_h2_frame_encoder_set_setting_max_frame_size(struct aws_h2_frame_encoder *encoder, uint32_t data);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H2_FRAMES_H */
