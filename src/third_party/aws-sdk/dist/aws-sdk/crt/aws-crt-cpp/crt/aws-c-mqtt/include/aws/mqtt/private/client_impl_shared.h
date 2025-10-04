#ifndef AWS_MQTT_PRIVATE_CLIENT_IMPL_SHARED_H
#define AWS_MQTT_PRIVATE_CLIENT_IMPL_SHARED_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/client.h>

struct aws_mqtt_client_connection;

/*
 * Internal enum that indicates what type of struct the underlying impl pointer actually is.  We use this
 * to safely interact with private APIs on the implementation or extract the adapted 5 client directly, as
 * necessary.
 */
enum aws_mqtt311_impl_type {

    /* 311 connection impl can be cast to `struct aws_mqtt_client_connection_311_impl` */
    AWS_MQTT311_IT_311_CONNECTION,

    /* 311 connection impl can be cast to `struct aws_mqtt_client_connection_5_impl`*/
    AWS_MQTT311_IT_5_ADAPTER,
};

struct aws_mqtt_client_connection_vtable {

    struct aws_mqtt_client_connection *(*acquire_fn)(void *impl);

    void (*release_fn)(void *impl);

    int (*set_will_fn)(
        void *impl,
        const struct aws_byte_cursor *topic,
        enum aws_mqtt_qos qos,
        bool retain,
        const struct aws_byte_cursor *payload);

    int (*set_login_fn)(void *impl, const struct aws_byte_cursor *username, const struct aws_byte_cursor *password);

    int (*use_websockets_fn)(
        void *impl,
        aws_mqtt_transform_websocket_handshake_fn *transformer,
        void *transformer_ud,
        aws_mqtt_validate_websocket_handshake_fn *validator,
        void *validator_ud);

    int (*set_http_proxy_options_fn)(void *impl, struct aws_http_proxy_options *proxy_options);

    int (*set_host_resolution_options_fn)(void *impl, const struct aws_host_resolution_config *host_resolution_config);

    int (*set_reconnect_timeout_fn)(void *impl, uint64_t min_timeout, uint64_t max_timeout);

    int (*set_connection_interruption_handlers_fn)(
        void *impl,
        aws_mqtt_client_on_connection_interrupted_fn *on_interrupted,
        void *on_interrupted_ud,
        aws_mqtt_client_on_connection_resumed_fn *on_resumed,
        void *on_resumed_ud);

    int (*set_connection_result_handlers)(
        void *impl,
        aws_mqtt_client_on_connection_success_fn *on_connection_success,
        void *on_connection_success_ud,
        aws_mqtt_client_on_connection_failure_fn *on_connection_failure,
        void *on_connection_failure_ud);

    int (*set_connection_closed_handler_fn)(
        void *impl,
        aws_mqtt_client_on_connection_closed_fn *on_closed,
        void *on_closed_ud);

    int (*set_on_any_publish_handler_fn)(
        void *impl,
        aws_mqtt_client_publish_received_fn *on_any_publish,
        void *on_any_publish_ud);

    int (*set_connection_termination_handler_fn)(
        void *impl,
        aws_mqtt_client_on_connection_termination_fn *on_termination,
        void *on_termination_ud);

    int (*connect_fn)(void *impl, const struct aws_mqtt_connection_options *connection_options);

    int (*reconnect_fn)(void *impl, aws_mqtt_client_on_connection_complete_fn *on_connection_complete, void *userdata);

    int (*disconnect_fn)(void *impl, aws_mqtt_client_on_disconnect_fn *on_disconnect, void *userdata);

    uint16_t (*subscribe_multiple_fn)(
        void *impl,
        const struct aws_array_list *topic_filters,
        aws_mqtt_suback_multi_fn *on_suback,
        void *on_suback_ud);

    uint16_t (*subscribe_fn)(
        void *impl,
        const struct aws_byte_cursor *topic_filter,
        enum aws_mqtt_qos qos,
        aws_mqtt_client_publish_received_fn *on_publish,
        void *on_publish_ud,
        aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
        aws_mqtt_suback_fn *on_suback,
        void *on_suback_ud);

    uint16_t (*resubscribe_existing_topics_fn)(void *impl, aws_mqtt_suback_multi_fn *on_suback, void *on_suback_ud);

    uint16_t (*unsubscribe_fn)(
        void *impl,
        const struct aws_byte_cursor *topic_filter,
        aws_mqtt_op_complete_fn *on_unsuback,
        void *on_unsuback_ud);

    uint16_t (*publish_fn)(
        void *impl,
        const struct aws_byte_cursor *topic,
        enum aws_mqtt_qos qos,
        bool retain,
        const struct aws_byte_cursor *payload,
        aws_mqtt_op_complete_fn *on_complete,
        void *userdata);

    int (*get_stats_fn)(void *impl, struct aws_mqtt_connection_operation_statistics *stats);

    enum aws_mqtt311_impl_type (*get_impl_type)(const void *impl);

    struct aws_event_loop *(*get_event_loop)(const void *impl);
};

struct aws_mqtt_client_connection {
    struct aws_mqtt_client_connection_vtable *vtable;
    void *impl;
};

AWS_MQTT_API enum aws_mqtt311_impl_type aws_mqtt_client_connection_get_impl_type(
    const struct aws_mqtt_client_connection *connection);

AWS_MQTT_API uint64_t aws_mqtt_hash_uint16_t(const void *item);

AWS_MQTT_API bool aws_mqtt_compare_uint16_t_eq(const void *a, const void *b);

AWS_MQTT_API bool aws_mqtt_byte_cursor_hash_equality(const void *a, const void *b);

AWS_MQTT_API struct aws_event_loop *aws_mqtt_client_connection_get_event_loop(
    const struct aws_mqtt_client_connection *connection);

#endif /* AWS_MQTT_PRIVATE_CLIENT_IMPL_SHARED_H */
