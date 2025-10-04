#ifndef AWS_MQTT_MQTT5_TYPES_H
#define AWS_MQTT_MQTT5_TYPES_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/common/array_list.h>
#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Some artificial (non-MQTT spec specified) limits that we place on input packets (publish, subscribe, unsubscibe)
 * which lets us safely do the various packet size calculations with a bare minimum of checked arithmetic.
 *
 * I don't see any conceivable use cases why you'd want more than this, but they are relaxable to some degree.
 *
 * TODO: Add some static assert calculations that verify that we can't possibly overflow against the maximum value
 * of a variable length integer for relevant packet size encodings that are absolute worst-case against these limits.
 */
#define AWS_MQTT5_CLIENT_MAXIMUM_USER_PROPERTIES 1024
#define AWS_MQTT5_CLIENT_MAXIMUM_SUBSCRIPTIONS_PER_SUBSCRIBE 1024
#define AWS_MQTT5_CLIENT_MAXIMUM_TOPIC_FILTERS_PER_UNSUBSCRIBE 1024

/**
 * Over-the-wire packet id as defined in the mqtt spec.  Allocated at the point in time when the packet is
 * is next to go down the channel and about to be encoded into an io message buffer.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901026
 */
typedef uint16_t aws_mqtt5_packet_id_t;

/**
 * MQTT Message delivery quality of service.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901234
 */
enum aws_mqtt5_qos {

    /** https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901235 */
    AWS_MQTT5_QOS_AT_MOST_ONCE = 0x0,

    /** https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901236 */
    AWS_MQTT5_QOS_AT_LEAST_ONCE = 0x1,

    /** https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901237 */
    AWS_MQTT5_QOS_EXACTLY_ONCE = 0x2,
};

/**
 * Server return code for CONNECT attempts.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901079
 */
enum aws_mqtt5_connect_reason_code {
    AWS_MQTT5_CRC_SUCCESS = 0,
    AWS_MQTT5_CRC_UNSPECIFIED_ERROR = 128,
    AWS_MQTT5_CRC_MALFORMED_PACKET = 129,
    AWS_MQTT5_CRC_PROTOCOL_ERROR = 130,
    AWS_MQTT5_CRC_IMPLEMENTATION_SPECIFIC_ERROR = 131,
    AWS_MQTT5_CRC_UNSUPPORTED_PROTOCOL_VERSION = 132,
    AWS_MQTT5_CRC_CLIENT_IDENTIFIER_NOT_VALID = 133,
    AWS_MQTT5_CRC_BAD_USERNAME_OR_PASSWORD = 134,
    AWS_MQTT5_CRC_NOT_AUTHORIZED = 135,
    AWS_MQTT5_CRC_SERVER_UNAVAILABLE = 136,
    AWS_MQTT5_CRC_SERVER_BUSY = 137,
    AWS_MQTT5_CRC_BANNED = 138,
    AWS_MQTT5_CRC_BAD_AUTHENTICATION_METHOD = 140,
    AWS_MQTT5_CRC_TOPIC_NAME_INVALID = 144,
    AWS_MQTT5_CRC_PACKET_TOO_LARGE = 149,
    AWS_MQTT5_CRC_QUOTA_EXCEEDED = 151,
    AWS_MQTT5_CRC_PAYLOAD_FORMAT_INVALID = 153,
    AWS_MQTT5_CRC_RETAIN_NOT_SUPPORTED = 154,
    AWS_MQTT5_CRC_QOS_NOT_SUPPORTED = 155,
    AWS_MQTT5_CRC_USE_ANOTHER_SERVER = 156,
    AWS_MQTT5_CRC_SERVER_MOVED = 157,
    AWS_MQTT5_CRC_CONNECTION_RATE_EXCEEDED = 159,
};

/**
 * Reason code inside DISCONNECT packets.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901208
 */
enum aws_mqtt5_disconnect_reason_code {
    AWS_MQTT5_DRC_NORMAL_DISCONNECTION = 0,
    AWS_MQTT5_DRC_DISCONNECT_WITH_WILL_MESSAGE = 4,
    AWS_MQTT5_DRC_UNSPECIFIED_ERROR = 128,
    AWS_MQTT5_DRC_MALFORMED_PACKET = 129,
    AWS_MQTT5_DRC_PROTOCOL_ERROR = 130,
    AWS_MQTT5_DRC_IMPLEMENTATION_SPECIFIC_ERROR = 131,
    AWS_MQTT5_DRC_NOT_AUTHORIZED = 135,
    AWS_MQTT5_DRC_SERVER_BUSY = 137,
    AWS_MQTT5_DRC_SERVER_SHUTTING_DOWN = 139,
    AWS_MQTT5_DRC_KEEP_ALIVE_TIMEOUT = 141,
    AWS_MQTT5_DRC_SESSION_TAKEN_OVER = 142,
    AWS_MQTT5_DRC_TOPIC_FILTER_INVALID = 143,
    AWS_MQTT5_DRC_TOPIC_NAME_INVALID = 144,
    AWS_MQTT5_DRC_RECEIVE_MAXIMUM_EXCEEDED = 147,
    AWS_MQTT5_DRC_TOPIC_ALIAS_INVALID = 148,
    AWS_MQTT5_DRC_PACKET_TOO_LARGE = 149,
    AWS_MQTT5_DRC_MESSAGE_RATE_TOO_HIGH = 150,
    AWS_MQTT5_DRC_QUOTA_EXCEEDED = 151,
    AWS_MQTT5_DRC_ADMINISTRATIVE_ACTION = 152,
    AWS_MQTT5_DRC_PAYLOAD_FORMAT_INVALID = 153,
    AWS_MQTT5_DRC_RETAIN_NOT_SUPPORTED = 154,
    AWS_MQTT5_DRC_QOS_NOT_SUPPORTED = 155,
    AWS_MQTT5_DRC_USE_ANOTHER_SERVER = 156,
    AWS_MQTT5_DRC_SERVER_MOVED = 157,
    AWS_MQTT5_DRC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED = 158,
    AWS_MQTT5_DRC_CONNECTION_RATE_EXCEEDED = 159,
    AWS_MQTT5_DRC_MAXIMUM_CONNECT_TIME = 160,
    AWS_MQTT5_DRC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 161,
    AWS_MQTT5_DRC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 162,
};

/**
 * Reason code inside PUBACK packets.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124
 */
enum aws_mqtt5_puback_reason_code {
    AWS_MQTT5_PARC_SUCCESS = 0,
    AWS_MQTT5_PARC_NO_MATCHING_SUBSCRIBERS = 16,
    AWS_MQTT5_PARC_UNSPECIFIED_ERROR = 128,
    AWS_MQTT5_PARC_IMPLEMENTATION_SPECIFIC_ERROR = 131,
    AWS_MQTT5_PARC_NOT_AUTHORIZED = 135,
    AWS_MQTT5_PARC_TOPIC_NAME_INVALID = 144,
    AWS_MQTT5_PARC_PACKET_IDENTIFIER_IN_USE = 145,
    AWS_MQTT5_PARC_QUOTA_EXCEEDED = 151,
    AWS_MQTT5_PARC_PAYLOAD_FORMAT_INVALID = 153,
};

/**
 * Reason code inside SUBACK packet payloads.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901178
 */
enum aws_mqtt5_suback_reason_code {
    AWS_MQTT5_SARC_GRANTED_QOS_0 = 0,
    AWS_MQTT5_SARC_GRANTED_QOS_1 = 1,
    AWS_MQTT5_SARC_GRANTED_QOS_2 = 2,
    AWS_MQTT5_SARC_UNSPECIFIED_ERROR = 128,
    AWS_MQTT5_SARC_IMPLEMENTATION_SPECIFIC_ERROR = 131,
    AWS_MQTT5_SARC_NOT_AUTHORIZED = 135,
    AWS_MQTT5_SARC_TOPIC_FILTER_INVALID = 143,
    AWS_MQTT5_SARC_PACKET_IDENTIFIER_IN_USE = 145,
    AWS_MQTT5_SARC_QUOTA_EXCEEDED = 151,
    AWS_MQTT5_SARC_SHARED_SUBSCRIPTIONS_NOT_SUPPORTED = 158,
    AWS_MQTT5_SARC_SUBSCRIPTION_IDENTIFIERS_NOT_SUPPORTED = 161,
    AWS_MQTT5_SARC_WILDCARD_SUBSCRIPTIONS_NOT_SUPPORTED = 162,
};

/**
 * Reason code inside UNSUBACK packet payloads.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901194
 */
enum aws_mqtt5_unsuback_reason_code {
    AWS_MQTT5_UARC_SUCCESS = 0,
    AWS_MQTT5_UARC_NO_SUBSCRIPTION_EXISTED = 17,
    AWS_MQTT5_UARC_UNSPECIFIED_ERROR = 128,
    AWS_MQTT5_UARC_IMPLEMENTATION_SPECIFIC_ERROR = 131,
    AWS_MQTT5_UARC_NOT_AUTHORIZED = 135,
    AWS_MQTT5_UARC_TOPIC_FILTER_INVALID = 143,
    AWS_MQTT5_UARC_PACKET_IDENTIFIER_IN_USE = 145,
};

/**
 * Type of mqtt packet.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901022
 */
enum aws_mqtt5_packet_type {
    /* internal indicator that the associated packet is null */
    AWS_MQTT5_PT_NONE = -1,
    AWS_MQTT5_PT_RESERVED = 0,
    AWS_MQTT5_PT_CONNECT = 1,
    AWS_MQTT5_PT_CONNACK = 2,
    AWS_MQTT5_PT_PUBLISH = 3,
    AWS_MQTT5_PT_PUBACK = 4,
    AWS_MQTT5_PT_PUBREC = 5,
    AWS_MQTT5_PT_PUBREL = 6,
    AWS_MQTT5_PT_PUBCOMP = 7,
    AWS_MQTT5_PT_SUBSCRIBE = 8,
    AWS_MQTT5_PT_SUBACK = 9,
    AWS_MQTT5_PT_UNSUBSCRIBE = 10,
    AWS_MQTT5_PT_UNSUBACK = 11,
    AWS_MQTT5_PT_PINGREQ = 12,
    AWS_MQTT5_PT_PINGRESP = 13,
    AWS_MQTT5_PT_DISCONNECT = 14,
    AWS_MQTT5_PT_AUTH = 15,
};

/**
 * Non-persistent representation of an mqtt5 user property.
 */
struct aws_mqtt5_user_property {
    struct aws_byte_cursor name;
    struct aws_byte_cursor value;
};

/**
 * Optional property describing a message's payload format.
 * Enum values match mqtt spec encoding values.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901063
 */
enum aws_mqtt5_payload_format_indicator {
    AWS_MQTT5_PFI_BYTES = 0,
    AWS_MQTT5_PFI_UTF8 = 1,
};

/**
 * Configures how retained messages should be handled when subscribing with a topic filter that matches topics with
 * associated retained messages.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169
 */
enum aws_mqtt5_retain_handling_type {

    /**
     * Server should send all retained messages on topics that match the subscription's filter.
     */
    AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE = 0x00,

    /**
     * Server should send all retained messages on topics that match the subscription's filter, where this is the
     * first (relative to connection) subscription filter that matches the topic with a retained message.
     */
    AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE_IF_NEW = 0x01,

    /**
     * Subscribe must not trigger any retained message publishes from the server.
     */
    AWS_MQTT5_RHT_DONT_SEND = 0x02,
};

/**
 * Configures a single subscription within a Subscribe operation
 */
struct aws_mqtt5_subscription_view {
    /**
     * Topic filter to subscribe to
     */
    struct aws_byte_cursor topic_filter;

    /**
     * Maximum QOS that the subscriber will accept messages for.  Negotiated QoS may be different.
     */
    enum aws_mqtt5_qos qos;

    /**
     * Should the server not send publishes to a client when that client was the one who sent the publish?
     */
    bool no_local;

    /**
     * Should messages sent due to this subscription keep the retain flag preserved on the message?
     */
    bool retain_as_published;

    /**
     * Should retained messages on matching topics be sent in reaction to this subscription?
     */
    enum aws_mqtt5_retain_handling_type retain_handling_type;
};

/**
 * Read-only snapshot of a DISCONNECT packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901205
 */
struct aws_mqtt5_packet_disconnect_view {
    enum aws_mqtt5_disconnect_reason_code reason_code;
    const uint32_t *session_expiry_interval_seconds;
    const struct aws_byte_cursor *reason_string;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;

    const struct aws_byte_cursor *server_reference;
};

/**
 * Read-only snapshot of a SUBSCRIBE packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901161
 */
struct aws_mqtt5_packet_subscribe_view {
    aws_mqtt5_packet_id_t packet_id;

    size_t subscription_count;
    const struct aws_mqtt5_subscription_view *subscriptions;

    const uint32_t *subscription_identifier;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;
};

/**
 * Read-only snapshot of an UNSUBSCRIBE packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901179
 */
struct aws_mqtt5_packet_unsubscribe_view {
    aws_mqtt5_packet_id_t packet_id;

    size_t topic_filter_count;
    const struct aws_byte_cursor *topic_filters;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;
};

/**
 * Read-only snapshot of a PUBLISH packet.  Used both in configuration of a publish operation and callback
 * data in message receipt.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901100
 */
struct aws_mqtt5_packet_publish_view {
    struct aws_byte_cursor payload;

    /* packet_id is only set for QoS 1 and QoS 2 */
    aws_mqtt5_packet_id_t packet_id;

    enum aws_mqtt5_qos qos;

    /*
     * Used to set the duplicate flag on QoS 1+ re-delivery attempts.
     * Set to false on all first attempts or QoS 0. Set to true on any re-delivery.
     */
    bool duplicate;
    bool retain;
    struct aws_byte_cursor topic;
    const enum aws_mqtt5_payload_format_indicator *payload_format;
    const uint32_t *message_expiry_interval_seconds;
    const uint16_t *topic_alias;
    const struct aws_byte_cursor *response_topic;
    const struct aws_byte_cursor *correlation_data;

    /* These are ignored when building publish operations */
    size_t subscription_identifier_count;
    const uint32_t *subscription_identifiers;

    const struct aws_byte_cursor *content_type;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;
};

/**
 * Read-only snapshot of a CONNECT packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901033
 */
struct aws_mqtt5_packet_connect_view {
    uint16_t keep_alive_interval_seconds;

    struct aws_byte_cursor client_id;

    const struct aws_byte_cursor *username;
    const struct aws_byte_cursor *password;

    bool clean_start;

    const uint32_t *session_expiry_interval_seconds;

    const uint8_t *request_response_information;
    const uint8_t *request_problem_information;
    const uint16_t *receive_maximum;
    const uint16_t *topic_alias_maximum;
    const uint32_t *maximum_packet_size_bytes;

    const uint32_t *will_delay_interval_seconds;
    const struct aws_mqtt5_packet_publish_view *will;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;

    /* Do not bind these.  We don't support AUTH packets yet.  For decode/encade testing purposes only. */
    const struct aws_byte_cursor *authentication_method;
    const struct aws_byte_cursor *authentication_data;
};

/**
 * Read-only snapshot of a CONNACK packet.
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901074
 */
struct aws_mqtt5_packet_connack_view {
    bool session_present;
    enum aws_mqtt5_connect_reason_code reason_code;

    const uint32_t *session_expiry_interval;
    const uint16_t *receive_maximum;
    const enum aws_mqtt5_qos *maximum_qos;
    const bool *retain_available;
    const uint32_t *maximum_packet_size;
    const struct aws_byte_cursor *assigned_client_identifier;
    const uint16_t *topic_alias_maximum;
    const struct aws_byte_cursor *reason_string;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;

    const bool *wildcard_subscriptions_available;
    const bool *subscription_identifiers_available;
    const bool *shared_subscriptions_available;

    const uint16_t *server_keep_alive;
    const struct aws_byte_cursor *response_information;
    const struct aws_byte_cursor *server_reference;
    const struct aws_byte_cursor *authentication_method;
    const struct aws_byte_cursor *authentication_data;
};

/**
 * Read-only snapshot of a PUBACK packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901121
 */
struct aws_mqtt5_packet_puback_view {
    aws_mqtt5_packet_id_t packet_id;

    enum aws_mqtt5_puback_reason_code reason_code;
    const struct aws_byte_cursor *reason_string;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;
};

/**
 * Read-only snapshot of a SUBACK packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901171
 */
struct aws_mqtt5_packet_suback_view {
    aws_mqtt5_packet_id_t packet_id;

    const struct aws_byte_cursor *reason_string;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;

    size_t reason_code_count;
    const enum aws_mqtt5_suback_reason_code *reason_codes;
};

/**
 * Read-only snapshot of an UNSUBACK packet
 *
 * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901187
 */
struct aws_mqtt5_packet_unsuback_view {
    aws_mqtt5_packet_id_t packet_id;

    const struct aws_byte_cursor *reason_string;

    size_t user_property_count;
    const struct aws_mqtt5_user_property *user_properties;

    size_t reason_code_count;
    const enum aws_mqtt5_unsuback_reason_code *reason_codes;
};
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_MQTT_MQTT5_TYPES_H */
