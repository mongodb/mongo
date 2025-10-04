/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/websocket_encoder.h>

#include <inttypes.h>

typedef int(state_fn)(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf);

/* STATE_INIT: Outputs no data */
static int s_state_init(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    (void)out_buf;

    if (!encoder->is_frame_in_progress) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    encoder->state = AWS_WEBSOCKET_ENCODER_STATE_OPCODE_BYTE;
    return AWS_OP_SUCCESS;
}

/* STATE_OPCODE_BYTE: Outputs 1st byte of frame, which is packed with goodies. */
static int s_state_opcode_byte(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {

    AWS_ASSERT((encoder->frame.opcode & 0xF0) == 0); /* Should be impossible, the opcode was checked in start_frame() */

    /* Right 4 bits are opcode, left 4 bits are fin|rsv1|rsv2|rsv3 */
    uint8_t byte = encoder->frame.opcode;
    byte |= (encoder->frame.fin << 7);
    byte |= (encoder->frame.rsv[0] << 6);
    byte |= (encoder->frame.rsv[1] << 5);
    byte |= (encoder->frame.rsv[2] << 4);

    /* If buffer has room to write, proceed to next state */
    if (aws_byte_buf_write_u8(out_buf, byte)) {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_LENGTH_BYTE;
    }

    return AWS_OP_SUCCESS;
}

/* STATE_LENGTH_BYTE: Output 2nd byte of frame, which indicates payload length */
static int s_state_length_byte(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    /* First bit is masking bool */
    uint8_t byte = (uint8_t)(encoder->frame.masked << 7);

    /* Next 7bits are length, if length is small.
     * Otherwise next 7bits are a magic number indicating how many bytes will be required to encode actual length */
    bool extended_length_required;

    if (encoder->frame.payload_length < AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MIN_VALUE) {
        byte |= (uint8_t)encoder->frame.payload_length;
        extended_length_required = false;
    } else if (encoder->frame.payload_length <= AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MAX_VALUE) {
        byte |= AWS_WEBSOCKET_7BIT_VALUE_FOR_2BYTE_EXTENDED_LENGTH;
        extended_length_required = true;
    } else {
        AWS_ASSERT(encoder->frame.payload_length <= AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MAX_VALUE);
        byte |= AWS_WEBSOCKET_7BIT_VALUE_FOR_8BYTE_EXTENDED_LENGTH;
        extended_length_required = true;
    }

    /* If buffer has room to write, proceed to next appropriate state */
    if (aws_byte_buf_write_u8(out_buf, byte)) {
        if (extended_length_required) {
            encoder->state = AWS_WEBSOCKET_ENCODER_STATE_EXTENDED_LENGTH;
            encoder->state_bytes_processed = 0;
        } else {
            encoder->state = AWS_WEBSOCKET_ENCODER_STATE_MASKING_KEY_CHECK;
        }
    }

    return AWS_OP_SUCCESS;
}

/* STATE_EXTENDED_LENGTH: Output extended length (state skipped if not using extended length). */
static int s_state_extended_length(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    /* Fill tmp buffer with extended-length in network byte order */
    uint8_t network_bytes_array[8] = {0};
    struct aws_byte_buf network_bytes_buf =
        aws_byte_buf_from_empty_array(network_bytes_array, sizeof(network_bytes_array));
    if (encoder->frame.payload_length <= AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MAX_VALUE) {
        aws_byte_buf_write_be16(&network_bytes_buf, (uint16_t)encoder->frame.payload_length);
    } else {
        aws_byte_buf_write_be64(&network_bytes_buf, encoder->frame.payload_length);
    }

    /* Use cursor to iterate over tmp buffer */
    struct aws_byte_cursor network_bytes_cursor = aws_byte_cursor_from_buf(&network_bytes_buf);

    /* Advance cursor if some bytes already written */
    aws_byte_cursor_advance(&network_bytes_cursor, (size_t)encoder->state_bytes_processed);

    /* Shorten cursor if it won't all fit in out_buf */
    bool all_data_written = true;
    size_t space_available = out_buf->capacity - out_buf->len;
    if (network_bytes_cursor.len > space_available) {
        network_bytes_cursor.len = space_available;
        all_data_written = false;
    }

    aws_byte_buf_write_from_whole_cursor(out_buf, network_bytes_cursor);
    encoder->state_bytes_processed += network_bytes_cursor.len;

    /* If all bytes written, advance to next state */
    if (all_data_written) {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_MASKING_KEY_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* MASKING_KEY_CHECK: Outputs no data. Gets things ready for (or decides to skip) the STATE_MASKING_KEY */
static int s_state_masking_key_check(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    (void)out_buf;

    if (encoder->frame.masked) {
        encoder->state_bytes_processed = 0;
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_MASKING_KEY;
    } else {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* MASKING_KEY: Output masking-key (state skipped if no masking key). */
static int s_state_masking_key(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    /* Prepare cursor to iterate over masking-key bytes */
    struct aws_byte_cursor cursor =
        aws_byte_cursor_from_array(encoder->frame.masking_key, sizeof(encoder->frame.masking_key));

    /* Advance cursor if some bytes already written (moves ptr forward but shortens len so end stays in place) */
    aws_byte_cursor_advance(&cursor, (size_t)encoder->state_bytes_processed);

    /* Shorten cursor if it won't all fit in out_buf */
    bool all_data_written = true;
    size_t space_available = out_buf->capacity - out_buf->len;
    if (cursor.len > space_available) {
        cursor.len = space_available;
        all_data_written = false;
    }

    aws_byte_buf_write_from_whole_cursor(out_buf, cursor);
    encoder->state_bytes_processed += cursor.len;

    /* If all bytes written, advance to next state */
    if (all_data_written) {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* MASKING_KEY_CHECK: Outputs no data. Gets things ready for (or decides to skip) STATE_PAYLOAD */
static int s_state_payload_check(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {
    (void)out_buf;

    if (encoder->frame.payload_length > 0) {
        encoder->state_bytes_processed = 0;
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD;
    } else {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_DONE;
    }

    return AWS_OP_SUCCESS;
}

/* PAYLOAD: Output payload until we're done (state skipped if no payload). */
static int s_state_payload(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {

    /* Bail early if out_buf has no space for writing */
    if (out_buf->len >= out_buf->capacity) {
        return AWS_OP_SUCCESS;
    }

    const uint64_t prev_bytes_processed = encoder->state_bytes_processed;
    const struct aws_byte_buf prev_buf = *out_buf;

    /* Invoke callback which will write to buffer */
    int err = encoder->stream_outgoing_payload(out_buf, encoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    /* Ensure that user did not commit forbidden acts upon the out_buf */
    AWS_FATAL_ASSERT(
        (out_buf->buffer == prev_buf.buffer) && (out_buf->capacity == prev_buf.capacity) &&
        (out_buf->len >= prev_buf.len));

    size_t bytes_written = out_buf->len - prev_buf.len;

    err = aws_add_u64_checked(encoder->state_bytes_processed, bytes_written, &encoder->state_bytes_processed);
    if (err) {
        return aws_raise_error(AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT);
    }

    /* Mask data, if necessary.
     * RFC-6455 Section 5.3 Client-to-Server Masking
     * Each byte of payload is XOR against a byte of the masking-key */
    if (encoder->frame.masked) {
        uint64_t mask_index = prev_bytes_processed;

        /* Optimization idea: don't do this 1 byte at a time */
        uint8_t *current_byte = out_buf->buffer + prev_buf.len;
        uint8_t *end_byte = out_buf->buffer + out_buf->len;
        while (current_byte != end_byte) {
            *current_byte++ ^= encoder->frame.masking_key[mask_index++ % 4];
        }
    }

    /* If done writing payload, proceed to next state */
    if (encoder->state_bytes_processed == encoder->frame.payload_length) {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_DONE;
    } else {
        /* Some more error-checking... */
        if (encoder->state_bytes_processed > encoder->frame.payload_length) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Outgoing stream has exceeded stated payload length of %" PRIu64,
                (void *)encoder->user_data,
                encoder->frame.payload_length);
            return aws_raise_error(AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT);
        }
    }

    return AWS_OP_SUCCESS;
}

static state_fn *s_state_functions[AWS_WEBSOCKET_ENCODER_STATE_DONE] = {
    s_state_init,
    s_state_opcode_byte,
    s_state_length_byte,
    s_state_extended_length,
    s_state_masking_key_check,
    s_state_masking_key,
    s_state_payload_check,
    s_state_payload,
};

int aws_websocket_encoder_process(struct aws_websocket_encoder *encoder, struct aws_byte_buf *out_buf) {

    /* Run state machine until frame is completely decoded, or the state stops changing.
     * Note that we don't necessarily stop looping when out_buf is full, because not all states need to output data */
    while (encoder->state != AWS_WEBSOCKET_ENCODER_STATE_DONE) {
        const enum aws_websocket_encoder_state prev_state = encoder->state;

        int err = s_state_functions[encoder->state](encoder, out_buf);
        if (err) {
            return AWS_OP_ERR;
        }

        if (prev_state == encoder->state) {
            /* dev-assert: Check that each state is doing as much work as it possibly can.
             * Except for the PAYLOAD state, where it's up to the user to fill the buffer. */
            AWS_ASSERT((out_buf->len == out_buf->capacity) || (encoder->state == AWS_WEBSOCKET_ENCODER_STATE_PAYLOAD));

            break;
        }
    }

    if (encoder->state == AWS_WEBSOCKET_ENCODER_STATE_DONE) {
        encoder->state = AWS_WEBSOCKET_ENCODER_STATE_INIT;
        encoder->is_frame_in_progress = false;
    }

    return AWS_OP_SUCCESS;
}

int aws_websocket_encoder_start_frame(struct aws_websocket_encoder *encoder, const struct aws_websocket_frame *frame) {
    /* Error-check as much as possible before accepting next frame */
    if (encoder->is_frame_in_progress) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    /* RFC-6455 Section 5.2 contains all these rules... */

    /* Opcode must fit in 4bits */
    if (frame->opcode != (frame->opcode & 0x0F)) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET,
            "id=%p: Outgoing frame has unknown opcode 0x%" PRIx8,
            (void *)encoder->user_data,
            frame->opcode);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* High bit of 8byte length must be clear */
    if (frame->payload_length > AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MAX_VALUE) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_WEBSOCKET,
            "id=%p: Outgoing frame's payload length exceeds the max",
            (void *)encoder->user_data);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* Data frames with the FIN bit clear are considered fragmented and must be followed by
     * 1+ CONTINUATION frames, where only the final CONTINUATION frame's FIN bit is set.
     *
     * Control frames may be injected in the middle of a fragmented message,
     * but control frames may not be fragmented themselves. */
    bool keep_expecting_continuation_data_frame = encoder->expecting_continuation_data_frame;
    if (aws_websocket_is_data_frame(frame->opcode)) {
        bool is_continuation_frame = (AWS_WEBSOCKET_OPCODE_CONTINUATION == frame->opcode);

        if (encoder->expecting_continuation_data_frame != is_continuation_frame) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Fragmentation error. Outgoing frame starts a new message but previous message has not ended",
                (void *)encoder->user_data);
            return aws_raise_error(AWS_ERROR_INVALID_STATE);
        }

        keep_expecting_continuation_data_frame = !frame->fin;
    } else {
        /* Control frames themselves MUST NOT be fragmented. */
        if (!frame->fin) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: It is illegal to send a fragmented control frame",
                (void *)encoder->user_data);

            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }
    }

    /* Frame accepted */
    encoder->frame = *frame;
    encoder->is_frame_in_progress = true;
    encoder->expecting_continuation_data_frame = keep_expecting_continuation_data_frame;

    return AWS_OP_SUCCESS;
}

bool aws_websocket_encoder_is_frame_in_progress(const struct aws_websocket_encoder *encoder) {
    return encoder->is_frame_in_progress;
}

void aws_websocket_encoder_init(
    struct aws_websocket_encoder *encoder,
    aws_websocket_encoder_payload_fn *stream_outgoing_payload,
    void *user_data) {

    AWS_ZERO_STRUCT(*encoder);
    encoder->user_data = user_data;
    encoder->stream_outgoing_payload = stream_outgoing_payload;
}

uint64_t aws_websocket_frame_encoded_size(const struct aws_websocket_frame *frame) {
    /* This is an internal function, so asserts are sufficient error handling */
    AWS_ASSERT(frame);
    AWS_ASSERT(frame->payload_length <= AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MAX_VALUE);

    /* All frames start with at least 2 bytes */
    uint64_t total = 2;

    /* If masked, add 4 bytes for masking-key */
    if (frame->masked) {
        total += 4;
    }

    /* If extended payload length, add 2 or 8 bytes */
    if (frame->payload_length >= AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MIN_VALUE) {
        total += 8;
    } else if (frame->payload_length >= AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MIN_VALUE) {
        total += 2;
    }

    /* Plus payload itself */
    total += frame->payload_length;

    return total;
}
