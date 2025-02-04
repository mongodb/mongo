/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/fixed_header.h>

/**
 * Implements encoding & decoding of the remaining_length field across 1-4 bytes [MQTT-2.2.3].
 *
 * Any number less than or equal to 127 (7 bit max) can be written into a single byte, where any number larger than 128
 * may be written into multiple bytes, using the most significant bit (128) as a continuation flag.
 */
static int s_encode_remaining_length(struct aws_byte_buf *buf, size_t remaining_length) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(remaining_length < UINT32_MAX);

    do {
        uint8_t encoded_byte = remaining_length % 128;
        remaining_length /= 128;
        if (remaining_length) {
            encoded_byte |= 128;
        }
        if (!aws_byte_buf_write_u8(buf, encoded_byte)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
    } while (remaining_length);

    return AWS_OP_SUCCESS;
}

int aws_mqtt311_decode_remaining_length(struct aws_byte_cursor *cur, size_t *remaining_length_out) {

    AWS_PRECONDITION(cur);

    /* Read remaining_length */
    size_t multiplier = 1;
    size_t remaining_length = 0;
    while (true) {
        uint8_t encoded_byte;
        if (!aws_byte_cursor_read_u8(cur, &encoded_byte)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }

        remaining_length += (encoded_byte & 127) * multiplier;
        multiplier *= 128;

        if (!(encoded_byte & 128)) {
            break;
        }
        if (multiplier > 128 * 128 * 128) {
            /* If high order bit is set on last byte, value is malformed */
            return aws_raise_error(AWS_ERROR_MQTT_INVALID_REMAINING_LENGTH);
        }
    }

    *remaining_length_out = remaining_length;
    return AWS_OP_SUCCESS;
}

enum aws_mqtt_packet_type aws_mqtt_get_packet_type(const uint8_t *buffer) {
    return *buffer >> 4;
}

bool aws_mqtt_packet_has_flags(const struct aws_mqtt_fixed_header *header) {

    /* Parse attributes based on packet type */
    switch (header->packet_type) {
        case AWS_MQTT_PACKET_SUBSCRIBE:
        case AWS_MQTT_PACKET_UNSUBSCRIBE:
        case AWS_MQTT_PACKET_PUBLISH:
        case AWS_MQTT_PACKET_PUBREL:
            return true;
            break;

        case AWS_MQTT_PACKET_CONNECT:
        case AWS_MQTT_PACKET_CONNACK:
        case AWS_MQTT_PACKET_PUBACK:
        case AWS_MQTT_PACKET_PUBREC:
        case AWS_MQTT_PACKET_PUBCOMP:
        case AWS_MQTT_PACKET_SUBACK:
        case AWS_MQTT_PACKET_UNSUBACK:
        case AWS_MQTT_PACKET_PINGREQ:
        case AWS_MQTT_PACKET_PINGRESP:
        case AWS_MQTT_PACKET_DISCONNECT:
            return false;

        default:
            return false;
    }
}

int aws_mqtt_fixed_header_encode(struct aws_byte_buf *buf, const struct aws_mqtt_fixed_header *header) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(header);

    /* Check that flags are 0 if they must not be present */
    if (!aws_mqtt_packet_has_flags(header) && header->flags != 0) {
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_RESERVED_BITS);
    }

    /* Write packet type and flags */
    uint8_t byte_1 = (uint8_t)((header->packet_type << 4) | (header->flags & 0xF));
    if (!aws_byte_buf_write_u8(buf, byte_1)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write remaining length */
    if (s_encode_remaining_length(buf, header->remaining_length)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_fixed_header_decode(struct aws_byte_cursor *cur, struct aws_mqtt_fixed_header *header) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(header);

    /* Read packet type and flags */
    uint8_t byte_1 = 0;
    if (!aws_byte_cursor_read_u8(cur, &byte_1)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    header->packet_type = aws_mqtt_get_packet_type(&byte_1);
    header->flags = byte_1 & 0xF;

    /* Read remaining length */
    if (aws_mqtt311_decode_remaining_length(cur, &header->remaining_length)) {
        return AWS_OP_ERR;
    }
    if (cur->len < header->remaining_length) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Check that flags are 0 if they must not be present */
    if (!aws_mqtt_packet_has_flags(header) && header->flags != 0) {
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_RESERVED_BITS);
    }

    return AWS_OP_SUCCESS;
}
