#ifndef AWS_HTTP_WEBSOCKET_DECODER_H
#define AWS_HTTP_WEBSOCKET_DECODER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/websocket_impl.h>

/* Called when the non-payload portion of a frame has been decoded. */
typedef int(aws_websocket_decoder_frame_fn)(const struct aws_websocket_frame *frame, void *user_data);

/* Called repeatedly as the payload is decoded. If a mask was used, the data has been unmasked. */
typedef int(aws_websocket_decoder_payload_fn)(struct aws_byte_cursor data, void *user_data);

/**
 * Each state consumes data and/or moves decoder to a subsequent state.
 */
enum aws_websocket_decoder_state {
    AWS_WEBSOCKET_DECODER_STATE_INIT,
    AWS_WEBSOCKET_DECODER_STATE_OPCODE_BYTE,
    AWS_WEBSOCKET_DECODER_STATE_LENGTH_BYTE,
    AWS_WEBSOCKET_DECODER_STATE_EXTENDED_LENGTH,
    AWS_WEBSOCKET_DECODER_STATE_MASKING_KEY_CHECK,
    AWS_WEBSOCKET_DECODER_STATE_MASKING_KEY,
    AWS_WEBSOCKET_DECODER_STATE_PAYLOAD_CHECK,
    AWS_WEBSOCKET_DECODER_STATE_PAYLOAD,
    AWS_WEBSOCKET_DECODER_STATE_FRAME_END,
    AWS_WEBSOCKET_DECODER_STATE_DONE,
};

struct aws_websocket_decoder {
    enum aws_websocket_decoder_state state;
    uint64_t state_bytes_processed; /* For multi-byte states, the number of bytes processed so far */
    uint8_t state_cache[8];         /* For multi-byte states to cache data that might be split across packets */

    struct aws_websocket_frame current_frame; /* Data about current frame being decoded */

    bool expecting_continuation_data_frame; /* True when the next data frame must be CONTINUATION frame */

    /* True while processing a TEXT "message" (from the start of a TEXT frame,
     * until the end of the TEXT or CONTINUATION frame with the FIN bit set). */
    bool processing_text_message;
    struct aws_utf8_decoder *text_message_validator;

    void *user_data;
    aws_websocket_decoder_frame_fn *on_frame;
    aws_websocket_decoder_payload_fn *on_payload;
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
void aws_websocket_decoder_init(
    struct aws_websocket_decoder *decoder,
    struct aws_allocator *alloc,
    aws_websocket_decoder_frame_fn *on_frame,
    aws_websocket_decoder_payload_fn *on_payload,
    void *user_data);

AWS_HTTP_API
void aws_websocket_decoder_clean_up(struct aws_websocket_decoder *decoder);

/**
 * Returns when all data is processed, or a frame and its payload have completed.
 * `data` will be advanced to reflect the amount of data processed by this call.
 * `frame_complete` will be set true if this call returned due to completion of a frame.
 * The `on_frame` and `on_payload` callbacks may each be invoked once as a result of this call.
 * If an error occurs, the decoder is invalid forevermore.
 */
AWS_HTTP_API int aws_websocket_decoder_process(
    struct aws_websocket_decoder *decoder,
    struct aws_byte_cursor *data,
    bool *frame_complete);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_WEBSOCKET_DECODER_H */
