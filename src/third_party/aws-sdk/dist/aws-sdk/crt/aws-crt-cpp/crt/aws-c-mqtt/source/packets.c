/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/packets.h>

enum { S_PROTOCOL_LEVEL = 4 };
enum { S_BIT_1_FLAGS = 0x2 };

static struct aws_byte_cursor s_protocol_name = {
    .ptr = (uint8_t *)"MQTT",
    .len = 4,
};

static size_t s_sizeof_encoded_buffer(struct aws_byte_cursor *buf) {
    return sizeof(uint16_t) + buf->len;
}

static int s_encode_buffer(struct aws_byte_buf *buf, const struct aws_byte_cursor cur) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(aws_byte_cursor_is_valid(&cur));

    /* Make sure the buffer isn't too big */
    if (cur.len > UINT16_MAX) {
        return aws_raise_error(AWS_ERROR_MQTT_BUFFER_TOO_BIG);
    }

    /* Write the length */
    if (!aws_byte_buf_write_be16(buf, (uint16_t)cur.len)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write the data */
    if (!aws_byte_buf_write(buf, cur.ptr, cur.len)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

static int s_decode_buffer(struct aws_byte_cursor *cur, struct aws_byte_cursor *buf) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(buf);

    /* Read the length */
    uint16_t len;
    if (!aws_byte_cursor_read_be16(cur, &len)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Store the data */
    *buf = aws_byte_cursor_advance(cur, len);

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Ack without payload                                                       */

static void s_ack_init(struct aws_mqtt_packet_ack *packet, enum aws_mqtt_packet_type type, uint16_t packet_identifier) {

    AWS_PRECONDITION(packet);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = type;
    packet->fixed_header.remaining_length = sizeof(uint16_t);

    packet->packet_identifier = packet_identifier;
}

int aws_mqtt_packet_ack_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_ack *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write packet identifier */
    if (!aws_byte_buf_write_be16(buf, packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_ack_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_ack *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /* Validate flags */
    if (packet->fixed_header.flags != (aws_mqtt_packet_has_flags(&packet->fixed_header) ? S_BIT_1_FLAGS : 0U)) {

        return aws_raise_error(AWS_ERROR_MQTT_INVALID_RESERVED_BITS);
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read packet identifier */
    if (!aws_byte_cursor_read_be16(cur, &packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Connect                                                                   */

int aws_mqtt_packet_connect_init(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor client_identifier,
    bool clean_session,
    uint16_t keep_alive) {

    AWS_PRECONDITION(packet);
    AWS_PRECONDITION(client_identifier.len > 0);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_CONNECT;
    /* [MQTT-3.1.1] */
    packet->fixed_header.remaining_length = 10 + s_sizeof_encoded_buffer(&client_identifier);

    packet->client_identifier = client_identifier;
    packet->clean_session = clean_session;
    packet->keep_alive_timeout = keep_alive;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connect_add_credentials(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor username,
    struct aws_byte_cursor password) {

    AWS_PRECONDITION(packet);
    AWS_PRECONDITION(username.len > 0);

    if (!packet->has_username) {
        /* If not already username, add size of length field */
        packet->fixed_header.remaining_length += 2;
    }

    /* Add change in size to remaining_length */
    packet->fixed_header.remaining_length += username.len - packet->username.len;
    packet->has_username = true;

    packet->username = username;

    if (password.len > 0) {

        if (!packet->has_password) {
            /* If not already password, add size of length field */
            packet->fixed_header.remaining_length += 2;
        }

        /* Add change in size to remaining_length */
        packet->fixed_header.remaining_length += password.len - packet->password.len;
        packet->has_password = true;

        packet->password = password;
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connect_add_will(
    struct aws_mqtt_packet_connect *packet,
    struct aws_byte_cursor topic,
    enum aws_mqtt_qos qos,
    bool retain,
    struct aws_byte_cursor payload) {

    packet->has_will = true;
    packet->will_topic = topic;
    packet->will_qos = qos;
    packet->will_retain = retain;
    packet->will_message = payload;

    packet->fixed_header.remaining_length += s_sizeof_encoded_buffer(&topic) + s_sizeof_encoded_buffer(&payload);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connect_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connect *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /* Do validation */
    if (packet->has_password && !packet->has_username) {

        return aws_raise_error(AWS_ERROR_MQTT_INVALID_CREDENTIALS);
    }

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write protocol name */
    if (s_encode_buffer(buf, s_protocol_name)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write protocol level */
    if (!aws_byte_buf_write_u8(buf, S_PROTOCOL_LEVEL)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write connect flags [MQTT-3.1.2.3] */
    uint8_t connect_flags = (uint8_t)(packet->clean_session << 1 | packet->has_will << 2 | packet->will_qos << 3 |
                                      packet->will_retain << 5 | packet->has_password << 6 | packet->has_username << 7);

    if (!aws_byte_buf_write_u8(buf, connect_flags)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write keep alive */
    if (!aws_byte_buf_write_be16(buf, packet->keep_alive_timeout)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /*************************************************************************/
    /* Payload                                                               */

    /* Client identifier is required, write it */
    if (s_encode_buffer(buf, packet->client_identifier)) {
        return AWS_OP_ERR;
    }

    /* Write will */
    if (packet->has_will) {
        if (s_encode_buffer(buf, packet->will_topic)) {
            return AWS_OP_ERR;
        }
        if (s_encode_buffer(buf, packet->will_message)) {
            return AWS_OP_ERR;
        }
    }

    /* Write username */
    if (packet->has_username) {
        if (s_encode_buffer(buf, packet->username)) {
            return AWS_OP_ERR;
        }
    }

    /* Write password */
    if (packet->has_password) {
        if (s_encode_buffer(buf, packet->password)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connect_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connect *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header                                                          */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Check protocol name */
    struct aws_byte_cursor protocol_name = {
        .ptr = NULL,
        .len = 0,
    };
    if (s_decode_buffer(cur, &protocol_name)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    AWS_ASSERT(protocol_name.ptr && protocol_name.len);
    if (protocol_name.len != s_protocol_name.len) {
        return aws_raise_error(AWS_ERROR_MQTT_UNSUPPORTED_PROTOCOL_NAME);
    }
    if (memcmp(protocol_name.ptr, s_protocol_name.ptr, s_protocol_name.len) != 0) {
        return aws_raise_error(AWS_ERROR_MQTT_UNSUPPORTED_PROTOCOL_NAME);
    }

    /* Check protocol level */
    struct aws_byte_cursor protocol_level = aws_byte_cursor_advance(cur, 1);
    if (protocol_level.len == 0) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    if (*protocol_level.ptr != S_PROTOCOL_LEVEL) {
        return aws_raise_error(AWS_ERROR_MQTT_UNSUPPORTED_PROTOCOL_LEVEL);
    }

    /* Read connect flags [MQTT-3.1.2.3] */
    uint8_t connect_flags = 0;
    if (!aws_byte_cursor_read_u8(cur, &connect_flags)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    packet->clean_session = (connect_flags >> 1) & 0x1;
    packet->has_will = (connect_flags >> 2) & 0x1;
    packet->will_qos = (connect_flags >> 3) & 0x3;
    packet->will_retain = (connect_flags >> 5) & 0x1;
    packet->has_password = (connect_flags >> 6) & 0x1;
    packet->has_username = (connect_flags >> 7) & 0x1;

    /* Read keep alive */
    if (!aws_byte_cursor_read_be16(cur, &packet->keep_alive_timeout)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /*************************************************************************/
    /* Payload                                                               */

    /* Client identifier is required, Read it */
    if (s_decode_buffer(cur, &packet->client_identifier)) {
        return AWS_OP_ERR;
    }

    /* Read will */
    if (packet->has_will) {
        if (s_decode_buffer(cur, &packet->will_topic)) {
            return AWS_OP_ERR;
        }
        if (s_decode_buffer(cur, &packet->will_message)) {
            return AWS_OP_ERR;
        }
    }

    /* Read username */
    if (packet->has_username) {
        if (s_decode_buffer(cur, &packet->username)) {
            return AWS_OP_ERR;
        }
    }

    /* Read password */
    if (packet->has_password) {
        if (s_decode_buffer(cur, &packet->password)) {
            return AWS_OP_ERR;
        }
    }

    /* Do validation */
    if (packet->has_password && !packet->has_username) {

        return aws_raise_error(AWS_ERROR_MQTT_INVALID_CREDENTIALS);
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Connack                                                                   */

int aws_mqtt_packet_connack_init(
    struct aws_mqtt_packet_connack *packet,
    bool session_present,
    enum aws_mqtt_connect_return_code return_code) {

    AWS_PRECONDITION(packet);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_CONNACK;
    packet->fixed_header.remaining_length = 1 + sizeof(packet->connect_return_code);

    packet->session_present = session_present;
    packet->connect_return_code = (uint8_t)return_code;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connack_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connack *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read connack flags */
    uint8_t connack_flags = packet->session_present & 0x1;
    if (!aws_byte_buf_write_u8(buf, connack_flags)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Read return code */
    if (!aws_byte_buf_write_u8(buf, packet->connect_return_code)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connack_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connack *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header                                                          */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read connack flags */
    uint8_t connack_flags = 0;
    if (!aws_byte_cursor_read_u8(cur, &connack_flags)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    packet->session_present = connack_flags & 0x1;

    /* Read return code */
    if (!aws_byte_cursor_read_u8(cur, &packet->connect_return_code)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Publish                                                                   */

int aws_mqtt_packet_publish_init(
    struct aws_mqtt_packet_publish *packet,
    bool retain,
    enum aws_mqtt_qos qos,
    bool dup,
    struct aws_byte_cursor topic_name,
    uint16_t packet_identifier,
    struct aws_byte_cursor payload) {

    AWS_PRECONDITION(packet);
    AWS_FATAL_PRECONDITION(topic_name.len > 0); /* [MQTT-4.7.3-1] */

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_PUBLISH;
    packet->fixed_header.remaining_length = s_sizeof_encoded_buffer(&topic_name) + payload.len;

    if (qos > 0) {
        packet->fixed_header.remaining_length += sizeof(packet->packet_identifier);
    }

    /* [MQTT-2.2.2] */
    uint8_t publish_flags = (uint8_t)((retain & 0x1) | (qos & 0x3) << 1 | (dup & 0x1) << 3);
    packet->fixed_header.flags = publish_flags;

    packet->topic_name = topic_name;
    packet->packet_identifier = packet_identifier;
    packet->payload = payload;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_publish_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_publish *packet) {

    if (aws_mqtt_packet_publish_encode_headers(buf, packet)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Payload                                                               */

    if (!aws_byte_buf_write(buf, packet->payload.ptr, packet->payload.len)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_publish_encode_headers(struct aws_byte_buf *buf, const struct aws_mqtt_packet_publish *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write topic name */
    if (s_encode_buffer(buf, packet->topic_name)) {
        return AWS_OP_ERR;
    }

    enum aws_mqtt_qos qos = aws_mqtt_packet_publish_get_qos(packet);
    if (qos > 0) {
        /* Write packet identifier */
        if (!aws_byte_buf_write_be16(buf, packet->packet_identifier)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_publish_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_publish *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read topic name */
    if (s_decode_buffer(cur, &packet->topic_name)) {
        return AWS_OP_ERR;
    }

    size_t payload_size = packet->fixed_header.remaining_length - s_sizeof_encoded_buffer(&packet->topic_name);

    /* Read QoS */
    enum aws_mqtt_qos qos = aws_mqtt_packet_publish_get_qos(packet);
    if (qos > 2) {
        return aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
    }

    /* Read packet identifier */
    if (qos > 0) {
        if (!aws_byte_cursor_read_be16(cur, &packet->packet_identifier)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
        payload_size -= sizeof(packet->packet_identifier);
    } else {
        packet->packet_identifier = 0;
    }

    /*************************************************************************/
    /* Payload                                                               */
    packet->payload = aws_byte_cursor_advance(cur, payload_size);
    if (packet->payload.len != payload_size) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    return AWS_OP_SUCCESS;
}

bool aws_mqtt_packet_publish_get_dup(const struct aws_mqtt_packet_publish *packet) {
    return packet->fixed_header.flags & (1 << 3); /* bit 3 */
}

void aws_mqtt_packet_publish_set_dup(struct aws_mqtt_packet_publish *packet) {
    packet->fixed_header.flags |= 0x08;
}

enum aws_mqtt_qos aws_mqtt_packet_publish_get_qos(const struct aws_mqtt_packet_publish *packet) {
    return (packet->fixed_header.flags >> 1) & 0x3; /* bits 2,1 */
}

bool aws_mqtt_packet_publish_get_retain(const struct aws_mqtt_packet_publish *packet) {
    return packet->fixed_header.flags & 0x1; /* bit 0 */
}

/*****************************************************************************/
/* Puback                                                                    */

int aws_mqtt_packet_puback_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier) {

    s_ack_init(packet, AWS_MQTT_PACKET_PUBACK, packet_identifier);

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Pubrec                                                                    */

int aws_mqtt_packet_pubrec_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier) {

    s_ack_init(packet, AWS_MQTT_PACKET_PUBREC, packet_identifier);

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Pubrel                                                                    */

int aws_mqtt_packet_pubrel_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier) {

    s_ack_init(packet, AWS_MQTT_PACKET_PUBREL, packet_identifier);
    packet->fixed_header.flags = S_BIT_1_FLAGS;

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Pubcomp                                                                   */

int aws_mqtt_packet_pubcomp_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier) {

    s_ack_init(packet, AWS_MQTT_PACKET_PUBCOMP, packet_identifier);

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Subscribe                                                                 */

int aws_mqtt_packet_subscribe_init(
    struct aws_mqtt_packet_subscribe *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier) {

    AWS_PRECONDITION(packet);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_SUBSCRIBE;
    packet->fixed_header.flags = S_BIT_1_FLAGS;
    packet->fixed_header.remaining_length = sizeof(uint16_t);

    packet->packet_identifier = packet_identifier;

    if (aws_array_list_init_dynamic(&packet->topic_filters, allocator, 1, sizeof(struct aws_mqtt_subscription))) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt_packet_subscribe_clean_up(struct aws_mqtt_packet_subscribe *packet) {

    AWS_PRECONDITION(packet);

    aws_array_list_clean_up(&packet->topic_filters);

    AWS_ZERO_STRUCT(*packet);
}

int aws_mqtt_packet_subscribe_add_topic(
    struct aws_mqtt_packet_subscribe *packet,
    struct aws_byte_cursor topic_filter,
    enum aws_mqtt_qos qos) {

    AWS_PRECONDITION(packet);

    /* Add to the array list */
    struct aws_mqtt_subscription subscription;
    subscription.topic_filter = topic_filter;
    subscription.qos = qos;
    if (aws_array_list_push_back(&packet->topic_filters, &subscription)) {
        return AWS_OP_ERR;
    }

    /* Add to the remaining length */
    packet->fixed_header.remaining_length += s_sizeof_encoded_buffer(&topic_filter) + 1;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_subscribe_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_subscribe *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write packet identifier */
    if (!aws_byte_buf_write_be16(buf, packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write topic filters */
    const size_t num_filters = aws_array_list_length(&packet->topic_filters);
    for (size_t i = 0; i < num_filters; ++i) {

        struct aws_mqtt_subscription *subscription;
        if (aws_array_list_get_at_ptr(&packet->topic_filters, (void **)&subscription, i)) {

            return AWS_OP_ERR;
        }
        s_encode_buffer(buf, subscription->topic_filter);

        uint8_t eos_byte = subscription->qos & 0x3;
        if (!aws_byte_buf_write_u8(buf, eos_byte)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_subscribe_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_subscribe *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    if (packet->fixed_header.remaining_length < sizeof(uint16_t)) {
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_REMAINING_LENGTH);
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read packet identifier */
    if (!aws_byte_cursor_read_be16(cur, &packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Read topic filters */
    size_t remaining_length = packet->fixed_header.remaining_length - sizeof(uint16_t);
    while (remaining_length) {

        struct aws_mqtt_subscription subscription = {
            .topic_filter = {.ptr = NULL, .len = 0},
            .qos = 0,
        };
        if (s_decode_buffer(cur, &subscription.topic_filter)) {
            return AWS_OP_ERR;
        }

        uint8_t eos_byte = 0;
        if (!aws_byte_cursor_read_u8(cur, &eos_byte)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
        if ((eos_byte >> 2) != 0) {
            return aws_raise_error(AWS_ERROR_MQTT_INVALID_RESERVED_BITS);
        }
        if (eos_byte == 0x3) {
            return aws_raise_error(AWS_ERROR_MQTT_INVALID_QOS);
        }
        subscription.qos = eos_byte & 0x3;

        aws_array_list_push_back(&packet->topic_filters, &subscription);

        remaining_length -= s_sizeof_encoded_buffer(&subscription.topic_filter) + 1;
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Suback                                                                    */

int aws_mqtt_packet_suback_init(
    struct aws_mqtt_packet_suback *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier) {

    AWS_PRECONDITION(packet);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_SUBACK;
    packet->fixed_header.remaining_length = sizeof(uint16_t);

    packet->packet_identifier = packet_identifier;

    if (aws_array_list_init_dynamic(&packet->return_codes, allocator, 1, sizeof(uint8_t))) {
        return AWS_OP_ERR;
    }
    return AWS_OP_SUCCESS;
}

void aws_mqtt_packet_suback_clean_up(struct aws_mqtt_packet_suback *packet) {

    AWS_PRECONDITION(packet);

    aws_array_list_clean_up(&packet->return_codes);

    AWS_ZERO_STRUCT(*packet);
}

static bool s_return_code_check(uint8_t return_code) {
    if (return_code != AWS_MQTT_QOS_FAILURE && return_code != AWS_MQTT_QOS_AT_MOST_ONCE &&
        return_code != AWS_MQTT_QOS_AT_LEAST_ONCE && return_code != AWS_MQTT_QOS_EXACTLY_ONCE) {
        return false;
    }
    return true;
}

int aws_mqtt_packet_suback_add_return_code(struct aws_mqtt_packet_suback *packet, uint8_t return_code) {

    AWS_PRECONDITION(packet);
    if (!(s_return_code_check(return_code))) {
        return aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
    }

    /* Add to the array list */
    if (aws_array_list_push_back(&packet->return_codes, &return_code)) {
        return AWS_OP_ERR;
    }

    /* Add to the remaining length, each return code takes one byte */
    packet->fixed_header.remaining_length += 1;

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_suback_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_suback *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write packet identifier */
    if (!aws_byte_buf_write_be16(buf, packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /*************************************************************************/
    /* Payload                                                               */

    /* Write topic filters */
    const size_t num_filters = aws_array_list_length(&packet->return_codes);
    for (size_t i = 0; i < num_filters; ++i) {

        uint8_t return_code = 0;
        if (aws_array_list_get_at(&packet->return_codes, (void *)&return_code, i)) {
            return AWS_OP_ERR;
        }
        if (!aws_byte_buf_write_u8(buf, return_code)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
    }
    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_suback_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_suback *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /* Validate flags */
    if (packet->fixed_header.flags != (aws_mqtt_packet_has_flags(&packet->fixed_header) ? S_BIT_1_FLAGS : 0U)) {

        return aws_raise_error(AWS_ERROR_MQTT_INVALID_RESERVED_BITS);
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read packet identifier */
    if (!aws_byte_cursor_read_be16(cur, &packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /*************************************************************************/
    /* Payload                                                               */

    /* Read return codes */
    size_t remaining_length = packet->fixed_header.remaining_length - sizeof(uint16_t);
    while (remaining_length) {

        uint8_t return_code = 0;
        if (!aws_byte_cursor_read_u8(cur, &return_code)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
        if (!(s_return_code_check(return_code))) {
            return aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
        }

        aws_array_list_push_back(&packet->return_codes, &return_code);

        remaining_length -= 1;
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Unsubscribe                                                               */

int aws_mqtt_packet_unsubscribe_init(
    struct aws_mqtt_packet_unsubscribe *packet,
    struct aws_allocator *allocator,
    uint16_t packet_identifier) {

    AWS_PRECONDITION(packet);
    AWS_PRECONDITION(allocator);

    AWS_ZERO_STRUCT(*packet);

    packet->fixed_header.packet_type = AWS_MQTT_PACKET_UNSUBSCRIBE;
    packet->fixed_header.flags = S_BIT_1_FLAGS;
    packet->fixed_header.remaining_length = sizeof(uint16_t);

    packet->packet_identifier = packet_identifier;

    if (aws_array_list_init_dynamic(&packet->topic_filters, allocator, 1, sizeof(struct aws_byte_cursor))) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt_packet_unsubscribe_clean_up(struct aws_mqtt_packet_unsubscribe *packet) {

    AWS_PRECONDITION(packet);

    aws_array_list_clean_up(&packet->topic_filters);

    AWS_ZERO_STRUCT(*packet);
}

int aws_mqtt_packet_unsubscribe_add_topic(
    struct aws_mqtt_packet_unsubscribe *packet,
    struct aws_byte_cursor topic_filter) {

    AWS_PRECONDITION(packet);

    /* Add to the array list */
    if (aws_array_list_push_back(&packet->topic_filters, &topic_filter)) {
        return AWS_OP_ERR;
    }

    /* Add to the remaining length */
    packet->fixed_header.remaining_length += s_sizeof_encoded_buffer(&topic_filter);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_unsubscribe_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_unsubscribe *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Write packet identifier */
    if (!aws_byte_buf_write_be16(buf, packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Write topic filters */
    const size_t num_filters = aws_array_list_length(&packet->topic_filters);
    for (size_t i = 0; i < num_filters; ++i) {

        struct aws_byte_cursor topic_filter = {.ptr = NULL, .len = 0};
        if (aws_array_list_get_at(&packet->topic_filters, (void *)&topic_filter, i)) {

            return AWS_OP_ERR;
        }
        s_encode_buffer(buf, topic_filter);
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_unsubscribe_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_unsubscribe *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    if (packet->fixed_header.remaining_length < sizeof(uint16_t)) {
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_REMAINING_LENGTH);
    }

    /*************************************************************************/
    /* Variable Header                                                       */

    /* Read packet identifier */
    if (!aws_byte_cursor_read_be16(cur, &packet->packet_identifier)) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    /* Read topic filters */
    size_t remaining_length = packet->fixed_header.remaining_length - sizeof(uint16_t);
    while (remaining_length) {

        struct aws_byte_cursor topic_filter;
        AWS_ZERO_STRUCT(topic_filter);
        if (s_decode_buffer(cur, &topic_filter)) {
            return AWS_OP_ERR;
        }

        aws_array_list_push_back(&packet->topic_filters, &topic_filter);

        remaining_length -= s_sizeof_encoded_buffer(&topic_filter);
    }

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Unsuback                                                                  */

int aws_mqtt_packet_unsuback_init(struct aws_mqtt_packet_ack *packet, uint16_t packet_identifier) {

    s_ack_init(packet, AWS_MQTT_PACKET_UNSUBACK, packet_identifier);

    return AWS_OP_SUCCESS;
}

/*****************************************************************************/
/* Ping request/response                                                     */

static void s_connection_init(struct aws_mqtt_packet_connection *packet, enum aws_mqtt_packet_type type) {

    AWS_PRECONDITION(packet);

    AWS_ZERO_STRUCT(*packet);
    packet->fixed_header.packet_type = type;
}

int aws_mqtt_packet_pingreq_init(struct aws_mqtt_packet_connection *packet) {

    s_connection_init(packet, AWS_MQTT_PACKET_PINGREQ);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_pingresp_init(struct aws_mqtt_packet_connection *packet) {

    s_connection_init(packet, AWS_MQTT_PACKET_PINGRESP);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_disconnect_init(struct aws_mqtt_packet_connection *packet) {

    s_connection_init(packet, AWS_MQTT_PACKET_DISCONNECT);

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connection_encode(struct aws_byte_buf *buf, const struct aws_mqtt_packet_connection *packet) {

    AWS_PRECONDITION(buf);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_encode(buf, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt_packet_connection_decode(struct aws_byte_cursor *cur, struct aws_mqtt_packet_connection *packet) {

    AWS_PRECONDITION(cur);
    AWS_PRECONDITION(packet);

    /*************************************************************************/
    /* Fixed Header */

    if (aws_mqtt_fixed_header_decode(cur, &packet->fixed_header)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}
