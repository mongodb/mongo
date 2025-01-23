#ifndef AWS_HTTP_H2_DECODER_H
#define AWS_HTTP_H2_DECODER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/h2_frames.h>
#include <aws/http/private/http_impl.h>

/* Decoder design goals:
 * - Minimize state tracking and verification required by user.
 *   For example, we have _begin()/_i()/_end() callbacks when something happens N times.
 *   The _begin() and _end() callbacks tell the user when to transition states.
 *   Without them the user needs to be like, oh, I was doing X but now I'm doing Y,
 *   so I guess I need to end X and start Y.

 * - A callback should result in 1 distinct action.
 *   For example, we have distinct callbacks for `on_ping()` and `on_ping_ack()`.
 *   We COULD have had just one `on_ping(bool ack)` callback, but since user must
 *   take two complete different actions based on the ACK, we opted for two callbacks.
 */

/* Return a failed aws_h2err from any callback to stop the decoder and cause a Connection Error */
struct aws_h2_decoder_vtable {
    /* For HEADERS header-block: _begin() is called, then 0+ _i() calls, then _end().
     * No other decoder callbacks will occur in this time.
     * If something is malformed, no further _i() calls occur, and it is reported in _end() */
    struct aws_h2err (*on_headers_begin)(uint32_t stream_id, void *userdata);
    struct aws_h2err (*on_headers_i)(
        uint32_t stream_id,
        const struct aws_http_header *header,
        enum aws_http_header_name name_enum,
        enum aws_http_header_block block_type,
        void *userdata);
    struct aws_h2err (
        *on_headers_end)(uint32_t stream_id, bool malformed, enum aws_http_header_block block_type, void *userdata);

    /* For PUSH_PROMISE header-block: _begin() is called, then 0+ _i() calls, then _end().
     * No other decoder callbacks will occur in this time.
     * If something is malformed, no further _i() calls occur, and it is reported in _end() */
    struct aws_h2err (*on_push_promise_begin)(uint32_t stream_id, uint32_t promised_stream_id, void *userdata);
    struct aws_h2err (*on_push_promise_i)(
        uint32_t stream_id,
        const struct aws_http_header *header,
        enum aws_http_header_name name_enum,
        void *userdata);
    struct aws_h2err (*on_push_promise_end)(uint32_t stream_id, bool malformed, void *userdata);

    /* For DATA frame: _begin() is called, then 0+ _i() calls, then _end().
     * No other decoder callbacks will occur in this time */
    struct aws_h2err (*on_data_begin)(
        uint32_t stream_id,
        uint32_t payload_len,         /* Whole payload length including padding and padding length */
        uint32_t total_padding_bytes, /* The length of padding and the byte for padding length */
        bool end_stream,
        void *userdata);
    struct aws_h2err (*on_data_i)(uint32_t stream_id, struct aws_byte_cursor data, void *userdata);
    struct aws_h2err (*on_data_end)(uint32_t stream_id, void *userdata);

    /* Called at end of DATA frame containing the END_STREAM flag.
     * OR called at end of header-block which began with HEADERS frame containing the END_STREAM flag */
    struct aws_h2err (*on_end_stream)(uint32_t stream_id, void *userdata);

    /* Called once for RST_STREAM frame */
    struct aws_h2err (*on_rst_stream)(uint32_t stream_id, uint32_t error_code, void *userdata);

    /* Called once For PING frame with ACK flag set */
    struct aws_h2err (*on_ping_ack)(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata);

    /* Called once for PING frame (no ACK flag set)*/
    struct aws_h2err (*on_ping)(uint8_t opaque_data[AWS_HTTP2_PING_DATA_SIZE], void *userdata);

    /* Called once for SETTINGS frame with ACK flag */
    struct aws_h2err (*on_settings_ack)(void *userdata);

    /* Called once for SETTINGS frame, without ACK flag */
    struct aws_h2err (
        *on_settings)(const struct aws_http2_setting *settings_array, size_t num_settings, void *userdata);

    /* Called once for GOAWAY frame */
    struct aws_h2err (
        *on_goaway)(uint32_t last_stream, uint32_t error_code, struct aws_byte_cursor debug_data, void *userdata);

    /* Called once for WINDOW_UPDATE frame */
    struct aws_h2err (*on_window_update)(uint32_t stream_id, uint32_t window_size_increment, void *userdata);
};

/**
 * Structure used to initialize an `aws_h2_decoder`.
 */
struct aws_h2_decoder_params {
    struct aws_allocator *alloc;
    const struct aws_h2_decoder_vtable *vtable;
    void *userdata;
    const void *logging_id;
    bool is_server;

    /* If true, do not expect the connection preface and immediately accept any frame type.
     * Only set this when testing the decoder itself */
    bool skip_connection_preface;
};

struct aws_h2_decoder;

AWS_EXTERN_C_BEGIN

AWS_HTTP_API struct aws_h2_decoder *aws_h2_decoder_new(struct aws_h2_decoder_params *params);
AWS_HTTP_API void aws_h2_decoder_destroy(struct aws_h2_decoder *decoder);

/* If failed aws_h2err returned, it is a Connection Error */
AWS_HTTP_API struct aws_h2err aws_h2_decode(struct aws_h2_decoder *decoder, struct aws_byte_cursor *data);

AWS_HTTP_API void aws_h2_decoder_set_setting_header_table_size(struct aws_h2_decoder *decoder, uint32_t data);
AWS_HTTP_API void aws_h2_decoder_set_setting_enable_push(struct aws_h2_decoder *decoder, uint32_t data);
AWS_HTTP_API void aws_h2_decoder_set_setting_max_frame_size(struct aws_h2_decoder *decoder, uint32_t data);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H2_DECODER_H */
