/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/v5/mqtt5_decoder.h>

#include <aws/mqtt/private/v5/mqtt5_topic_alias.h>
#include <aws/mqtt/private/v5/mqtt5_utils.h>

#define AWS_MQTT5_DECODER_BUFFER_START_SIZE 2048
#define PUBLISH_PACKET_FIXED_HEADER_DUPLICATE_FLAG 8
#define PUBLISH_PACKET_FIXED_HEADER_RETAIN_FLAG 1
#define PUBLISH_PACKET_FIXED_HEADER_QOS_FLAG 3

static void s_reset_decoder_for_new_packet(struct aws_mqtt5_decoder *decoder) {
    aws_byte_buf_reset(&decoder->scratch_space, false);

    decoder->packet_first_byte = 0;
    decoder->remaining_length = 0;
    AWS_ZERO_STRUCT(decoder->packet_cursor);
}

static void s_enter_state(struct aws_mqtt5_decoder *decoder, enum aws_mqtt5_decoder_state state) {
    decoder->state = state;

    if (state == AWS_MQTT5_DS_READ_PACKET_TYPE) {
        s_reset_decoder_for_new_packet(decoder);
    } else {
        aws_byte_buf_reset(&decoder->scratch_space, false);
    }
}

static bool s_is_decodable_packet_type(struct aws_mqtt5_decoder *decoder, enum aws_mqtt5_packet_type type) {
    return (uint32_t)type < AWS_ARRAY_SIZE(decoder->options.decoder_table->decoders_by_packet_type) &&
           decoder->options.decoder_table->decoders_by_packet_type[type] != NULL;
}

/*
 * Every mqtt packet has a first byte that, amongst other things, determines the packet type
 */
static int s_aws_mqtt5_decoder_read_packet_type_on_data(
    struct aws_mqtt5_decoder *decoder,
    struct aws_byte_cursor *data) {

    if (data->len == 0) {
        return AWS_MQTT5_DRT_MORE_DATA;
    }

    uint8_t byte = *data->ptr;
    aws_byte_cursor_advance(data, 1);
    aws_byte_buf_append_byte_dynamic(&decoder->scratch_space, byte);

    enum aws_mqtt5_packet_type packet_type = (byte >> 4);

    if (!s_is_decodable_packet_type(decoder, packet_type)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: unsupported or illegal packet type value: %d",
            decoder->options.callback_user_data,
            (int)packet_type);
        return AWS_MQTT5_DRT_ERROR;
    }

    decoder->packet_first_byte = byte;

    s_enter_state(decoder, AWS_MQTT5_DS_READ_REMAINING_LENGTH);

    return AWS_MQTT5_DRT_SUCCESS;
}

/*
 * non-streaming variable length integer decode.  cursor is updated only if the value was successfully read
 */
enum aws_mqtt5_decode_result_type aws_mqtt5_decode_vli(struct aws_byte_cursor *cursor, uint32_t *dest) {
    uint32_t value = 0;
    bool more_data = false;
    size_t bytes_used = 0;

    uint32_t shift = 0;

    struct aws_byte_cursor cursor_copy = *cursor;
    for (; bytes_used < 4; ++bytes_used) {
        uint8_t byte = 0;
        if (!aws_byte_cursor_read_u8(&cursor_copy, &byte)) {
            return AWS_MQTT5_DRT_MORE_DATA;
        }

        value |= ((byte & 0x7F) << shift);
        shift += 7;

        more_data = (byte & 0x80) != 0;
        if (!more_data) {
            break;
        }
    }

    if (more_data) {
        /* A variable length integer with the 4th byte high bit set is not valid */
        AWS_LOGF_ERROR(AWS_LS_MQTT5_GENERAL, "(static) aws_mqtt5_decoder - illegal variable length integer encoding");
        return AWS_MQTT5_DRT_ERROR;
    }

    aws_byte_cursor_advance(cursor, bytes_used + 1);
    *dest = value;

    return AWS_MQTT5_DRT_SUCCESS;
}

/* "streaming" variable length integer decode */
static enum aws_mqtt5_decode_result_type s_aws_mqtt5_decoder_read_vli_on_data(
    struct aws_mqtt5_decoder *decoder,
    uint32_t *vli_dest,
    struct aws_byte_cursor *data) {

    enum aws_mqtt5_decode_result_type decode_vli_result = AWS_MQTT5_DRT_MORE_DATA;

    /* try to decode the vli integer one byte at a time */
    while (data->len > 0 && decode_vli_result == AWS_MQTT5_DRT_MORE_DATA) {
        /* append a single byte to the scratch buffer */
        struct aws_byte_cursor byte_cursor = aws_byte_cursor_advance(data, 1);
        aws_byte_buf_append_dynamic(&decoder->scratch_space, &byte_cursor);

        /* now try and decode a vli integer based on the range implied by the offset into the buffer */
        struct aws_byte_cursor vli_cursor = {
            .ptr = decoder->scratch_space.buffer,
            .len = decoder->scratch_space.len,
        };

        decode_vli_result = aws_mqtt5_decode_vli(&vli_cursor, vli_dest);
    }

    return decode_vli_result;
}

/* attempts to read the variable length integer that is always the second piece of data in an mqtt packet */
static enum aws_mqtt5_decode_result_type s_aws_mqtt5_decoder_read_remaining_length_on_data(
    struct aws_mqtt5_decoder *decoder,
    struct aws_byte_cursor *data) {

    enum aws_mqtt5_decode_result_type result =
        s_aws_mqtt5_decoder_read_vli_on_data(decoder, &decoder->remaining_length, data);
    if (result != AWS_MQTT5_DRT_SUCCESS) {
        return result;
    }

    s_enter_state(decoder, AWS_MQTT5_DS_READ_PACKET);

    return AWS_MQTT5_DRT_SUCCESS;
}

/* non-streaming decode of a user property; failure implies connection termination */
int aws_mqtt5_decode_user_property(
    struct aws_byte_cursor *packet_cursor,
    struct aws_mqtt5_user_property_set *properties) {
    struct aws_mqtt5_user_property property;

    AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR(packet_cursor, &property.name, error);
    AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR(packet_cursor, &property.value, error);

    if (aws_array_list_push_back(&properties->properties, &property)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;

error:

    return AWS_OP_ERR;
}

/* decode function for all CONNACK properties */
static int s_read_connack_property(
    struct aws_mqtt5_packet_connack_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_connack_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_SESSION_EXPIRY_INTERVAL:
            AWS_MQTT5_DECODE_U32_OPTIONAL(
                packet_cursor, &storage->session_expiry_interval, &storage_view->session_expiry_interval, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_RECEIVE_MAXIMUM:
            AWS_MQTT5_DECODE_U16_OPTIONAL(
                packet_cursor, &storage->receive_maximum, &storage_view->receive_maximum, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_MAXIMUM_QOS:
            AWS_MQTT5_DECODE_U8_OPTIONAL(packet_cursor, &storage->maximum_qos, &storage_view->maximum_qos, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_RETAIN_AVAILABLE:
            AWS_MQTT5_DECODE_U8_OPTIONAL(
                packet_cursor, &storage->retain_available, &storage_view->retain_available, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_MAXIMUM_PACKET_SIZE:
            AWS_MQTT5_DECODE_U32_OPTIONAL(
                packet_cursor, &storage->maximum_packet_size, &storage_view->maximum_packet_size, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_ASSIGNED_CLIENT_IDENTIFIER:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->assigned_client_identifier, &storage_view->assigned_client_identifier, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_TOPIC_ALIAS_MAXIMUM:
            AWS_MQTT5_DECODE_U16_OPTIONAL(
                packet_cursor, &storage->topic_alias_maximum, &storage_view->topic_alias_maximum, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_REASON_STRING:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->reason_string, &storage_view->reason_string, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_WILDCARD_SUBSCRIPTIONS_AVAILABLE:
            AWS_MQTT5_DECODE_U8_OPTIONAL(
                packet_cursor,
                &storage->wildcard_subscriptions_available,
                &storage_view->wildcard_subscriptions_available,
                done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SUBSCRIPTION_IDENTIFIERS_AVAILABLE:
            AWS_MQTT5_DECODE_U8_OPTIONAL(
                packet_cursor,
                &storage->subscription_identifiers_available,
                &storage_view->subscription_identifiers_available,
                done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SHARED_SUBSCRIPTIONS_AVAILABLE:
            AWS_MQTT5_DECODE_U8_OPTIONAL(
                packet_cursor,
                &storage->shared_subscriptions_available,
                &storage_view->shared_subscriptions_available,
                done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SERVER_KEEP_ALIVE:
            AWS_MQTT5_DECODE_U16_OPTIONAL(
                packet_cursor, &storage->server_keep_alive, &storage_view->server_keep_alive, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_RESPONSE_INFORMATION:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->response_information, &storage_view->response_information, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SERVER_REFERENCE:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->server_reference, &storage_view->server_reference, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_AUTHENTICATION_METHOD:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->authentication_method, &storage_view->authentication_method, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_AUTHENTICATION_DATA:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->authentication_data, &storage_view->authentication_data, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read CONNACK property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes a CONNACK packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_connack(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_connack_storage storage;
    if (aws_mqtt5_packet_connack_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }

    int result = AWS_OP_ERR;

    uint8_t first_byte = decoder->packet_first_byte;
    /* CONNACK flags must be zero by protocol */
    if ((first_byte & 0x0F) != 0) {
        goto done;
    }

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;
    uint32_t remaining_length = decoder->remaining_length;
    if (remaining_length != (uint32_t)packet_cursor.len) {
        goto done;
    }

    uint8_t connect_flags = 0;
    AWS_MQTT5_DECODE_U8(&packet_cursor, &connect_flags, done);

    /* everything but the 0-bit must be 0 */
    if ((connect_flags & 0xFE) != 0) {
        goto done;
    }

    struct aws_mqtt5_packet_connack_view *storage_view = &storage.storage_view;

    storage_view->session_present = (connect_flags & 0x01) != 0;

    uint8_t reason_code = 0;
    AWS_MQTT5_DECODE_U8(&packet_cursor, &reason_code, done);
    storage_view->reason_code = reason_code;

    uint32_t property_length = 0;
    AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
    if (property_length != (uint32_t)packet_cursor.len) {
        goto done;
    }

    while (packet_cursor.len > 0) {
        if (s_read_connack_property(&storage, &packet_cursor)) {
            goto done;
        }
    }

    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_CONNACK, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: CONNACK decode failure", decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_connack_storage_clean_up(&storage);

    return result;
}

/* decode function for all PUBLISH properties */
static int s_read_publish_property(
    struct aws_mqtt5_packet_publish_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_publish_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_PAYLOAD_FORMAT_INDICATOR:
            AWS_MQTT5_DECODE_U8_OPTIONAL(packet_cursor, &storage->payload_format, &storage_view->payload_format, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_MESSAGE_EXPIRY_INTERVAL:
            AWS_MQTT5_DECODE_U32_OPTIONAL(
                packet_cursor,
                &storage->message_expiry_interval_seconds,
                &storage_view->message_expiry_interval_seconds,
                done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_TOPIC_ALIAS:
            AWS_MQTT5_DECODE_U16_OPTIONAL(packet_cursor, &storage->topic_alias, &storage_view->topic_alias, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_RESPONSE_TOPIC:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->response_topic, &storage_view->response_topic, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_CORRELATION_DATA:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->correlation_data, &storage_view->correlation_data, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SUBSCRIPTION_IDENTIFIER: {
            uint32_t subscription_identifier = 0;
            AWS_MQTT5_DECODE_VLI(packet_cursor, &subscription_identifier, done);
            aws_array_list_push_back(&storage->subscription_identifiers, &subscription_identifier);
            break;
        }

        case AWS_MQTT5_PROPERTY_TYPE_CONTENT_TYPE:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->content_type, &storage_view->content_type, done);
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read PUBLISH property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes a PUBLISH packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_publish(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_publish_storage storage;
    if (aws_mqtt5_packet_publish_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }

    int result = AWS_OP_ERR;
    struct aws_mqtt5_packet_publish_view *storage_view = &storage.storage_view;

    /*
     * Fixed Header
     * byte 1:
     *  bits 4-7: MQTT Control Packet Type
     *  bit 3: DUP flag
     *  bit 1-2: QoS level
     *  bit 0: RETAIN
     * byte 2-x: Remaining Length as Variable Byte Integer (1-4 bytes)
     */

    uint8_t first_byte = decoder->packet_first_byte;
    if ((first_byte & PUBLISH_PACKET_FIXED_HEADER_DUPLICATE_FLAG) != 0) {
        storage_view->duplicate = true;
    }
    if ((first_byte & PUBLISH_PACKET_FIXED_HEADER_RETAIN_FLAG) != 0) {
        storage_view->retain = true;
    }
    storage_view->qos = (enum aws_mqtt5_qos)((first_byte >> 1) & PUBLISH_PACKET_FIXED_HEADER_QOS_FLAG);

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;
    uint32_t remaining_length = decoder->remaining_length;
    if (remaining_length != (uint32_t)packet_cursor.len) {
        goto done;
    }

    /*
     * Topic Name
     * Packet Identifier (only present for > QoS 0)
     * Properties
     *  - Property Length
     *  - Properties
     * Payload
     */
    AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR(&packet_cursor, &storage_view->topic, done);

    if (storage_view->qos > 0) {
        AWS_MQTT5_DECODE_U16(&packet_cursor, &storage_view->packet_id, done);
    }

    uint32_t property_length = 0;
    AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
    if (property_length > (uint32_t)packet_cursor.len) {
        goto done;
    }
    struct aws_byte_cursor properties_cursor = aws_byte_cursor_advance(&packet_cursor, property_length);

    while (properties_cursor.len > 0) {
        if (s_read_publish_property(&storage, &properties_cursor)) {
            goto done;
        }
    }

    storage_view->subscription_identifier_count = aws_array_list_length(&storage.subscription_identifiers);
    storage_view->subscription_identifiers = storage.subscription_identifiers.data;

    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;
    storage_view->payload = packet_cursor;

    if (storage_view->topic_alias != NULL) {
        if (decoder->topic_alias_resolver == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT,
                "id=%p: PUBLISH packet contained topic alias when not allowed",
                decoder->options.callback_user_data);
            goto done;
        }

        uint16_t topic_alias_id = *storage_view->topic_alias;
        if (topic_alias_id == 0) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT,
                "id=%p: PUBLISH packet contained illegal topic alias",
                decoder->options.callback_user_data);
            goto done;
        }

        if (storage_view->topic.len > 0) {
            if (aws_mqtt5_inbound_topic_alias_resolver_register_alias(
                    decoder->topic_alias_resolver, topic_alias_id, storage_view->topic)) {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_CLIENT, "id=%p: unable to register topic alias", decoder->options.callback_user_data);
                goto done;
            }
        } else {
            if (aws_mqtt5_inbound_topic_alias_resolver_resolve_alias(
                    decoder->topic_alias_resolver, topic_alias_id, &storage_view->topic)) {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_CLIENT,
                    "id=%p: PUBLISH packet contained unknown topic alias",
                    decoder->options.callback_user_data);
                goto done;
            }
        }
    }

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_PUBLISH, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: PUBLISH decode failure", decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_publish_storage_clean_up(&storage);

    return result;
}

/* decode function for all PUBACK properties */
static int s_read_puback_property(
    struct aws_mqtt5_packet_puback_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_puback_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_REASON_STRING:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->reason_string, &storage_view->reason_string, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read PUBACK property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes a PUBACK packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_puback(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_puback_storage storage;
    if (aws_mqtt5_packet_puback_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }
    int result = AWS_OP_ERR;

    uint8_t first_byte = decoder->packet_first_byte;
    /* PUBACK flags must be zero by protocol */
    if ((first_byte & 0x0F) != 0) {
        goto done;
    }

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;
    uint32_t remaining_length = decoder->remaining_length;
    if (remaining_length != (uint32_t)packet_cursor.len) {
        goto done;
    }

    struct aws_mqtt5_packet_puback_view *storage_view = &storage.storage_view;

    AWS_MQTT5_DECODE_U16(&packet_cursor, &storage_view->packet_id, done);

    /* Packet can end immediately following packet id with default success reason code */
    uint8_t reason_code = 0;
    if (packet_cursor.len > 0) {
        AWS_MQTT5_DECODE_U8(&packet_cursor, &reason_code, done);

        /* Packet can end immediately following reason code */
        if (packet_cursor.len > 0) {
            uint32_t property_length = 0;
            AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
            if (property_length != (uint32_t)packet_cursor.len) {
                goto done;
            }
            while (packet_cursor.len > 0) {
                if (s_read_puback_property(&storage, &packet_cursor)) {
                    goto done;
                }
            }
        }
    }

    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;
    storage_view->reason_code = reason_code;

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_PUBACK, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) aws_mqtt5_decoder - PUBACK decode failure",
            decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_puback_storage_clean_up(&storage);

    return result;
}

/* decode function for all SUBACK properties */
static int s_read_suback_property(
    struct aws_mqtt5_packet_suback_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_suback_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_REASON_STRING:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->reason_string, &storage_view->reason_string, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read SUBACK property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes a SUBACK packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_suback(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_suback_storage storage;
    if (aws_mqtt5_packet_suback_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }
    int result = AWS_OP_ERR;

    struct aws_mqtt5_packet_suback_view *storage_view = &storage.storage_view;

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;

    AWS_MQTT5_DECODE_U16(&packet_cursor, &storage_view->packet_id, done);

    uint32_t property_length = 0;
    AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
    struct aws_byte_cursor properties_cursor = aws_byte_cursor_advance(&packet_cursor, property_length);
    while (properties_cursor.len > 0) {
        if (s_read_suback_property(&storage, &properties_cursor)) {
            goto done;
        }
    }

    aws_array_list_init_dynamic(
        &storage.reason_codes, decoder->allocator, packet_cursor.len, sizeof(enum aws_mqtt5_suback_reason_code));

    while (packet_cursor.len > 0) {
        uint8_t reason_code;
        AWS_MQTT5_DECODE_U8(&packet_cursor, &reason_code, done);
        enum aws_mqtt5_suback_reason_code reason_code_enum = reason_code;
        aws_array_list_push_back(&storage.reason_codes, &reason_code_enum);
    }

    storage_view->reason_code_count = aws_array_list_length(&storage.reason_codes);
    storage_view->reason_codes = storage.reason_codes.data;
    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_SUBACK, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) aws_mqtt5_decoder - SUBACK decode failure",
            decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_suback_storage_clean_up(&storage);

    return result;
}

/* decode function for all UNSUBACK properties */
static int s_read_unsuback_property(
    struct aws_mqtt5_packet_unsuback_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_unsuback_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_REASON_STRING:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->reason_string, &storage_view->reason_string, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read UNSUBACK property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes an UNSUBACK packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_unsuback(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_unsuback_storage storage;

    /*
     * Fixed Header
     * byte 1: MQTT5 Control Packet - Reserved 0
     * byte 2 - x: VLI Remaining Length
     *
     * Variable Header
     * byte 1-2: Packet Identifier
     * Byte 3 - x: VLI Property Length
     *
     * Properties
     * byte 1: Idenfier
     * bytes 2 - x: Property content
     *
     * Payload
     * 1 byte per reason code in order of unsub requests
     */

    if (aws_mqtt5_packet_unsuback_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }
    int result = AWS_OP_ERR;

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;

    struct aws_mqtt5_packet_unsuback_view *storage_view = &storage.storage_view;

    AWS_MQTT5_DECODE_U16(&packet_cursor, &storage_view->packet_id, done);

    uint32_t property_length = 0;
    AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
    struct aws_byte_cursor properties_cursor = aws_byte_cursor_advance(&packet_cursor, property_length);
    while (properties_cursor.len > 0) {
        if (s_read_unsuback_property(&storage, &properties_cursor)) {
            goto done;
        }
    }

    aws_array_list_init_dynamic(
        &storage.reason_codes, decoder->allocator, packet_cursor.len, sizeof(enum aws_mqtt5_unsuback_reason_code));

    while (packet_cursor.len > 0) {
        uint8_t reason_code;
        AWS_MQTT5_DECODE_U8(&packet_cursor, &reason_code, done);
        enum aws_mqtt5_unsuback_reason_code reason_code_enum = reason_code;
        aws_array_list_push_back(&storage.reason_codes, &reason_code_enum);
    }

    storage_view->reason_code_count = aws_array_list_length(&storage.reason_codes);
    storage_view->reason_codes = storage.reason_codes.data;
    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_UNSUBACK, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_GENERAL,
            "(%p) aws_mqtt5_decoder - UNSUBACK decode failure",
            decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_unsuback_storage_clean_up(&storage);

    return result;
}

/* decode function for all DISCONNECT properties */
static int s_read_disconnect_property(
    struct aws_mqtt5_packet_disconnect_storage *storage,
    struct aws_byte_cursor *packet_cursor) {
    int result = AWS_OP_ERR;

    uint8_t property_type = 0;
    AWS_MQTT5_DECODE_U8(packet_cursor, &property_type, done);

    struct aws_mqtt5_packet_disconnect_view *storage_view = &storage->storage_view;

    switch (property_type) {
        case AWS_MQTT5_PROPERTY_TYPE_SESSION_EXPIRY_INTERVAL:
            AWS_MQTT5_DECODE_U32_OPTIONAL(
                packet_cursor,
                &storage->session_expiry_interval_seconds,
                &storage_view->session_expiry_interval_seconds,
                done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_SERVER_REFERENCE:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->server_reference, &storage_view->server_reference, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_REASON_STRING:
            AWS_MQTT5_DECODE_LENGTH_PREFIXED_CURSOR_OPTIONAL(
                packet_cursor, &storage->reason_string, &storage_view->reason_string, done);
            break;

        case AWS_MQTT5_PROPERTY_TYPE_USER_PROPERTY:
            if (aws_mqtt5_decode_user_property(packet_cursor, &storage->user_properties)) {
                goto done;
            }
            break;

        default:
            goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_ERR) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Read DISCONNECT property decode failure");
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return result;
}

/* decodes a DISCONNECT packet whose data must be in the scratch buffer */
static int s_aws_mqtt5_decoder_decode_disconnect(struct aws_mqtt5_decoder *decoder) {
    struct aws_mqtt5_packet_disconnect_storage storage;
    if (aws_mqtt5_packet_disconnect_storage_init_from_external_storage(&storage, decoder->allocator)) {
        return AWS_OP_ERR;
    }

    int result = AWS_OP_ERR;

    uint8_t first_byte = decoder->packet_first_byte;
    /* DISCONNECT flags must be zero by protocol */
    if ((first_byte & 0x0F) != 0) {
        goto done;
    }

    struct aws_byte_cursor packet_cursor = decoder->packet_cursor;
    uint32_t remaining_length = decoder->remaining_length;
    if (remaining_length != (uint32_t)packet_cursor.len) {
        goto done;
    }

    struct aws_mqtt5_packet_disconnect_view *storage_view = &storage.storage_view;
    if (remaining_length > 0) {
        uint8_t reason_code = 0;
        AWS_MQTT5_DECODE_U8(&packet_cursor, &reason_code, done);
        storage_view->reason_code = reason_code;
        if (packet_cursor.len == 0) {
            result = AWS_OP_SUCCESS;
            goto done;
        }

        uint32_t property_length = 0;
        AWS_MQTT5_DECODE_VLI(&packet_cursor, &property_length, done);
        if (property_length != (uint32_t)packet_cursor.len) {
            goto done;
        }

        while (packet_cursor.len > 0) {
            if (s_read_disconnect_property(&storage, &packet_cursor)) {
                goto done;
            }
        }
    }

    storage_view->user_property_count = aws_mqtt5_user_property_set_size(&storage.user_properties);
    storage_view->user_properties = storage.user_properties.properties.data;

    result = AWS_OP_SUCCESS;

done:

    if (result == AWS_OP_SUCCESS) {
        if (decoder->options.on_packet_received != NULL) {
            result = (*decoder->options.on_packet_received)(
                AWS_MQTT5_PT_DISCONNECT, &storage.storage_view, decoder->options.callback_user_data);
        }
    } else {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: DISCONNECT decode failure", decoder->options.callback_user_data);
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    aws_mqtt5_packet_disconnect_storage_clean_up(&storage);

    return result;
}

static int s_aws_mqtt5_decoder_decode_pingresp(struct aws_mqtt5_decoder *decoder) {
    if (decoder->packet_cursor.len != 0) {
        goto error;
    }

    uint8_t expected_first_byte = aws_mqtt5_compute_fixed_header_byte1(AWS_MQTT5_PT_PINGRESP, 0);
    if (decoder->packet_first_byte != expected_first_byte || decoder->remaining_length != 0) {
        goto error;
    }

    int result = AWS_OP_SUCCESS;
    if (decoder->options.on_packet_received != NULL) {
        result =
            (*decoder->options.on_packet_received)(AWS_MQTT5_PT_PINGRESP, NULL, decoder->options.callback_user_data);
    }

    return result;

error:

    AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: PINGRESP decode failure", decoder->options.callback_user_data);
    return aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
}

static int s_aws_mqtt5_decoder_decode_packet(struct aws_mqtt5_decoder *decoder) {
    enum aws_mqtt5_packet_type packet_type = (enum aws_mqtt5_packet_type)(decoder->packet_first_byte >> 4);
    aws_mqtt5_decoding_fn *decoder_fn = decoder->options.decoder_table->decoders_by_packet_type[packet_type];
    if (decoder_fn == NULL) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "Decoder decode packet function missing for enum: %d", packet_type);
        return aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
    }

    return (*decoder_fn)(decoder);
}

/*
 * (Streaming) Given a packet type and a variable length integer specifying the packet length, this state either
 *    (1) decodes directly from the cursor if possible
 *    (2) reads the packet into the scratch buffer and then decodes it once it is completely present
 *
 */
static enum aws_mqtt5_decode_result_type s_aws_mqtt5_decoder_read_packet_on_data(
    struct aws_mqtt5_decoder *decoder,
    struct aws_byte_cursor *data) {

    /* Are we able to decode directly from the channel message data buffer? */
    if (decoder->scratch_space.len == 0 && decoder->remaining_length <= data->len) {
        /* The cursor contains the entire packet, so decode directly from the backing io message buffer */
        decoder->packet_cursor = aws_byte_cursor_advance(data, decoder->remaining_length);
    } else {
        /* If the packet is fragmented across multiple io messages, then we buffer it internally */
        size_t unread_length = decoder->remaining_length - decoder->scratch_space.len;
        size_t copy_length = aws_min_size(unread_length, data->len);

        struct aws_byte_cursor copy_cursor = aws_byte_cursor_advance(data, copy_length);
        if (aws_byte_buf_append_dynamic(&decoder->scratch_space, &copy_cursor)) {
            return AWS_MQTT5_DRT_ERROR;
        }

        if (copy_length < unread_length) {
            return AWS_MQTT5_DRT_MORE_DATA;
        }

        decoder->packet_cursor = aws_byte_cursor_from_buf(&decoder->scratch_space);
    }

    if (s_aws_mqtt5_decoder_decode_packet(decoder)) {
        return AWS_MQTT5_DRT_ERROR;
    }

    s_enter_state(decoder, AWS_MQTT5_DS_READ_PACKET_TYPE);

    return AWS_MQTT5_DRT_SUCCESS;
}

/* top-level entry function for all new data received from the remote mqtt endpoint */
int aws_mqtt5_decoder_on_data_received(struct aws_mqtt5_decoder *decoder, struct aws_byte_cursor data) {
    enum aws_mqtt5_decode_result_type result = AWS_MQTT5_DRT_SUCCESS;
    while (result == AWS_MQTT5_DRT_SUCCESS) {
        switch (decoder->state) {
            case AWS_MQTT5_DS_READ_PACKET_TYPE:
                result = s_aws_mqtt5_decoder_read_packet_type_on_data(decoder, &data);
                break;

            case AWS_MQTT5_DS_READ_REMAINING_LENGTH:
                result = s_aws_mqtt5_decoder_read_remaining_length_on_data(decoder, &data);
                break;

            case AWS_MQTT5_DS_READ_PACKET:
                result = s_aws_mqtt5_decoder_read_packet_on_data(decoder, &data);
                break;

            default:
                result = AWS_MQTT5_DRT_ERROR;
                break;
        }
    }

    if (result == AWS_MQTT5_DRT_ERROR) {
        aws_raise_error(AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR);
        decoder->state = AWS_MQTT5_DS_FATAL_ERROR;
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static struct aws_mqtt5_decoder_function_table s_aws_mqtt5_decoder_default_function_table = {
    .decoders_by_packet_type =
        {
            NULL,                                   /* RESERVED = 0 */
            NULL,                                   /* CONNECT */
            &s_aws_mqtt5_decoder_decode_connack,    /* CONNACK */
            &s_aws_mqtt5_decoder_decode_publish,    /* PUBLISH */
            &s_aws_mqtt5_decoder_decode_puback,     /* PUBACK */
            NULL,                                   /* PUBREC */
            NULL,                                   /* PUBREL */
            NULL,                                   /* PUBCOMP */
            NULL,                                   /* SUBSCRIBE */
            &s_aws_mqtt5_decoder_decode_suback,     /* SUBACK */
            NULL,                                   /* UNSUBSCRIBE */
            &s_aws_mqtt5_decoder_decode_unsuback,   /* UNSUBACK */
            NULL,                                   /* PINGREQ */
            &s_aws_mqtt5_decoder_decode_pingresp,   /* PINGRESP */
            &s_aws_mqtt5_decoder_decode_disconnect, /* DISCONNECT */
            NULL                                    /* AUTH */
        },
};

const struct aws_mqtt5_decoder_function_table *g_aws_mqtt5_default_decoder_table =
    &s_aws_mqtt5_decoder_default_function_table;

int aws_mqtt5_decoder_init(
    struct aws_mqtt5_decoder *decoder,
    struct aws_allocator *allocator,
    struct aws_mqtt5_decoder_options *options) {
    AWS_ZERO_STRUCT(*decoder);

    decoder->options = *options;

    if (decoder->options.decoder_table == NULL) {
        decoder->options.decoder_table = g_aws_mqtt5_default_decoder_table;
    }

    decoder->allocator = allocator;

    decoder->state = AWS_MQTT5_DS_READ_PACKET_TYPE;

    if (aws_byte_buf_init(&decoder->scratch_space, allocator, AWS_MQTT5_DECODER_BUFFER_START_SIZE)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_decoder_reset(struct aws_mqtt5_decoder *decoder) {
    s_reset_decoder_for_new_packet(decoder);

    decoder->state = AWS_MQTT5_DS_READ_PACKET_TYPE;
}

void aws_mqtt5_decoder_clean_up(struct aws_mqtt5_decoder *decoder) {
    aws_byte_buf_clean_up(&decoder->scratch_space);
}

void aws_mqtt5_decoder_set_inbound_topic_alias_resolver(
    struct aws_mqtt5_decoder *decoder,
    struct aws_mqtt5_inbound_topic_alias_resolver *resolver) {
    decoder->topic_alias_resolver = resolver;
}
