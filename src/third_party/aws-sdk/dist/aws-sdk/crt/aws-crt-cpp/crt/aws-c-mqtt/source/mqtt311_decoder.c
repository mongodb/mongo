/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/mqtt311_decoder.h>

#include <aws/mqtt/private/fixed_header.h>

static void s_aws_mqtt311_decoder_reset(struct aws_mqtt311_decoder *decoder) {
    decoder->state = AWS_MDST_READ_FIRST_BYTE;
    decoder->total_packet_length = 0;
    aws_byte_buf_reset(&decoder->packet_buffer, false);
}

void aws_mqtt311_decoder_init(
    struct aws_mqtt311_decoder *decoder,
    struct aws_allocator *allocator,
    const struct aws_mqtt311_decoder_options *options) {

    aws_byte_buf_init(&decoder->packet_buffer, allocator, 5);
    decoder->config = *options;

    s_aws_mqtt311_decoder_reset(decoder);
}

void aws_mqtt311_decoder_clean_up(struct aws_mqtt311_decoder *decoder) {
    aws_byte_buf_clean_up(&decoder->packet_buffer);
}

static void s_aws_mqtt311_decoder_reset_for_new_packet(struct aws_mqtt311_decoder *decoder) {
    if (decoder->state != AWS_MDST_PROTOCOL_ERROR) {
        s_aws_mqtt311_decoder_reset(decoder);
    }
}

enum aws_mqtt311_decoding_directive { AWS_MDD_CONTINUE, AWS_MDD_OUT_OF_DATA, AWS_MDD_PROTOCOL_ERROR };

static enum aws_mqtt311_decoding_directive aws_result_to_mqtt311_decoding_directive(int result) {
    return (result == AWS_OP_SUCCESS) ? AWS_MDD_CONTINUE : AWS_MDD_PROTOCOL_ERROR;
}

static int s_aws_mqtt311_decoder_safe_packet_handle(
    struct aws_mqtt311_decoder *decoder,
    enum aws_mqtt_packet_type packet_type,
    struct aws_byte_cursor packet_cursor) {
    packet_handler_fn *handler = decoder->config.packet_handlers->handlers_by_packet_type[packet_type];
    if (handler != NULL) {
        return handler(packet_cursor, decoder->config.handler_user_data);
    } else {
        return aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
    }
}

static enum aws_mqtt311_decoding_directive s_handle_decoder_read_first_byte(
    struct aws_mqtt311_decoder *decoder,
    struct aws_byte_cursor *data) {
    AWS_FATAL_ASSERT(decoder->packet_buffer.len == 0);
    if (data->len == 0) {
        return AWS_MDD_OUT_OF_DATA;
    }

    /*
     * Do a greedy check to see if the whole MQTT packet is contained within the received data.  If it is, decode it
     * directly from the incoming data cursor without buffering it first.  Otherwise, the packet is fragmented
     * across multiple received data calls, and so we must use the packet buffer and copy everything first via the
     * full decoder state machine.
     *
     * A corollary of this is that the decoder is only ever in the AWS_MDST_READ_REMAINING_LENGTH or AWS_MDST_READ_BODY
     * states if the current MQTT packet was received in a fragmented manner.
     */
    struct aws_byte_cursor temp_cursor = *data;
    struct aws_mqtt_fixed_header packet_header;
    AWS_ZERO_STRUCT(packet_header);
    if (!aws_mqtt_fixed_header_decode(&temp_cursor, &packet_header) &&
        temp_cursor.len >= packet_header.remaining_length) {

        /* figure out the cursor that spans the full packet */
        size_t fixed_header_length = temp_cursor.ptr - data->ptr;
        struct aws_byte_cursor whole_packet_cursor = *data;
        whole_packet_cursor.len = fixed_header_length + packet_header.remaining_length;

        /* advance the external, mutable data cursor to the start of the next packet */
        aws_byte_cursor_advance(data, whole_packet_cursor.len);

        /*
         * if this fails, the decoder goes into an error state.  If it succeeds we'll loop again into the same state
         * because we'll be back at the beginning of the next packet (if it exists).
         */
        enum aws_mqtt_packet_type packet_type = aws_mqtt_get_packet_type(whole_packet_cursor.ptr);
        return aws_result_to_mqtt311_decoding_directive(
            s_aws_mqtt311_decoder_safe_packet_handle(decoder, packet_type, whole_packet_cursor));
    }

    /*
     * The packet is fragmented, spanning more than this io message.  So we buffer it and use the
     * simple state machine to decode.
     */
    uint8_t byte = *data->ptr;
    aws_byte_cursor_advance(data, 1);
    aws_byte_buf_append_byte_dynamic(&decoder->packet_buffer, byte);

    decoder->state = AWS_MDST_READ_REMAINING_LENGTH;

    return AWS_MDD_CONTINUE;
}

static enum aws_mqtt311_decoding_directive s_handle_decoder_read_remaining_length(
    struct aws_mqtt311_decoder *decoder,
    struct aws_byte_cursor *data) {
    AWS_FATAL_ASSERT(decoder->total_packet_length == 0);
    if (data->len == 0) {
        return AWS_MDD_OUT_OF_DATA;
    }

    uint8_t byte = *data->ptr;
    aws_byte_cursor_advance(data, 1);
    aws_byte_buf_append_byte_dynamic(&decoder->packet_buffer, byte);

    struct aws_byte_cursor vli_cursor = aws_byte_cursor_from_buf(&decoder->packet_buffer);
    aws_byte_cursor_advance(&vli_cursor, 1);

    size_t remaining_length = 0;
    if (aws_mqtt311_decode_remaining_length(&vli_cursor, &remaining_length) == AWS_OP_ERR) {
        /* anything other than a short buffer error (not enough data yet) is a terminal error */
        if (aws_last_error() == AWS_ERROR_SHORT_BUFFER) {
            return AWS_MDD_CONTINUE;
        } else {
            return AWS_MDD_PROTOCOL_ERROR;
        }
    }

    /*
     * If we successfully decoded a variable-length integer, we now know exactly how many bytes we need to receive in
     * order to have the full packet.
     */
    decoder->total_packet_length = remaining_length + decoder->packet_buffer.len;
    AWS_FATAL_ASSERT(decoder->total_packet_length > 0);
    decoder->state = AWS_MDST_READ_BODY;

    return AWS_MDD_CONTINUE;
}

static enum aws_mqtt311_decoding_directive s_handle_decoder_read_body(
    struct aws_mqtt311_decoder *decoder,
    struct aws_byte_cursor *data) {
    AWS_FATAL_ASSERT(decoder->total_packet_length > 0);

    size_t buffer_length = decoder->packet_buffer.len;
    size_t amount_to_read = aws_min_size(decoder->total_packet_length - buffer_length, data->len);

    struct aws_byte_cursor copy_cursor = aws_byte_cursor_advance(data, amount_to_read);
    aws_byte_buf_append_dynamic(&decoder->packet_buffer, &copy_cursor);

    if (decoder->packet_buffer.len == decoder->total_packet_length) {

        /* We have the full packet in the scratch buffer, invoke the correct handler to decode and process it */
        struct aws_byte_cursor packet_data = aws_byte_cursor_from_buf(&decoder->packet_buffer);
        enum aws_mqtt_packet_type packet_type = aws_mqtt_get_packet_type(packet_data.ptr);
        if (s_aws_mqtt311_decoder_safe_packet_handle(decoder, packet_type, packet_data) == AWS_OP_ERR) {
            return AWS_MDD_PROTOCOL_ERROR;
        }

        s_aws_mqtt311_decoder_reset_for_new_packet(decoder);
        return AWS_MDD_CONTINUE;
    }

    return AWS_MDD_OUT_OF_DATA;
}

int aws_mqtt311_decoder_on_bytes_received(struct aws_mqtt311_decoder *decoder, struct aws_byte_cursor data) {
    struct aws_byte_cursor data_cursor = data;

    enum aws_mqtt311_decoding_directive decode_directive = AWS_MDD_CONTINUE;
    while (decode_directive == AWS_MDD_CONTINUE) {
        switch (decoder->state) {
            case AWS_MDST_READ_FIRST_BYTE:
                decode_directive = s_handle_decoder_read_first_byte(decoder, &data_cursor);
                break;

            case AWS_MDST_READ_REMAINING_LENGTH:
                decode_directive = s_handle_decoder_read_remaining_length(decoder, &data_cursor);
                break;

            case AWS_MDST_READ_BODY:
                decode_directive = s_handle_decoder_read_body(decoder, &data_cursor);
                break;

            default:
                decode_directive = AWS_MDD_PROTOCOL_ERROR;
                break;
        }

        /*
         * Protocol error is a terminal failure state until aws_mqtt311_decoder_reset_for_new_connection() is called.
         */
        if (decode_directive == AWS_MDD_PROTOCOL_ERROR) {
            decoder->state = AWS_MDST_PROTOCOL_ERROR;
            if (aws_last_error() == AWS_ERROR_SUCCESS) {
                aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
            }
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt311_decoder_reset_for_new_connection(struct aws_mqtt311_decoder *decoder) {
    s_aws_mqtt311_decoder_reset(decoder);
}
