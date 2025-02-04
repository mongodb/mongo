/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/websocket_decoder.h>

#include <aws/common/encoding.h>

#include <inttypes.h>

typedef int(state_fn)(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data);

/* STATE_INIT: Resets things, consumes no data */
static int s_state_init(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    (void)data;
    AWS_ZERO_STRUCT(decoder->current_frame);
    decoder->state = AWS_WEBSOCKET_DECODER_STATE_OPCODE_BYTE;
    return AWS_OP_SUCCESS;
}

/* STATE_OPCODE_BYTE: Decode first byte of frame, which has all kinds of goodies in it. */
static int s_state_opcode_byte(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    uint8_t byte = data->ptr[0];
    aws_byte_cursor_advance(data, 1);

    /* first 4 bits are all bools */
    decoder->current_frame.fin = byte & 0x80;
    decoder->current_frame.rsv[0] = byte & 0x40;
    decoder->current_frame.rsv[1] = byte & 0x20;
    decoder->current_frame.rsv[2] = byte & 0x10;

    /* next 4 bits are opcode */
    decoder->current_frame.opcode = byte & 0x0F;

    /* RFC-6455 Section 5.2 - Opcode
     * If an unknown opcode is received, the receiving endpoint MUST _Fail the WebSocket Connection_. */
    switch (decoder->current_frame.opcode) {
        case AWS_WEBSOCKET_OPCODE_CONTINUATION:
        case AWS_WEBSOCKET_OPCODE_TEXT:
        case AWS_WEBSOCKET_OPCODE_BINARY:
        case AWS_WEBSOCKET_OPCODE_CLOSE:
        case AWS_WEBSOCKET_OPCODE_PING:
        case AWS_WEBSOCKET_OPCODE_PONG:
            break;
        default:
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Received frame with unknown opcode 0x%" PRIx8,
                (void *)decoder->user_data,
                decoder->current_frame.opcode);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
    }

    /* RFC-6455 Section 5.2 Fragmentation
     *
     * Data frames with the FIN bit clear are considered fragmented and must be followed by
     * 1+ CONTINUATION frames, where only the final CONTINUATION frame's FIN bit is set.
     *
     * Control frames may be injected in the middle of a fragmented message,
     * but control frames may not be fragmented themselves.
     */
    if (aws_websocket_is_data_frame(decoder->current_frame.opcode)) {
        bool is_continuation_frame = AWS_WEBSOCKET_OPCODE_CONTINUATION == decoder->current_frame.opcode;

        if (decoder->expecting_continuation_data_frame != is_continuation_frame) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Fragmentation error. Received start of new message before end of previous message",
                (void *)decoder->user_data);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
        }

        decoder->expecting_continuation_data_frame = !decoder->current_frame.fin;

    } else {
        /* Control frames themselves MUST NOT be fragmented. */
        if (!decoder->current_frame.fin) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Received fragmented control frame. This is illegal",
                (void *)decoder->user_data);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
        }
    }

    if (decoder->current_frame.opcode == AWS_WEBSOCKET_OPCODE_TEXT) {
        decoder->processing_text_message = true;
    }

    decoder->state = AWS_WEBSOCKET_DECODER_STATE_LENGTH_BYTE;
    return AWS_OP_SUCCESS;
}

/* STATE_LENGTH_BYTE: Decode byte containing length, determine if we need to decode extended length. */
static int s_state_length_byte(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    uint8_t byte = data->ptr[0];
    aws_byte_cursor_advance(data, 1);

    /* first bit is a bool */
    decoder->current_frame.masked = byte & 0x80;

    /* remaining 7 bits are payload length */
    decoder->current_frame.payload_length = byte & 0x7F;

    if (decoder->current_frame.payload_length >= AWS_WEBSOCKET_7BIT_VALUE_FOR_2BYTE_EXTENDED_LENGTH) {
        /* If 7bit payload length has a high value, then the next few bytes contain the real payload length */
        decoder->state_bytes_processed = 0;
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_EXTENDED_LENGTH;
    } else {
        /* If 7bit payload length has low value, that's the actual payload size, jump past EXTENDED_LENGTH state */
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_MASKING_KEY_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* STATE_EXTENDED_LENGTH: Decode extended length (state skipped if no extended length). */
static int s_state_extended_length(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    /* The 7bit payload value loaded during the previous state indicated that
     * actual payload length is encoded across the next 2 or 8 bytes. */
    uint8_t total_bytes_extended_length;
    uint64_t min_acceptable_value;
    uint64_t max_acceptable_value;
    if (decoder->current_frame.payload_length == AWS_WEBSOCKET_7BIT_VALUE_FOR_2BYTE_EXTENDED_LENGTH) {
        total_bytes_extended_length = 2;
        min_acceptable_value = AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MIN_VALUE;
        max_acceptable_value = AWS_WEBSOCKET_2BYTE_EXTENDED_LENGTH_MAX_VALUE;
    } else {
        AWS_ASSERT(decoder->current_frame.payload_length == AWS_WEBSOCKET_7BIT_VALUE_FOR_8BYTE_EXTENDED_LENGTH);

        total_bytes_extended_length = 8;
        min_acceptable_value = AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MIN_VALUE;
        max_acceptable_value = AWS_WEBSOCKET_8BYTE_EXTENDED_LENGTH_MAX_VALUE;
    }

    /* Copy bytes of extended-length to state_cache, we'll process them later.*/
    AWS_ASSERT(total_bytes_extended_length > decoder->state_bytes_processed);

    size_t remaining_bytes = (size_t)(total_bytes_extended_length - decoder->state_bytes_processed);
    size_t bytes_to_consume = remaining_bytes <= data->len ? remaining_bytes : data->len;

    AWS_ASSERT(bytes_to_consume + decoder->state_bytes_processed <= sizeof(decoder->state_cache));

    memcpy(decoder->state_cache + decoder->state_bytes_processed, data->ptr, bytes_to_consume);

    aws_byte_cursor_advance(data, bytes_to_consume);
    decoder->state_bytes_processed += bytes_to_consume;

    /* Return, still waiting on more bytes */
    if (decoder->state_bytes_processed < total_bytes_extended_length) {
        return AWS_OP_SUCCESS;
    }

    /* All bytes have been copied into state_cache, now read them together as one number,
     * transforming from network byte order (big endian) to native endianness. */
    struct aws_byte_cursor cache_cursor = aws_byte_cursor_from_array(decoder->state_cache, total_bytes_extended_length);
    if (total_bytes_extended_length == 2) {
        uint16_t val;
        aws_byte_cursor_read_be16(&cache_cursor, &val);
        decoder->current_frame.payload_length = val;
    } else {
        aws_byte_cursor_read_be64(&cache_cursor, &decoder->current_frame.payload_length);
    }

    if (decoder->current_frame.payload_length < min_acceptable_value ||
        decoder->current_frame.payload_length > max_acceptable_value) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_WEBSOCKET, "id=%p: Failed to decode payload length", (void *)decoder->user_data);
        return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
    }

    decoder->state = AWS_WEBSOCKET_DECODER_STATE_MASKING_KEY_CHECK;
    return AWS_OP_SUCCESS;
}

/* MASKING_KEY_CHECK: Determine if we need to decode masking-key. Consumes no data. */
static int s_state_masking_key_check(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    (void)data;

    /* If mask bit was set, move to next state to process 4 bytes of masking key.
     * Otherwise skip next step, there is no masking key. */
    if (decoder->current_frame.masked) {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_MASKING_KEY;
        decoder->state_bytes_processed = 0;
    } else {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_PAYLOAD_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* MASKING_KEY: Decode masking-key (state skipped if no masking key). */
static int s_state_masking_key(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    AWS_ASSERT(4 > decoder->state_bytes_processed);
    size_t bytes_remaining = 4 - (size_t)decoder->state_bytes_processed;
    size_t bytes_to_consume = bytes_remaining < data->len ? bytes_remaining : data->len;

    memcpy(decoder->current_frame.masking_key + decoder->state_bytes_processed, data->ptr, bytes_to_consume);

    aws_byte_cursor_advance(data, bytes_to_consume);
    decoder->state_bytes_processed += bytes_to_consume;

    /* If all bytes consumed, proceed to next state */
    if (decoder->state_bytes_processed == 4) {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_PAYLOAD_CHECK;
    }

    return AWS_OP_SUCCESS;
}

/* PAYLOAD_CHECK: Determine if we need to decode a payload. Consumes no data. */
static int s_state_payload_check(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    (void)data;

    /* Invoke on_frame() callback to inform user of non-payload data. */
    int err = decoder->on_frame(&decoder->current_frame, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    /* Choose next state: either we have payload to process or we don't. */
    if (decoder->current_frame.payload_length > 0) {
        decoder->state_bytes_processed = 0;
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_PAYLOAD;
    } else {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_FRAME_END;
    }

    return AWS_OP_SUCCESS;
}

/* PAYLOAD: Decode payload until we're done (state skipped if no payload). */
static int s_state_payload(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    if (data->len == 0) {
        return AWS_OP_SUCCESS;
    }

    AWS_ASSERT(decoder->current_frame.payload_length > decoder->state_bytes_processed);
    uint64_t bytes_remaining = decoder->current_frame.payload_length - decoder->state_bytes_processed;
    size_t bytes_to_consume = bytes_remaining < data->len ? (size_t)bytes_remaining : data->len;

    struct aws_byte_cursor payload = aws_byte_cursor_advance(data, bytes_to_consume);

    /* Unmask data, if necessary.
     * RFC-6455 Section 5.3 Client-to-Server Masking
     * Each byte of payload is XOR against a byte of the masking-key */
    if (decoder->current_frame.masked) {
        uint64_t mask_index = decoder->state_bytes_processed;

        /* Optimization idea: don't do this 1 byte at a time */
        uint8_t *current_byte = payload.ptr;
        uint8_t *end_byte = payload.ptr + payload.len;
        while (current_byte != end_byte) {
            *current_byte++ ^= decoder->current_frame.masking_key[mask_index++ % 4];
        }
    }

    /* TODO: validate payload of CLOSE frame */

    /* Validate the UTF-8 for TEXT messages (a TEXT frame and any subsequent CONTINUATION frames) */
    if (decoder->processing_text_message && aws_websocket_is_data_frame(decoder->current_frame.opcode)) {
        if (aws_utf8_decoder_update(decoder->text_message_validator, payload)) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_WEBSOCKET, "id=%p: Received invalid UTF-8", (void *)decoder->user_data);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
        }
    }

    /* Invoke on_payload() callback to inform user of payload data */
    int err = decoder->on_payload(payload, decoder->user_data);
    if (err) {
        return AWS_OP_ERR;
    }

    decoder->state_bytes_processed += payload.len;
    AWS_ASSERT(decoder->state_bytes_processed <= decoder->current_frame.payload_length);

    /* If all data consumed, proceed to next state. */
    if (decoder->state_bytes_processed == decoder->current_frame.payload_length) {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_FRAME_END;
    }

    return AWS_OP_SUCCESS;
}

/* FRAME_END: Perform checks once we reach the end of the frame. */
static int s_state_frame_end(struct aws_websocket_decoder *decoder, struct aws_byte_cursor *data) {
    (void)data;

    /* If we're done processing a text message (a TEXT frame and any subsequent CONTINUATION frames),
     * complete the UTF-8 validation */
    if (decoder->processing_text_message && aws_websocket_is_data_frame(decoder->current_frame.opcode) &&
        decoder->current_frame.fin) {

        if (aws_utf8_decoder_finalize(decoder->text_message_validator)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_WEBSOCKET,
                "id=%p: Received invalid UTF-8 (incomplete encoding)",
                (void *)decoder->user_data);
            return aws_raise_error(AWS_ERROR_HTTP_WEBSOCKET_PROTOCOL_ERROR);
        }

        decoder->processing_text_message = false;
    }

    /* Done! */
    decoder->state = AWS_WEBSOCKET_DECODER_STATE_DONE;
    return AWS_OP_SUCCESS;
}

static state_fn *s_state_functions[AWS_WEBSOCKET_DECODER_STATE_DONE] = {
    s_state_init,
    s_state_opcode_byte,
    s_state_length_byte,
    s_state_extended_length,
    s_state_masking_key_check,
    s_state_masking_key,
    s_state_payload_check,
    s_state_payload,
    s_state_frame_end,
};

int aws_websocket_decoder_process(
    struct aws_websocket_decoder *decoder,
    struct aws_byte_cursor *data,
    bool *frame_complete) {

    /* Run state machine until frame is completely decoded, or the state stops changing.
     * Note that we don't stop looping when data->len reaches zero, because some states consume no data. */
    while (decoder->state != AWS_WEBSOCKET_DECODER_STATE_DONE) {
        enum aws_websocket_decoder_state prev_state = decoder->state;

        int err = s_state_functions[decoder->state](decoder, data);
        if (err) {
            return AWS_OP_ERR;
        }

        if (decoder->state == prev_state) {
            AWS_ASSERT(data->len == 0); /* If no more work to do, all possible data should have been consumed */
            break;
        }
    }

    if (decoder->state == AWS_WEBSOCKET_DECODER_STATE_DONE) {
        decoder->state = AWS_WEBSOCKET_DECODER_STATE_INIT;
        *frame_complete = true;
        return AWS_OP_SUCCESS;
    }

    *frame_complete = false;
    return AWS_OP_SUCCESS;
}

void aws_websocket_decoder_init(
    struct aws_websocket_decoder *decoder,
    struct aws_allocator *alloc,
    aws_websocket_decoder_frame_fn *on_frame,
    aws_websocket_decoder_payload_fn *on_payload,
    void *user_data) {

    AWS_ZERO_STRUCT(*decoder);
    decoder->user_data = user_data;
    decoder->on_frame = on_frame;
    decoder->on_payload = on_payload;
    decoder->text_message_validator = aws_utf8_decoder_new(alloc, NULL /*options*/);
}

void aws_websocket_decoder_clean_up(struct aws_websocket_decoder *decoder) {
    aws_utf8_decoder_destroy(decoder->text_message_validator);
    AWS_ZERO_STRUCT(*decoder);
}
