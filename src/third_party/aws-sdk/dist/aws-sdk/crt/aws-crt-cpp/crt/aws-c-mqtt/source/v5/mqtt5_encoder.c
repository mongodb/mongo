/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/v5/mqtt5_encoder.h>

#include <aws/io/stream.h>
#include <aws/mqtt/private/v5/mqtt5_topic_alias.h>
#include <aws/mqtt/private/v5/mqtt5_utils.h>
#include <aws/mqtt/v5/mqtt5_types.h>

#include <inttypes.h>

#define INITIAL_ENCODING_STEP_COUNT 64
#define SUBSCRIBE_PACKET_FIXED_HEADER_RESERVED_BITS 2
#define UNSUBSCRIBE_PACKET_FIXED_HEADER_RESERVED_BITS 2

int aws_mqtt5_encode_variable_length_integer(struct aws_byte_buf *buf, uint32_t value) {
    AWS_PRECONDITION(buf);

    if (value > AWS_MQTT5_MAXIMUM_VARIABLE_LENGTH_INTEGER) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    do {
        uint8_t encoded_byte = value % 128;
        value /= 128;
        if (value) {
            encoded_byte |= 128;
        }
        if (!aws_byte_buf_write_u8(buf, encoded_byte)) {
            return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        }
    } while (value);

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_get_variable_length_encode_size(size_t value, size_t *encode_size) {
    if (value > AWS_MQTT5_MAXIMUM_VARIABLE_LENGTH_INTEGER) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (value < 128) {
        *encode_size = 1;
    } else if (value < 16384) {
        *encode_size = 2;
    } else if (value < 2097152) {
        *encode_size = 3;
    } else {
        *encode_size = 4;
    }

    return AWS_OP_SUCCESS;
}

/* helper functions that add a single type of encoding step to the list of steps in an encoder */

void aws_mqtt5_encoder_push_step_u8(struct aws_mqtt5_encoder *encoder, uint8_t value) {
    struct aws_mqtt5_encoding_step step;
    AWS_ZERO_STRUCT(step);

    step.type = AWS_MQTT5_EST_U8;
    step.value.value_u8 = value;

    aws_array_list_push_back(&encoder->encoding_steps, &step);
}

void aws_mqtt5_encoder_push_step_u16(struct aws_mqtt5_encoder *encoder, uint16_t value) {
    struct aws_mqtt5_encoding_step step;
    AWS_ZERO_STRUCT(step);

    step.type = AWS_MQTT5_EST_U16;
    step.value.value_u16 = value;

    aws_array_list_push_back(&encoder->encoding_steps, &step);
}

void aws_mqtt5_encoder_push_step_u32(struct aws_mqtt5_encoder *encoder, uint32_t value) {
    struct aws_mqtt5_encoding_step step;
    AWS_ZERO_STRUCT(step);

    step.type = AWS_MQTT5_EST_U32;
    step.value.value_u32 = value;

    aws_array_list_push_back(&encoder->encoding_steps, &step);
}

int aws_mqtt5_encoder_push_step_vli(struct aws_mqtt5_encoder *encoder, uint32_t value) {
    if (value > AWS_MQTT5_MAXIMUM_VARIABLE_LENGTH_INTEGER) {
        return aws_raise_error(AWS_ERROR_MQTT5_ENCODE_FAILURE);
    }

    struct aws_mqtt5_encoding_step step;
    AWS_ZERO_STRUCT(step);

    step.type = AWS_MQTT5_EST_VLI;
    step.value.value_u32 = value;

    aws_array_list_push_back(&encoder->encoding_steps, &step);

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_encoder_push_step_cursor(struct aws_mqtt5_encoder *encoder, struct aws_byte_cursor value) {
    struct aws_mqtt5_encoding_step step;
    AWS_ZERO_STRUCT(step);

    step.type = AWS_MQTT5_EST_CURSOR;
    step.value.value_cursor = value;

    aws_array_list_push_back(&encoder->encoding_steps, &step);
}

/*
 * All size calculations are done with size_t.  We assume that view validation will catch and fail all packets
 * that violate length constraints either from the MQTT5 spec or additional constraints that we impose on packets
 * to ensure that the size calculations do not need to perform checked arithmetic.  The only place where we need
 * to use checked arithmetic is a PUBLISH packet when combining the payload size and "sizeof everything else"
 *
 * The additional beyond-spec constraints we apply to view validation ensure our results actually fit in 32 bits.
 */
size_t aws_mqtt5_compute_user_property_encode_length(
    const struct aws_mqtt5_user_property *properties,
    size_t user_property_count) {
    /*
     * for each user property, in addition to the raw name-value bytes, we also have 5 bytes of prefix required:
     *  1 byte for the property type
     *  2 bytes for the name length
     *  2 bytes for the value length
     */
    size_t length = 5 * user_property_count;

    for (size_t i = 0; i < user_property_count; ++i) {
        const struct aws_mqtt5_user_property *property = &properties[i];

        length += property->name.len;
        length += property->value.len;
    }

    return length;
}

void aws_mqtt5_add_user_property_encoding_steps(
    struct aws_mqtt5_encoder *encoder,
    const struct aws_mqtt5_user_property *user_properties,
    size_t user_property_count) {
    for (size_t i = 0; i < user_property_count; ++i) {
        const struct aws_mqtt5_user_property *property = &user_properties[i];

        /* https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901054 */
        ADD_ENCODE_STEP_U8(encoder, AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY);
        ADD_ENCODE_STEP_U16(encoder, (uint16_t)property->name.len);
        ADD_ENCODE_STEP_CURSOR(encoder, property->name);
        ADD_ENCODE_STEP_U16(encoder, (uint16_t)property->value.len);
        ADD_ENCODE_STEP_CURSOR(encoder, property->value);
    }
}

static int s_aws_mqtt5_encoder_begin_pingreq(struct aws_mqtt5_encoder *encoder, const void *view) {
    (void)view;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: setting up encode for a PINGREQ packet", (void *)encoder->config.client);

    /* A ping is just a fixed header with a 0-valued remaining length which we encode as a 0 u8 rather than a 0 vli */
    ADD_ENCODE_STEP_U8(encoder, aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_PINGREQ, 0));
    ADD_ENCODE_STEP_U8(encoder, 0);

    return AWS_OP_SUCCESS;
}

static int s_compute_disconnect_variable_length_fields(
    const struct aws_mqtt5_packet_disconnect_view *disconnect_view,
    size_t *total_remaining_length,
    size_t *property_length) {
    size_t local_property_length = aws_mqtt5_compute_user_property_encode_length(
        disconnect_view->user_properties, disconnect_view->user_property_count);

    ADD_OPTIONAL_U32_PROPERTY_LENGTH(disconnect_view->session_expiry_interval_seconds, local_property_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(disconnect_view->server_reference, local_property_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(disconnect_view->reason_string, local_property_length);

    *property_length = local_property_length;

    size_t property_length_encoding_length = 0;
    if (aws_mqtt5_get_variable_length_encode_size(local_property_length, &property_length_encoding_length)) {
        return AWS_OP_ERR;
    }

    /* reason code is the only other thing to worry about */
    *total_remaining_length = 1 + *property_length + property_length_encoding_length;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_encoder_begin_disconnect(struct aws_mqtt5_encoder *encoder, const void *view) {

    const struct aws_mqtt5_packet_disconnect_view *disconnect_view = view;

    size_t total_remaining_length = 0;
    size_t property_length = 0;
    if (s_compute_disconnect_variable_length_fields(disconnect_view, &total_remaining_length, &property_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to compute variable length values for DISCONNECT packet with error "
            "%d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t property_length_u32 = (uint32_t)property_length;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: setting up encode for a DISCONNECT packet with remaining length %" PRIu32,
        (void *)encoder->config.client,
        total_remaining_length_u32);

    ADD_ENCODE_STEP_U8(encoder, aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_DISCONNECT, 0));
    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);
    ADD_ENCODE_STEP_U8(encoder, (uint8_t)disconnect_view->reason_code);
    ADD_ENCODE_STEP_VLI(encoder, property_length_u32);

    if (property_length > 0) {
        ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_SESSION_EXPIRY_INTERVAL, disconnect_view->session_expiry_interval_seconds);
        ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_REASON_STRING, disconnect_view->reason_string);
        ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_SERVER_REFERENCE, disconnect_view->server_reference);

        aws_mqtt5_add_user_property_encoding_steps(
            encoder, disconnect_view->user_properties, disconnect_view->user_property_count);
    }

    return AWS_OP_SUCCESS;
}

static int s_compute_connect_variable_length_fields(
    const struct aws_mqtt5_packet_connect_view *connect_view,
    size_t *total_remaining_length,
    size_t *connect_property_length,
    size_t *will_property_length) {

    size_t connect_property_section_length =
        aws_mqtt5_compute_user_property_encode_length(connect_view->user_properties, connect_view->user_property_count);

    ADD_OPTIONAL_U32_PROPERTY_LENGTH(connect_view->session_expiry_interval_seconds, connect_property_section_length);
    ADD_OPTIONAL_U16_PROPERTY_LENGTH(connect_view->receive_maximum, connect_property_section_length);
    ADD_OPTIONAL_U32_PROPERTY_LENGTH(connect_view->maximum_packet_size_bytes, connect_property_section_length);
    ADD_OPTIONAL_U16_PROPERTY_LENGTH(connect_view->topic_alias_maximum, connect_property_section_length);
    ADD_OPTIONAL_U8_PROPERTY_LENGTH(connect_view->request_response_information, connect_property_section_length);
    ADD_OPTIONAL_U8_PROPERTY_LENGTH(connect_view->request_problem_information, connect_property_section_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(connect_view->authentication_method, connect_property_section_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(connect_view->authentication_data, connect_property_section_length);

    *connect_property_length = (uint32_t)connect_property_section_length;

    /* variable header length =
     *    10 bytes (6 for mqtt string, 1 for protocol version, 1 for flags, 2 for keep alive)
     *  + # bytes(variable_length_encoding(connect_property_section_length))
     *  + connect_property_section_length
     */
    size_t variable_header_length = 0;
    if (aws_mqtt5_get_variable_length_encode_size(connect_property_section_length, &variable_header_length)) {
        return AWS_OP_ERR;
    }

    variable_header_length += 10 + connect_property_section_length;

    size_t payload_length = 2 + connect_view->client_id.len;

    *will_property_length = 0;
    if (connect_view->will != NULL) {
        const struct aws_mqtt5_packet_publish_view *publish_view = connect_view->will;

        *will_property_length = aws_mqtt5_compute_user_property_encode_length(
            publish_view->user_properties, publish_view->user_property_count);

        ADD_OPTIONAL_U32_PROPERTY_LENGTH(connect_view->will_delay_interval_seconds, *will_property_length);
        ADD_OPTIONAL_U8_PROPERTY_LENGTH(publish_view->payload_format, *will_property_length);
        ADD_OPTIONAL_U32_PROPERTY_LENGTH(publish_view->message_expiry_interval_seconds, *will_property_length);
        ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->content_type, *will_property_length);
        ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->response_topic, *will_property_length);
        ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->correlation_data, *will_property_length);

        size_t will_properties_length_encode_size = 0;
        if (aws_mqtt5_get_variable_length_encode_size(
                (uint32_t)*will_property_length, &will_properties_length_encode_size)) {
            return AWS_OP_ERR;
        }

        payload_length += *will_property_length;
        payload_length += will_properties_length_encode_size;

        payload_length += 2 + publish_view->topic.len;
        payload_length += 2 + publish_view->payload.len;
    }

    /* Can't use the optional property macros because these don't have a leading property type byte */
    if (connect_view->username != NULL) {
        payload_length += connect_view->username->len + 2;
    }

    if (connect_view->password != NULL) {
        payload_length += connect_view->password->len + 2;
    }

    *total_remaining_length = payload_length + variable_header_length;

    return AWS_OP_SUCCESS;
}

static uint8_t s_aws_mqtt5_connect_compute_connect_flags(const struct aws_mqtt5_packet_connect_view *connect_view) {
    uint8_t flags = 0;

    if (connect_view->clean_start) {
        flags |= 1 << 1;
    }

    const struct aws_mqtt5_packet_publish_view *will = connect_view->will;
    if (will != NULL) {
        flags |= 1 << 2;
        flags |= ((uint8_t)will->qos) << 3;

        if (will->retain) {
            flags |= 1 << 5;
        }
    }

    if (connect_view->password != NULL) {
        flags |= 1 << 6;
    }

    if (connect_view->username != NULL) {
        flags |= 1 << 7;
    }

    return flags;
}

static int s_aws_mqtt5_encoder_begin_connect(struct aws_mqtt5_encoder *encoder, const void *view) {

    const struct aws_mqtt5_packet_connect_view *connect_view = view;
    const struct aws_mqtt5_packet_publish_view *will = connect_view->will;

    size_t total_remaining_length = 0;
    size_t connect_property_length = 0;
    size_t will_property_length = 0;
    if (s_compute_connect_variable_length_fields(
            connect_view, &total_remaining_length, &connect_property_length, &will_property_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to compute variable length values for CONNECT packet with error %d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: setting up encode for a CONNECT packet with remaining length %zu",
        (void *)encoder->config.client,
        total_remaining_length);

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t connect_property_length_u32 = (uint32_t)connect_property_length;
    uint32_t will_property_length_u32 = (uint32_t)will_property_length;

    ADD_ENCODE_STEP_U8(encoder, aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_CONNECT, 0));
    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);
    ADD_ENCODE_STEP_CURSOR(encoder, g_aws_mqtt5_connect_protocol_cursor);
    ADD_ENCODE_STEP_U8(encoder, s_aws_mqtt5_connect_compute_connect_flags(connect_view));
    ADD_ENCODE_STEP_U16(encoder, connect_view->keep_alive_interval_seconds);

    ADD_ENCODE_STEP_VLI(encoder, connect_property_length_u32);
    ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_SESSION_EXPIRY_INTERVAL, connect_view->session_expiry_interval_seconds);
    ADD_ENCODE_STEP_OPTIONAL_U16_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_RECEIVE_MAXIMUM, connect_view->receive_maximum);
    ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_MAXIMUM_PACKET_SIZE, connect_view->maximum_packet_size_bytes);
    ADD_ENCODE_STEP_OPTIONAL_U16_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_TOPIC_ALIAS_MAXIMUM, connect_view->topic_alias_maximum);
    ADD_ENCODE_STEP_OPTIONAL_U8_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_REQUEST_RESPONSE_INFORMATION, connect_view->request_response_information);
    ADD_ENCODE_STEP_OPTIONAL_U8_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_REQUEST_PROBLEM_INFORMATION, connect_view->request_problem_information);
    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_AUTHENTICATION_METHOD, connect_view->authentication_method);
    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_AUTHENTICATION_DATA, connect_view->authentication_data);

    aws_mqtt5_add_user_property_encoding_steps(
        encoder, connect_view->user_properties, connect_view->user_property_count);

    ADD_ENCODE_STEP_LENGTH_PREFIXED_CURSOR(encoder, connect_view->client_id);

    if (will != NULL) {
        ADD_ENCODE_STEP_VLI(encoder, will_property_length_u32);
        ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_WILL_DELAY_INTERVAL, connect_view->will_delay_interval_seconds);
        ADD_ENCODE_STEP_OPTIONAL_U8_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_PAYLOAD_FORMAT_INDICATOR, will->payload_format);
        ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_MESSAGE_EXPIRY_INTERVAL, will->message_expiry_interval_seconds);
        ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(encoder, AWS_MQTT5_PROPERTY_TYPE_CONTENT_TYPE, will->content_type);
        ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(encoder, AWS_MQTT5_PROPERTY_TYPE_RESPONSE_TOPIC, will->response_topic);
        ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_CORRELATION_DATA, will->correlation_data);

        aws_mqtt5_add_user_property_encoding_steps(encoder, will->user_properties, will->user_property_count);

        ADD_ENCODE_STEP_LENGTH_PREFIXED_CURSOR(encoder, will->topic);
        ADD_ENCODE_STEP_U16(encoder, (uint16_t)will->payload.len);
        ADD_ENCODE_STEP_CURSOR(encoder, will->payload);
    }

    ADD_ENCODE_STEP_OPTIONAL_LENGTH_PREFIXED_CURSOR(encoder, connect_view->username);
    ADD_ENCODE_STEP_OPTIONAL_LENGTH_PREFIXED_CURSOR(encoder, connect_view->password);

    return AWS_OP_SUCCESS;
}

static uint8_t s_aws_mqtt5_subscribe_compute_subscription_flags(
    const struct aws_mqtt5_subscription_view *subscription_view) {

    uint8_t flags = (uint8_t)subscription_view->qos;

    if (subscription_view->no_local) {
        flags |= 1 << 2;
    }

    if (subscription_view->retain_as_published) {
        flags |= 1 << 3;
    }

    flags |= ((uint8_t)subscription_view->retain_handling_type) << 4;

    return flags;
}

static void aws_mqtt5_add_subscribe_topic_filter_encoding_steps(
    struct aws_mqtt5_encoder *encoder,
    const struct aws_mqtt5_subscription_view *subscriptions,
    size_t subscription_count) {
    /* https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169 */
    for (size_t i = 0; i < subscription_count; ++i) {
        const struct aws_mqtt5_subscription_view *subscription = &subscriptions[i];
        ADD_ENCODE_STEP_LENGTH_PREFIXED_CURSOR(encoder, subscription->topic_filter);
        ADD_ENCODE_STEP_U8(encoder, s_aws_mqtt5_subscribe_compute_subscription_flags(subscription));
    }
}

static void aws_mqtt5_add_unsubscribe_topic_filter_encoding_steps(
    struct aws_mqtt5_encoder *encoder,
    const struct aws_byte_cursor *topics,
    size_t unsubscription_count) {
    /* https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901185 */
    for (size_t i = 0; i < unsubscription_count; ++i) {
        const struct aws_byte_cursor topic_filter = topics[i];
        ADD_ENCODE_STEP_LENGTH_PREFIXED_CURSOR(encoder, topic_filter);
    }
}

static int s_compute_subscribe_variable_length_fields(
    const struct aws_mqtt5_packet_subscribe_view *subscribe_view,
    size_t *total_remaining_length,
    size_t *subscribe_properties_length) {

    size_t subscribe_variable_header_property_length = aws_mqtt5_compute_user_property_encode_length(
        subscribe_view->user_properties, subscribe_view->user_property_count);

    /*
     * Add the length of 1 byte for the identifier of a Subscription Identifier property
     * and the VLI of the subscription_identifier itself
     */
    if (subscribe_view->subscription_identifier != 0) {
        size_t subscription_identifier_length = 0;
        aws_mqtt5_get_variable_length_encode_size(
            *subscribe_view->subscription_identifier, &subscription_identifier_length);
        subscribe_variable_header_property_length += subscription_identifier_length + 1;
    }

    *subscribe_properties_length = subscribe_variable_header_property_length;

    /* variable header total length =
     *    2 bytes for Packet Identifier
     *  + # bytes (variable_length_encoding(subscribe_variable_header_property_length))
     *  + subscribe_variable_header_property_length
     */
    size_t variable_header_length = 0;
    if (aws_mqtt5_get_variable_length_encode_size(subscribe_variable_header_property_length, &variable_header_length)) {
        return AWS_OP_ERR;
    }
    variable_header_length += 2 + subscribe_variable_header_property_length;

    size_t payload_length = 0;

    /*
     *  for each subscription view, in addition to the raw name-value bytes, we also have 2 bytes of
     *  prefix and one byte suffix required.
     *   2 bytes for the Topic Filter length
     *   1 byte for the Subscription Options Flags
     */

    for (size_t i = 0; i < subscribe_view->subscription_count; ++i) {
        const struct aws_mqtt5_subscription_view *subscription = &subscribe_view->subscriptions[i];
        payload_length += subscription->topic_filter.len;
    }
    payload_length += (3 * subscribe_view->subscription_count);

    *total_remaining_length = variable_header_length + payload_length;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_encoder_begin_subscribe(struct aws_mqtt5_encoder *encoder, const void *view) {

    const struct aws_mqtt5_packet_subscribe_view *subscription_view = view;

    size_t total_remaining_length = 0;
    size_t subscribe_properties_length = 0;

    if (s_compute_subscribe_variable_length_fields(
            subscription_view, &total_remaining_length, &subscribe_properties_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) mqtt5 client encoder - failed to compute variable length values for SUBSCRIBE packet with error "
            "%d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_GENERAL,
        "(%p) mqtt5 client encoder - setting up encode for a SUBSCRIBE packet with remaining length %zu",
        (void *)encoder->config.client,
        total_remaining_length);

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t subscribe_property_length_u32 = (uint32_t)subscribe_properties_length;

    /*
     * Fixed Header
     * byte 1:
     *  bits 7-4 MQTT Control Packet Type
     *  bits 3-0 Reserved, must be set to 0, 0, 1, 0
     * byte 2-x: Remaining Length as Variable Byte Integer (1-4 bytes)
     */
    ADD_ENCODE_STEP_U8(
        encoder,
        aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_SUBSCRIBE, SUBSCRIBE_PACKET_FIXED_HEADER_RESERVED_BITS));
    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);

    /*
     * Variable Header
     * byte 1-2: Packet Identifier
     * byte 3-x: Property Length as Variable Byte Integer (1-4 bytes)
     */
    ADD_ENCODE_STEP_U16(encoder, (uint16_t)subscription_view->packet_id);
    ADD_ENCODE_STEP_VLI(encoder, subscribe_property_length_u32);

    /*
     * Subscribe Properties
     * (optional) Subscription Identifier
     * (optional) User Properties
     */
    if (subscription_view->subscription_identifier != 0) {
        ADD_ENCODE_STEP_U8(encoder, AWS_MQTT5_PROPERTY_TYPE_SUBSCRIPTION_IDENTIFIER);
        ADD_ENCODE_STEP_VLI(encoder, *subscription_view->subscription_identifier);
    }

    aws_mqtt5_add_user_property_encoding_steps(
        encoder, subscription_view->user_properties, subscription_view->user_property_count);

    /*
     * Payload
     * n Topic Filters
     *  byte 1-2: Length
     *  byte 3..N: UTF-8 encoded Topic Filter
     *  byte N+1:
     *      bits 7-6 Reserved
     *      bits 5-4 Retain Handling
     *      bit 3    Retain as Published
     *      bit 2    No Local
     *      bits 1-0 Maximum QoS
     */
    aws_mqtt5_add_subscribe_topic_filter_encoding_steps(
        encoder, subscription_view->subscriptions, subscription_view->subscription_count);

    return AWS_OP_SUCCESS;
}

static int s_compute_unsubscribe_variable_length_fields(
    const struct aws_mqtt5_packet_unsubscribe_view *unsubscribe_view,
    size_t *total_remaining_length,
    size_t *unsubscribe_properties_length) {

    size_t unsubscribe_variable_header_property_length = aws_mqtt5_compute_user_property_encode_length(
        unsubscribe_view->user_properties, unsubscribe_view->user_property_count);

    *unsubscribe_properties_length = unsubscribe_variable_header_property_length;

    /* variable header total length =
     *    2 bytes for Packet Identifier
     *  + # bytes (variable_length_encoding(subscribe_variable_header_property_length))
     *  + unsubscribe_variable_header_property_length
     */
    size_t variable_header_length = 0;
    if (aws_mqtt5_get_variable_length_encode_size(
            unsubscribe_variable_header_property_length, &variable_header_length)) {
        return AWS_OP_ERR;
    }
    variable_header_length += 2 + unsubscribe_variable_header_property_length;

    size_t payload_length = 0;

    /*
     *  for each unsubscribe topic filter
     *   2 bytes for the Topic Filter length
     *   n bytes for Topic Filter
     */

    for (size_t i = 0; i < unsubscribe_view->topic_filter_count; ++i) {
        const struct aws_byte_cursor topic_filter = unsubscribe_view->topic_filters[i];
        payload_length += topic_filter.len;
    }

    payload_length += (2 * unsubscribe_view->topic_filter_count);

    *total_remaining_length = variable_header_length + payload_length;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_encoder_begin_unsubscribe(struct aws_mqtt5_encoder *encoder, const void *view) {

    const struct aws_mqtt5_packet_unsubscribe_view *unsubscribe_view = view;

    size_t total_remaining_length = 0;
    size_t unsubscribe_properties_length = 0;

    if (s_compute_unsubscribe_variable_length_fields(
            unsubscribe_view, &total_remaining_length, &unsubscribe_properties_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) mqtt5 client encoder - failed to compute variable length values for UNSUBSCRIBE packet with error "
            "%d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_GENERAL,
        "(%p) mqtt5 client encoder - setting up encode for a UNSUBSCRIBE packet with remaining length %zu",
        (void *)encoder->config.client,
        total_remaining_length);

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t unsubscribe_property_length_u32 = (uint32_t)unsubscribe_properties_length;

    /*
     * Fixed Header
     * byte 1:
     *  bits 7-4 MQTT Control Packet type (10)
     *  bits 3-0 Reserved, must be set to 0, 0, 1, 0
     * byte 2-x: Remaining Length as Variable Byte Integer (1-4 bytes)
     */
    ADD_ENCODE_STEP_U8(
        encoder,
        aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_UNSUBSCRIBE, UNSUBSCRIBE_PACKET_FIXED_HEADER_RESERVED_BITS));
    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);

    /*
     * Variable Header
     * byte 1-2: Packet Identifier
     * byte 3-x: Properties length as Variable Byte Integer (1-4 bytes)
     */
    ADD_ENCODE_STEP_U16(encoder, (uint16_t)unsubscribe_view->packet_id);
    ADD_ENCODE_STEP_VLI(encoder, unsubscribe_property_length_u32);

    /*
     * (optional) User Properties
     */
    aws_mqtt5_add_user_property_encoding_steps(
        encoder, unsubscribe_view->user_properties, unsubscribe_view->user_property_count);

    /*
     * Payload
     * n Topic Filters
     *  byte 1-2: Length
     *  byte 3..N: UTF-8 encoded Topic Filter
     */

    aws_mqtt5_add_unsubscribe_topic_filter_encoding_steps(
        encoder, unsubscribe_view->topic_filters, unsubscribe_view->topic_filter_count);

    return AWS_OP_SUCCESS;
}

static int s_compute_publish_variable_length_fields(
    const struct aws_mqtt5_packet_publish_view *publish_view,
    size_t *total_remaining_length,
    size_t *publish_properties_length) {

    size_t publish_property_section_length =
        aws_mqtt5_compute_user_property_encode_length(publish_view->user_properties, publish_view->user_property_count);

    ADD_OPTIONAL_U8_PROPERTY_LENGTH(publish_view->payload_format, publish_property_section_length);
    ADD_OPTIONAL_U32_PROPERTY_LENGTH(publish_view->message_expiry_interval_seconds, publish_property_section_length);
    ADD_OPTIONAL_U16_PROPERTY_LENGTH(publish_view->topic_alias, publish_property_section_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->response_topic, publish_property_section_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->correlation_data, publish_property_section_length);
    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(publish_view->content_type, publish_property_section_length);

    for (size_t i = 0; i < publish_view->subscription_identifier_count; ++i) {
        size_t encoding_size = 0;
        if (aws_mqtt5_get_variable_length_encode_size(publish_view->subscription_identifiers[i], &encoding_size)) {
            return AWS_OP_ERR;
        }
        publish_property_section_length += 1 + encoding_size;
    }

    *publish_properties_length = (uint32_t)publish_property_section_length;

    /*
     * Remaining Length:
     * Variable Header
     *  - Topic Name
     *  - Packet Identifier
     *  - Property Length as VLI x
     *  - All Properties x
     * Payload
     */

    size_t remaining_length = 0;

    /* Property Length VLI size */
    if (aws_mqtt5_get_variable_length_encode_size(publish_property_section_length, &remaining_length)) {
        return AWS_OP_ERR;
    }

    /* Topic name */
    remaining_length += 2 + publish_view->topic.len;

    /* Optional packet id */
    if (publish_view->packet_id != 0) {
        remaining_length += 2;
    }

    /* Properties */
    remaining_length += publish_property_section_length;

    /* Payload */
    remaining_length += publish_view->payload.len;

    *total_remaining_length = remaining_length;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_encoder_begin_publish(struct aws_mqtt5_encoder *encoder, const void *view) {

    /* We do a shallow copy of the stored view in order to temporarily side affect it for topic aliasing */
    struct aws_mqtt5_packet_publish_view local_publish_view = *((const struct aws_mqtt5_packet_publish_view *)view);

    uint16_t outbound_topic_alias = 0;
    struct aws_byte_cursor outbound_topic;

    if (encoder->topic_alias_resolver != NULL) {
        AWS_ZERO_STRUCT(outbound_topic);
        if (aws_mqtt5_outbound_topic_alias_resolver_resolve_outbound_publish(
                encoder->topic_alias_resolver, &local_publish_view, &outbound_topic_alias, &outbound_topic)) {
            int error_code = aws_last_error();
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_GENERAL,
                "(%p) mqtt5 client encoder - failed to perform outbound topic alias resolution on PUBLISH packet with "
                "error "
                "%d(%s)",
                (void *)encoder->config.client,
                error_code,
                aws_error_debug_str(error_code));
            return AWS_OP_ERR;
        }

        local_publish_view.topic = outbound_topic;
        if (outbound_topic_alias != 0) {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_GENERAL,
                "(%p) mqtt5 client encoder - PUBLISH packet using topic alias value %" PRIu16,
                (void *)encoder->config.client,
                outbound_topic_alias);
            if (outbound_topic.len == 0) {
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT5_GENERAL,
                    "(%p) mqtt5 client encoder - PUBLISH packet dropping topic field for previously established alias",
                    (void *)encoder->config.client);
            }
            local_publish_view.topic_alias = &outbound_topic_alias;
        } else {
            AWS_FATAL_ASSERT(local_publish_view.topic.len > 0);
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_GENERAL,
                "(%p) mqtt5 client encoder - PUBLISH packet not using a topic alias",
                (void *)encoder->config.client);
            local_publish_view.topic_alias = NULL;
        }
    }

    /*
     * We're going to encode the local mutated view copy, not the stored view.  This lets the original packet stay
     * unchanged for the entire time it is owned by the client.  Otherwise, events that disrupt the alias cache
     * (like disconnections) would make correct aliasing impossible (because we'd have mutated and potentially lost
     * topic information).
     */
    const struct aws_mqtt5_packet_publish_view *publish_view = &local_publish_view;

    size_t total_remaining_length = 0;
    size_t publish_properties_length = 0;

    if (s_compute_publish_variable_length_fields(publish_view, &total_remaining_length, &publish_properties_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) mqtt5 client encoder - failed to compute variable length values for PUBLISH packet with error "
            "%d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_GENERAL,
        "(%p) mqtt5 client encoder - setting up encode for a PUBLISH packet with remaining length %zu",
        (void *)encoder->config.client,
        total_remaining_length);

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t publish_property_length_u32 = (uint32_t)publish_properties_length;

    /*
     * Fixed Header
     * byte 1:
     *  bits 4-7: MQTT Control Packet Type
     *  bit 3: DUP flag
     *  bit 1-2: QoS level
     *  bit 0: RETAIN
     * byte 2-x: Remaining Length as Variable Byte Integer (1-4 bytes)
     */

    uint8_t flags = 0;

    if (publish_view->duplicate) {
        flags |= 1 << 3;
    }

    flags |= ((uint8_t)publish_view->qos) << 1;

    if (publish_view->retain) {
        flags |= 1;
    }

    ADD_ENCODE_STEP_U8(encoder, aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_PUBLISH, flags));

    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);

    /*
     * Variable Header
     * UTF-8 Encoded Topic Name
     * 2 byte Packet Identifier
     * 1-4 byte Property Length as Variable Byte Integer
     * n bytes Properties
     */

    ADD_ENCODE_STEP_LENGTH_PREFIXED_CURSOR(encoder, publish_view->topic);
    if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {
        ADD_ENCODE_STEP_U16(encoder, (uint16_t)publish_view->packet_id);
    }
    ADD_ENCODE_STEP_VLI(encoder, publish_property_length_u32);

    ADD_ENCODE_STEP_OPTIONAL_U8_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_PAYLOAD_FORMAT_INDICATOR, publish_view->payload_format);
    ADD_ENCODE_STEP_OPTIONAL_U32_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_MESSAGE_EXPIRY_INTERVAL, publish_view->message_expiry_interval_seconds);
    ADD_ENCODE_STEP_OPTIONAL_U16_PROPERTY(encoder, AWS_MQTT5_PROPERTY_TYPE_TOPIC_ALIAS, publish_view->topic_alias);
    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_RESPONSE_TOPIC, publish_view->response_topic);
    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_CORRELATION_DATA, publish_view->correlation_data);

    for (size_t i = 0; i < publish_view->subscription_identifier_count; ++i) {
        ADD_ENCODE_STEP_OPTIONAL_VLI_PROPERTY(
            encoder, AWS_MQTT5_PROPERTY_TYPE_SUBSCRIPTION_IDENTIFIER, &publish_view->subscription_identifiers[i]);
    }

    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(encoder, AWS_MQTT5_PROPERTY_TYPE_CONTENT_TYPE, publish_view->content_type);

    aws_mqtt5_add_user_property_encoding_steps(
        encoder, publish_view->user_properties, publish_view->user_property_count);

    /*
     * Payload
     * Content and format of data is application specific
     */
    if (publish_view->payload.len > 0) {
        ADD_ENCODE_STEP_CURSOR(encoder, publish_view->payload);
    }

    return AWS_OP_SUCCESS;
}

static int s_compute_puback_variable_length_fields(
    const struct aws_mqtt5_packet_puback_view *puback_view,
    size_t *total_remaining_length,
    size_t *puback_properties_length) {

    size_t local_property_length =
        aws_mqtt5_compute_user_property_encode_length(puback_view->user_properties, puback_view->user_property_count);

    ADD_OPTIONAL_CURSOR_PROPERTY_LENGTH(puback_view->reason_string, local_property_length);

    *puback_properties_length = (uint32_t)local_property_length;

    /* variable header total length =
     *    2 bytes for Packet Identifier
     *  + 1 byte for PUBACK reason code if it exists
     *  + subscribe_variable_header_property_length
     *
     * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124
     * If there are no properties and Reason Code is success, PUBACK ends with the packet id
     *
     * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124
     * If there are no properties and Reason Code is not success, PUBACK ends with the reason code
     */
    if (local_property_length == 0) {
        if (puback_view->reason_code == AWS_MQTT5_PARC_SUCCESS) {
            *total_remaining_length = 2;
        } else {
            *total_remaining_length = 3;
        }
        return AWS_OP_SUCCESS;
    }

    size_t variable_property_length_size = 0;
    if (aws_mqtt5_get_variable_length_encode_size(local_property_length, &variable_property_length_size)) {
        return AWS_OP_ERR;
    }
    /* vli of property length + packet id + reason code + properties length */
    *total_remaining_length = variable_property_length_size + 3 + local_property_length;

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_encoder_begin_puback(struct aws_mqtt5_encoder *encoder, const void *view) {
    const struct aws_mqtt5_packet_puback_view *puback_view = view;

    size_t total_remaining_length = 0;
    size_t puback_properties_length = 0;

    if (s_compute_puback_variable_length_fields(puback_view, &total_remaining_length, &puback_properties_length)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) mqtt5 client encoder - failed to compute variable length values for PUBACK packet with error "
            "%d(%s)",
            (void *)encoder->config.client,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_GENERAL,
        "(%p) mqtt5 client encoder - setting up encode for a PUBACK packet with remaining length %zu",
        (void *)encoder->config.client,
        total_remaining_length);

    uint32_t total_remaining_length_u32 = (uint32_t)total_remaining_length;
    uint32_t puback_property_length_u32 = (uint32_t)puback_properties_length;

    /*
     * Fixed Header
     * byte 1:
     *  bits 7-4 MQTT Control Packet Type
     *  bits 3-0 Reserved, bust be set to 0, 0, 0, 0
     * byte 2-x: Remaining Length as a Variable Byte Integer (1-4 bytes)
     */

    ADD_ENCODE_STEP_U8(encoder, aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_PUBACK, 0));
    ADD_ENCODE_STEP_VLI(encoder, total_remaining_length_u32);

    /*
     * Variable Header
     * byte 1-2: Packet Identifier
     * byte 3: PUBACK Reason Code
     * byte 4-x: Property Length
     * Properties
     */
    ADD_ENCODE_STEP_U16(encoder, (uint16_t)puback_view->packet_id);
    /*
     * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124
     * If Reason Code is success and there are no properties, PUBACK ends with the packet id
     */
    if (total_remaining_length == 2) {
        return AWS_OP_SUCCESS;
    }

    ADD_ENCODE_STEP_U8(encoder, puback_view->reason_code);

    /*
     * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901126
     * If remaining length < 4 there is no property length
     */
    if (total_remaining_length < 4) {
        return AWS_OP_SUCCESS;
    }

    ADD_ENCODE_STEP_VLI(encoder, puback_property_length_u32);
    ADD_ENCODE_STEP_OPTIONAL_CURSOR_PROPERTY(
        encoder, AWS_MQTT5_PROPERTY_TYPE_REASON_STRING, puback_view->reason_string);
    aws_mqtt5_add_user_property_encoding_steps(encoder, puback_view->user_properties, puback_view->user_property_count);

    return AWS_OP_SUCCESS;
}

static enum aws_mqtt5_encoding_result s_execute_encode_step(
    struct aws_mqtt5_encoder *encoder,
    struct aws_mqtt5_encoding_step *step,
    struct aws_byte_buf *buffer) {
    size_t buffer_room = buffer->capacity - buffer->len;

    switch (step->type) {
        case AWS_MQTT5_EST_U8:
            if (buffer_room < 1) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            aws_byte_buf_write_u8(buffer, step->value.value_u8);

            return AWS_MQTT5_ER_FINISHED;

        case AWS_MQTT5_EST_U16:
            if (buffer_room < 2) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            aws_byte_buf_write_be16(buffer, step->value.value_u16);

            return AWS_MQTT5_ER_FINISHED;

        case AWS_MQTT5_EST_U32:
            if (buffer_room < 4) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            aws_byte_buf_write_be32(buffer, step->value.value_u32);

            return AWS_MQTT5_ER_FINISHED;

        case AWS_MQTT5_EST_VLI:
            /* being lazy here and just assuming the worst case */
            if (buffer_room < 4) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            /* This can't fail.  We've already validated the vli value when we made the step */
            aws_mqtt5_encode_variable_length_integer(buffer, step->value.value_u32);

            return AWS_MQTT5_ER_FINISHED;

        case AWS_MQTT5_EST_CURSOR:
            if (buffer_room < 1) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            aws_byte_buf_write_to_capacity(buffer, &step->value.value_cursor);

            return (step->value.value_cursor.len == 0) ? AWS_MQTT5_ER_FINISHED : AWS_MQTT5_ER_OUT_OF_ROOM;

        case AWS_MQTT5_EST_STREAM:
            while (buffer->len < buffer->capacity) {
                if (aws_input_stream_read(step->value.value_stream, buffer)) {
                    int error_code = aws_last_error();
                    AWS_LOGF_ERROR(
                        AWS_LS_MQTT5_CLIENT,
                        "id=%p: failed to read from stream with error %d(%s)",
                        (void *)encoder->config.client,
                        error_code,
                        aws_error_debug_str(error_code));
                    return AWS_MQTT5_ER_ERROR;
                }

                struct aws_stream_status status;
                if (aws_input_stream_get_status(step->value.value_stream, &status)) {
                    int error_code = aws_last_error();
                    AWS_LOGF_ERROR(
                        AWS_LS_MQTT5_CLIENT,
                        "id=%p: failed to query stream status with error %d(%s)",
                        (void *)encoder->config.client,
                        error_code,
                        aws_error_debug_str(error_code));
                    return AWS_MQTT5_ER_ERROR;
                }

                if (status.is_end_of_stream) {
                    return AWS_MQTT5_ER_FINISHED;
                }
            }

            if (buffer->len == buffer->capacity) {
                return AWS_MQTT5_ER_OUT_OF_ROOM;
            }

            /* fall through intentional */
    }

    /* shouldn't be reachable */
    AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: encoder reached an unreachable state", (void *)encoder->config.client);
    aws_raise_error(AWS_ERROR_INVALID_STATE);
    return AWS_MQTT5_ER_ERROR;
}

enum aws_mqtt5_encoding_result aws_mqtt5_encoder_encode_to_buffer(
    struct aws_mqtt5_encoder *encoder,
    struct aws_byte_buf *buffer) {

    enum aws_mqtt5_encoding_result result = AWS_MQTT5_ER_FINISHED;
    size_t step_count = aws_array_list_length(&encoder->encoding_steps);
    while (result == AWS_MQTT5_ER_FINISHED && encoder->current_encoding_step_index < step_count) {
        struct aws_mqtt5_encoding_step *step = NULL;
        aws_array_list_get_at_ptr(&encoder->encoding_steps, (void **)&step, encoder->current_encoding_step_index);

        result = s_execute_encode_step(encoder, step, buffer);
        if (result == AWS_MQTT5_ER_FINISHED) {
            encoder->current_encoding_step_index++;
        }
    }

    if (result == AWS_MQTT5_ER_FINISHED) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_CLIENT, "id=%p: finished encoding current operation", (void *)encoder->config.client);
        aws_mqtt5_encoder_reset(encoder);
    }

    return result;
}

static struct aws_mqtt5_encoder_function_table s_aws_mqtt5_encoder_default_function_table = {
    .encoders_by_packet_type =
        {
            NULL,                                   /* RESERVED = 0 */
            &s_aws_mqtt5_encoder_begin_connect,     /* CONNECT */
            NULL,                                   /* CONNACK */
            &s_aws_mqtt5_encoder_begin_publish,     /* PUBLISH */
            &s_aws_mqtt5_encoder_begin_puback,      /* PUBACK */
            NULL,                                   /* PUBREC */
            NULL,                                   /* PUBREL */
            NULL,                                   /* PUBCOMP */
            &s_aws_mqtt5_encoder_begin_subscribe,   /* SUBSCRIBE */
            NULL,                                   /* SUBACK */
            &s_aws_mqtt5_encoder_begin_unsubscribe, /* UNSUBSCRIBE */
            NULL,                                   /* UNSUBACK */
            &s_aws_mqtt5_encoder_begin_pingreq,     /* PINGREQ */
            NULL,                                   /* PINGRESP */
            &s_aws_mqtt5_encoder_begin_disconnect,  /* DISCONNECT */
            NULL                                    /* AUTH */
        },
};

const struct aws_mqtt5_encoder_function_table *g_aws_mqtt5_encoder_default_function_table =
    &s_aws_mqtt5_encoder_default_function_table;

int aws_mqtt5_encoder_init(
    struct aws_mqtt5_encoder *encoder,
    struct aws_allocator *allocator,
    struct aws_mqtt5_encoder_options *options) {
    AWS_ZERO_STRUCT(*encoder);

    encoder->config = *options;
    if (encoder->config.encoders == NULL) {
        encoder->config.encoders = &s_aws_mqtt5_encoder_default_function_table;
    }

    if (aws_array_list_init_dynamic(
            &encoder->encoding_steps, allocator, INITIAL_ENCODING_STEP_COUNT, sizeof(struct aws_mqtt5_encoding_step))) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_encoder_clean_up(struct aws_mqtt5_encoder *encoder) {
    aws_array_list_clean_up(&encoder->encoding_steps);
}

void aws_mqtt5_encoder_reset(struct aws_mqtt5_encoder *encoder) {
    aws_array_list_clear(&encoder->encoding_steps);
    encoder->current_encoding_step_index = 0;
}

int aws_mqtt5_encoder_append_packet_encoding(
    struct aws_mqtt5_encoder *encoder,
    enum aws_mqtt5_packet_type packet_type,
    const void *packet_view) {
    aws_mqtt5_encode_begin_packet_type_fn *encoding_fn = encoder->config.encoders->encoders_by_packet_type[packet_type];
    if (encoding_fn == NULL) {
        return aws_raise_error(AWS_ERROR_MQTT5_ENCODE_FAILURE);
    }

    return (*encoding_fn)(encoder, packet_view);
}

static int s_compute_packet_size(size_t total_remaining_length, size_t *packet_size) {
    /* 1 (packet type + flags) + vli_length(total_remaining_length) + total_remaining_length */
    size_t encode_size = 0;
    if (aws_mqtt5_get_variable_length_encode_size(total_remaining_length, &encode_size)) {
        return AWS_OP_ERR;
    }

    size_t prefix = (size_t)1 + encode_size;

    if (aws_add_size_checked(prefix, total_remaining_length, packet_size)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_packet_view_get_encoded_size(
    enum aws_mqtt5_packet_type packet_type,
    const void *packet_view,
    size_t *packet_size) {
    size_t total_remaining_length = 0;
    size_t properties_length = 0;

    if (packet_type == AWS_MQTT5_PT_PINGREQ) {
        *packet_size = AWS_MQTT5_PINGREQ_ENCODED_SIZE;
        return AWS_OP_SUCCESS;
    }

    switch (packet_type) {
        case AWS_MQTT5_PT_PUBLISH:
            if (s_compute_publish_variable_length_fields(packet_view, &total_remaining_length, &properties_length)) {
                return AWS_OP_ERR;
            }
            break;

        case AWS_MQTT5_PT_SUBSCRIBE:
            if (s_compute_subscribe_variable_length_fields(packet_view, &total_remaining_length, &properties_length)) {
                return AWS_OP_ERR;
            }
            break;

        case AWS_MQTT5_PT_UNSUBSCRIBE:
            if (s_compute_unsubscribe_variable_length_fields(
                    packet_view, &total_remaining_length, &properties_length)) {
                return AWS_OP_ERR;
            }
            break;

        case AWS_MQTT5_PT_DISCONNECT:
            if (s_compute_disconnect_variable_length_fields(packet_view, &total_remaining_length, &properties_length)) {
                return AWS_OP_ERR;
            }
            break;

        case AWS_MQTT5_PT_PUBACK:
            if (s_compute_puback_variable_length_fields(packet_view, &total_remaining_length, &properties_length)) {
                return AWS_OP_ERR;
            }
            break;

        default:
            return aws_raise_error(AWS_ERROR_MQTT5_ENCODE_SIZE_UNSUPPORTED_PACKET_TYPE);
    }

    return s_compute_packet_size(total_remaining_length, packet_size);
}

void aws_mqtt5_encoder_set_outbound_topic_alias_resolver(
    struct aws_mqtt5_encoder *encoder,
    struct aws_mqtt5_outbound_topic_alias_resolver *resolver) {

    encoder->topic_alias_resolver = resolver;
}
