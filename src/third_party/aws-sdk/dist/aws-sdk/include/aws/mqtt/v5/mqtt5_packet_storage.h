#ifndef AWS_MQTT_MQTT5_PACKET_STORAGE_H
#define AWS_MQTT_MQTT5_PACKET_STORAGE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/mqtt/v5/mqtt5_types.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_mqtt5_user_property_set {
    struct aws_array_list properties;
};

struct aws_mqtt5_packet_connect_storage {
    struct aws_allocator *allocator;

    struct aws_mqtt5_packet_connect_view storage_view;

    struct aws_byte_cursor username;

    struct aws_byte_cursor password;

    uint32_t session_expiry_interval_seconds;

    uint8_t request_response_information;

    uint8_t request_problem_information;

    uint16_t receive_maximum;

    uint16_t topic_alias_maximum;

    uint32_t maximum_packet_size_bytes;

    struct aws_mqtt5_packet_publish_storage *will;

    uint32_t will_delay_interval_seconds;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_cursor authentication_method;

    struct aws_byte_cursor authentication_data;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_connack_storage {
    struct aws_allocator *allocator;

    struct aws_mqtt5_packet_connack_view storage_view;

    uint32_t session_expiry_interval;

    uint16_t receive_maximum;

    enum aws_mqtt5_qos maximum_qos;

    bool retain_available;

    uint32_t maximum_packet_size;

    struct aws_byte_cursor assigned_client_identifier;

    uint16_t topic_alias_maximum;

    struct aws_byte_cursor reason_string;

    bool wildcard_subscriptions_available;

    bool subscription_identifiers_available;

    bool shared_subscriptions_available;

    uint16_t server_keep_alive;

    struct aws_byte_cursor response_information;

    struct aws_byte_cursor server_reference;

    struct aws_byte_cursor authentication_method;

    struct aws_byte_cursor authentication_data;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_suback_storage {

    struct aws_allocator *allocator;

    struct aws_mqtt5_packet_suback_view storage_view;

    struct aws_byte_cursor reason_string;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_array_list reason_codes;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_unsuback_storage {

    struct aws_allocator *allocator;

    struct aws_mqtt5_packet_unsuback_view storage_view;

    struct aws_byte_cursor reason_string;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_array_list reason_codes;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_publish_storage {
    struct aws_mqtt5_packet_publish_view storage_view;

    enum aws_mqtt5_payload_format_indicator payload_format;

    uint32_t message_expiry_interval_seconds;

    uint16_t topic_alias;

    struct aws_byte_cursor response_topic;

    struct aws_byte_cursor correlation_data;

    struct aws_byte_cursor content_type;

    struct aws_mqtt5_user_property_set user_properties;
    struct aws_array_list subscription_identifiers;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_puback_storage {
    struct aws_mqtt5_packet_puback_view storage_view;

    struct aws_byte_cursor reason_string;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_disconnect_storage {
    struct aws_mqtt5_packet_disconnect_view storage_view;

    uint32_t session_expiry_interval_seconds;

    struct aws_byte_cursor reason_string;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_cursor server_reference;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_subscribe_storage {
    struct aws_mqtt5_packet_subscribe_view storage_view;

    uint32_t subscription_identifier;

    struct aws_array_list subscriptions;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_buf storage;
};

struct aws_mqtt5_packet_unsubscribe_storage {
    struct aws_mqtt5_packet_unsubscribe_view storage_view;

    struct aws_array_list topic_filters;

    struct aws_mqtt5_user_property_set user_properties;

    struct aws_byte_buf storage;
};

AWS_EXTERN_C_BEGIN

/* User properties */

AWS_MQTT_API int aws_mqtt5_user_property_set_init_with_storage(
    struct aws_mqtt5_user_property_set *property_set,
    struct aws_allocator *allocator,
    struct aws_byte_buf *storage_buffer,
    size_t property_count,
    const struct aws_mqtt5_user_property *properties);

AWS_MQTT_API void aws_mqtt5_user_property_set_clean_up(struct aws_mqtt5_user_property_set *property_set);

AWS_MQTT_API size_t aws_mqtt5_user_property_set_size(const struct aws_mqtt5_user_property_set *property_set);

/* Connect */

AWS_MQTT_API int aws_mqtt5_packet_connect_storage_init(
    struct aws_mqtt5_packet_connect_storage *connect_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_connect_view *connect_options);

AWS_MQTT_API int aws_mqtt5_packet_connect_storage_init_from_external_storage(
    struct aws_mqtt5_packet_connect_storage *connect_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_connect_storage_clean_up(struct aws_mqtt5_packet_connect_storage *connect_storage);

/* Connack */

AWS_MQTT_API int aws_mqtt5_packet_connack_storage_init(
    struct aws_mqtt5_packet_connack_storage *connack_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_connack_view *connack_options);

AWS_MQTT_API int aws_mqtt5_packet_connack_storage_init_from_external_storage(
    struct aws_mqtt5_packet_connack_storage *connack_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_connack_storage_clean_up(struct aws_mqtt5_packet_connack_storage *connack_storage);

/* Disconnect */

AWS_MQTT_API int aws_mqtt5_packet_disconnect_storage_init(
    struct aws_mqtt5_packet_disconnect_storage *disconnect_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_disconnect_view *disconnect_options);

AWS_MQTT_API int aws_mqtt5_packet_disconnect_storage_init_from_external_storage(
    struct aws_mqtt5_packet_disconnect_storage *disconnect_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_disconnect_storage_clean_up(
    struct aws_mqtt5_packet_disconnect_storage *disconnect_storage);

/* Publish */

AWS_MQTT_API int aws_mqtt5_packet_publish_storage_init(
    struct aws_mqtt5_packet_publish_storage *publish_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_publish_view *publish_options);

AWS_MQTT_API int aws_mqtt5_packet_publish_storage_init_from_external_storage(
    struct aws_mqtt5_packet_publish_storage *publish_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_publish_storage_clean_up(struct aws_mqtt5_packet_publish_storage *publish_storage);

/* Puback */

AWS_MQTT_API int aws_mqtt5_packet_puback_storage_init(
    struct aws_mqtt5_packet_puback_storage *puback_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_puback_view *puback_view);

AWS_MQTT_API int aws_mqtt5_packet_puback_storage_init_from_external_storage(
    struct aws_mqtt5_packet_puback_storage *puback_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_puback_storage_clean_up(struct aws_mqtt5_packet_puback_storage *puback_storage);

/* Subscribe */

AWS_MQTT_API int aws_mqtt5_packet_subscribe_storage_init(
    struct aws_mqtt5_packet_subscribe_storage *subscribe_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_subscribe_view *subscribe_options);

AWS_MQTT_API int aws_mqtt5_packet_subscribe_storage_init_from_external_storage(
    struct aws_mqtt5_packet_subscribe_storage *subscribe_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_subscribe_storage_clean_up(
    struct aws_mqtt5_packet_subscribe_storage *subscribe_storage);

/* Suback */

AWS_MQTT_API int aws_mqtt5_packet_suback_storage_init(
    struct aws_mqtt5_packet_suback_storage *suback_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_suback_view *suback_view);

AWS_MQTT_API int aws_mqtt5_packet_suback_storage_init_from_external_storage(
    struct aws_mqtt5_packet_suback_storage *suback_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_suback_storage_clean_up(struct aws_mqtt5_packet_suback_storage *suback_storage);

/* Unsubscribe */

AWS_MQTT_API int aws_mqtt5_packet_unsubscribe_storage_init(
    struct aws_mqtt5_packet_unsubscribe_storage *unsubscribe_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_unsubscribe_view *unsubscribe_options);

AWS_MQTT_API int aws_mqtt5_packet_unsubscribe_storage_init_from_external_storage(
    struct aws_mqtt5_packet_unsubscribe_storage *unsubscribe_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_unsubscribe_storage_clean_up(
    struct aws_mqtt5_packet_unsubscribe_storage *unsubscribe_storage);

/* Unsuback */

AWS_MQTT_API int aws_mqtt5_packet_unsuback_storage_init(
    struct aws_mqtt5_packet_unsuback_storage *unsuback_storage,
    struct aws_allocator *allocator,
    const struct aws_mqtt5_packet_unsuback_view *unsuback_view);

AWS_MQTT_API int aws_mqtt5_packet_unsuback_storage_init_from_external_storage(
    struct aws_mqtt5_packet_unsuback_storage *unsuback_storage,
    struct aws_allocator *allocator);

AWS_MQTT_API void aws_mqtt5_packet_unsuback_storage_clean_up(
    struct aws_mqtt5_packet_unsuback_storage *unsuback_storage);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_MQTT_MQTT5_PACKET_STORAGE_H */
