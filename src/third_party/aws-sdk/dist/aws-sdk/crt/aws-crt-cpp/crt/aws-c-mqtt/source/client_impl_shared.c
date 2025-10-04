/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/client.h>
#include <aws/mqtt/private/client_impl_shared.h>

struct aws_mqtt_client_connection *aws_mqtt_client_connection_acquire(struct aws_mqtt_client_connection *connection) {
    if (connection != NULL) {
        return (*connection->vtable->acquire_fn)(connection->impl);
    }

    return NULL;
}

void aws_mqtt_client_connection_release(struct aws_mqtt_client_connection *connection) {
    if (connection != NULL) {
        (*connection->vtable->release_fn)(connection->impl);
    }
}

int aws_mqtt_client_connection_set_will(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload) {

    return (*connection->vtable->set_will_fn)(connection->impl, topic, qos, retain, payload);
}

int aws_mqtt_client_connection_set_login(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *username,
    const struct aws_byte_cursor *password) {

    return (*connection->vtable->set_login_fn)(connection->impl, username, password);
}

int aws_mqtt_client_connection_use_websockets(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_transform_websocket_handshake_fn *transformer,
    void *transformer_ud,
    aws_mqtt_validate_websocket_handshake_fn *validator,
    void *validator_ud) {

    return (*connection->vtable->use_websockets_fn)(
        connection->impl, transformer, transformer_ud, validator, validator_ud);
}

int aws_mqtt_client_connection_set_http_proxy_options(
    struct aws_mqtt_client_connection *connection,
    struct aws_http_proxy_options *proxy_options) {

    return (*connection->vtable->set_http_proxy_options_fn)(connection->impl, proxy_options);
}

int aws_mqtt_client_connection_set_host_resolution_options(
    struct aws_mqtt_client_connection *connection,
    const struct aws_host_resolution_config *host_resolution_config) {

    return (*connection->vtable->set_host_resolution_options_fn)(connection->impl, host_resolution_config);
}

int aws_mqtt_client_connection_set_reconnect_timeout(
    struct aws_mqtt_client_connection *connection,
    uint64_t min_timeout,
    uint64_t max_timeout) {

    return (*connection->vtable->set_reconnect_timeout_fn)(connection->impl, min_timeout, max_timeout);
}

int aws_mqtt_client_connection_set_connection_result_handlers(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_success_fn *on_connection_success,
    void *on_connection_success_ud,
    aws_mqtt_client_on_connection_failure_fn *on_connection_failure,
    void *on_connection_failure_ud) {

    return (*connection->vtable->set_connection_result_handlers)(
        connection->impl,
        on_connection_success,
        on_connection_success_ud,
        on_connection_failure,
        on_connection_failure_ud);
}

int aws_mqtt_client_connection_set_connection_interruption_handlers(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted,
    void *on_interrupted_ud,
    aws_mqtt_client_on_connection_resumed_fn *on_resumed,
    void *on_resumed_ud) {

    return (*connection->vtable->set_connection_interruption_handlers_fn)(
        connection->impl, on_interrupted, on_interrupted_ud, on_resumed, on_resumed_ud);
}

int aws_mqtt_client_connection_set_connection_closed_handler(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_closed_fn *on_closed,
    void *on_closed_ud) {

    return (*connection->vtable->set_connection_closed_handler_fn)(connection->impl, on_closed, on_closed_ud);
}

int aws_mqtt_client_connection_set_on_any_publish_handler(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_publish_received_fn *on_any_publish,
    void *on_any_publish_ud) {

    return (*connection->vtable->set_on_any_publish_handler_fn)(connection->impl, on_any_publish, on_any_publish_ud);
}

int aws_mqtt_client_connection_set_connection_termination_handler(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_termination_fn *on_termination,
    void *on_termination_ud) {

    return (*connection->vtable->set_connection_termination_handler_fn)(
        connection->impl, on_termination, on_termination_ud);
}

int aws_mqtt_client_connection_connect(
    struct aws_mqtt_client_connection *connection,
    const struct aws_mqtt_connection_options *connection_options) {

    return (*connection->vtable->connect_fn)(connection->impl, connection_options);
}

int aws_mqtt_client_connection_reconnect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete,
    void *userdata) {

    return (*connection->vtable->reconnect_fn)(connection->impl, on_connection_complete, userdata);
}

int aws_mqtt_client_connection_disconnect(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_client_on_disconnect_fn *on_disconnect,
    void *userdata) {

    return (*connection->vtable->disconnect_fn)(connection->impl, on_disconnect, userdata);
}

uint16_t aws_mqtt_client_connection_subscribe_multiple(
    struct aws_mqtt_client_connection *connection,
    const struct aws_array_list *topic_filters,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_ud) {

    return (*connection->vtable->subscribe_multiple_fn)(connection->impl, topic_filters, on_suback, on_suback_ud);
}

uint16_t aws_mqtt_client_connection_subscribe(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_client_publish_received_fn *on_publish,
    void *on_publish_ud,
    aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
    aws_mqtt_suback_fn *on_suback,
    void *on_suback_ud) {

    return (*connection->vtable->subscribe_fn)(
        connection->impl, topic_filter, qos, on_publish, on_publish_ud, on_ud_cleanup, on_suback, on_suback_ud);
}

uint16_t aws_mqtt_resubscribe_existing_topics(
    struct aws_mqtt_client_connection *connection,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_ud) {

    return (*connection->vtable->resubscribe_existing_topics_fn)(connection->impl, on_suback, on_suback_ud);
}

uint16_t aws_mqtt_client_connection_unsubscribe(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic_filter,
    aws_mqtt_op_complete_fn *on_unsuback,
    void *on_unsuback_ud) {

    return (*connection->vtable->unsubscribe_fn)(connection->impl, topic_filter, on_unsuback, on_unsuback_ud);
}

uint16_t aws_mqtt_client_connection_publish(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload,
    aws_mqtt_op_complete_fn *on_complete,
    void *userdata) {

    return (*connection->vtable->publish_fn)(connection->impl, topic, qos, retain, payload, on_complete, userdata);
}

int aws_mqtt_client_connection_get_stats(
    struct aws_mqtt_client_connection *connection,
    struct aws_mqtt_connection_operation_statistics *stats) {

    return (*connection->vtable->get_stats_fn)(connection->impl, stats);
}

enum aws_mqtt311_impl_type aws_mqtt_client_connection_get_impl_type(
    const struct aws_mqtt_client_connection *connection) {
    return (*connection->vtable->get_impl_type)(connection->impl);
}

uint64_t aws_mqtt_hash_uint16_t(const void *item) {
    return *(uint16_t *)item;
}

bool aws_mqtt_compare_uint16_t_eq(const void *a, const void *b) {
    return *(uint16_t *)a == *(uint16_t *)b;
}

bool aws_mqtt_byte_cursor_hash_equality(const void *a, const void *b) {
    const struct aws_byte_cursor *a_cursor = a;
    const struct aws_byte_cursor *b_cursor = b;

    return aws_byte_cursor_eq(a_cursor, b_cursor);
}

struct aws_event_loop *aws_mqtt_client_connection_get_event_loop(const struct aws_mqtt_client_connection *connection) {
    return (*connection->vtable->get_event_loop)(connection->impl);
}
