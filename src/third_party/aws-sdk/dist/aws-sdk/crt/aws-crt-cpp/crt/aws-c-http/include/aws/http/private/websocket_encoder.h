#ifndef AWS_HTTP_WEBSOCKET_ENCODER_H
#define AWS_HTTP_WEBSOCKET_ENCODER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/websocket_impl.h>

typedef int(aws_websocket_encoder_payload_fn)(struct aws_byte_buf *out_buf, void *user_data);

enum aws_websocket_encoder_state {
    AWS_WEBSOCKET_ENCODER_STATE_INIT,
    AWS_WEBSOCKET_ENCODER_STATE_OPCODE_BYTE,
    AWS_WEBSOCKET_ENCODER_STATE_LENGTH_BYTE,
    AWS_WEBSOCKET_ENCODER_STATE_EXTENDED_LENGTH,
    AWS_WEBSOCKET_ENCODER_STATE_MASKING_KEY_CHECK,
    AWS_WEBSOCKET_ENCODER_STATE_MASKING_KEY,
    AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD_CHECK,
    AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD,
    AWS_WEBSOCKET_ENCODER_STATE_DONE,
};

struct aws_websocket_encoder {
    enum aws_websocket_encoder_state state;
    uint64_t state_bytes_processed;
    struct aws_websocket_frame frame;
    bool is_frame_in_progress;

    /* True when the next data frame must be a CONTINUATION frame */
    bool expecting_continuation_data_frame;

    void *user_data;
    aws_websocket_encoder_payload_fn *stream_outgoing_payload;
};

AWS_EXTERN_C_BEGIN

AWS_HTTP_API
void aws_websocket_encoder_init(
    struct aws_websocket_encoder *encoder,
    aws_websocket_encoder_payload_fn *stream_outgoing_payload,
    void *user_data);

AWS_HTTP_API
int aws_websocket_encoder_start_frame(struct aws_websocket_encoder *encoder, const struct aws_websocket_frame *frame);

AWS_HTTP_API
bool aws_websocket_encoder_is_frame_in_progress(const struct aws_websocket_encoder *encoder);

AWS_HTTP_API
int aws_websocket_encoder_process(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_WEBSOCKET_ENCODER_H */
