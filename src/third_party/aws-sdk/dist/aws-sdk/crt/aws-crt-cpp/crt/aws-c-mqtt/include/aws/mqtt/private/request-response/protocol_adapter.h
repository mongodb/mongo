#ifndef AWS_MQTT_PRIVATE_REQUEST_RESPONSE_PROTOCOL_ADAPTER_H
#define AWS_MQTT_PRIVATE_REQUEST_RESPONSE_PROTOCOL_ADAPTER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/exports.h>
#include <aws/mqtt/mqtt.h>

#include <aws/common/byte_buf.h>

struct aws_allocator;
struct aws_event_loop;
struct aws_mqtt_client_connection;
struct aws_mqtt5_client;

/*
 * The request-response protocol adapter is a translation layer that sits between the request-response native client
 * implementation and a protocol client capable of subscribing, unsubscribing, and publishing MQTT messages.
 * Valid protocol clients include the CRT MQTT5 client, the CRT MQTT311 client, and an eventstream RPC connection
 * that belongs to a Greengrass IPC client.  Each of these protocol clients has a different (or even implicit)
 * contract for carrying out pub-sub operations.  The protocol adapter abstracts these details with a simple,
 * minimal interface based on the requirements identified in the request-response design documents.
 */

/*
 * Minimal MQTT subscribe options
 */
struct aws_protocol_adapter_subscribe_options {
    struct aws_byte_cursor topic_filter;
    uint32_t ack_timeout_seconds;
};

/*
 * Minimal MQTT unsubscribe options
 */
struct aws_protocol_adapter_unsubscribe_options {
    struct aws_byte_cursor topic_filter;
    uint32_t ack_timeout_seconds;
};

/*
 * Minimal MQTT publish options
 */
struct aws_protocol_adapter_publish_options {
    struct aws_byte_cursor topic;
    struct aws_byte_cursor payload;
    uint32_t ack_timeout_seconds;

    /*
     * Invoked on success/failure of the publish itself.  Our implementations use QoS1 which means that success
     * will be on puback receipt.
     */
    void (*completion_callback_fn)(int, void *);

    /*
     * User data to pass in when invoking the completion callback
     */
    void *user_data;
};

/*
 * Describes the type of subscription event (relative to a topic filter)
 */
enum aws_protocol_adapter_subscription_event_type {
    AWS_PASET_SUBSCRIBE,
    AWS_PASET_UNSUBSCRIBE,
};

/*
 * An event emitted by the protocol adapter when a subscribe or unsubscribe is completed by the adapted protocol
 * client.
 */
struct aws_protocol_adapter_subscription_event {
    struct aws_byte_cursor topic_filter;
    enum aws_protocol_adapter_subscription_event_type event_type;
    int error_code;
    bool retryable;
};

/*
 * An event emitted by the protocol adapter whenever a publish is received by the protocol client.  This will
 * potentially include messages that are completely unrelated to MQTT request-response.  The topic is the first
 * thing that should be checked for relevance.
 */
struct aws_protocol_adapter_incoming_publish_event {
    struct aws_byte_cursor topic;
    struct aws_byte_cursor payload;
};

enum aws_protocol_adapter_connection_event_type {
    AWS_PACET_CONNECTED,
    AWS_PACET_DISCONNECTED,
};

/*
 * An event emitted by the protocol adapter whenever the protocol client's connection status changes
 */
struct aws_protocol_adapter_connection_event {
    enum aws_protocol_adapter_connection_event_type event_type;
    bool joined_session;
};

typedef void(aws_protocol_adapter_subscription_event_fn)(
    const struct aws_protocol_adapter_subscription_event *event,
    void *user_data);

typedef void(aws_protocol_adapter_incoming_publish_fn)(
    const struct aws_protocol_adapter_incoming_publish_event *publish,
    void *user_data);

typedef void(aws_protocol_adapter_terminate_callback_fn)(void *user_data);

typedef void(aws_protocol_adapter_connection_event_fn)(
    const struct aws_protocol_adapter_connection_event *event,
    void *user_data);

/*
 * Set of callbacks invoked by the protocol adapter.  These must all be set.
 */
struct aws_mqtt_protocol_adapter_options {
    aws_protocol_adapter_subscription_event_fn *subscription_event_callback;
    aws_protocol_adapter_incoming_publish_fn *incoming_publish_callback;
    aws_protocol_adapter_terminate_callback_fn *terminate_callback;
    aws_protocol_adapter_connection_event_fn *connection_event_callback;

    /*
     * User data to pass into all singleton protocol adapter callbacks.  Likely either the request-response client
     * or the subscription manager component of the request-response client.
     */
    void *user_data;
};

struct aws_mqtt_protocol_adapter_vtable {

    void (*aws_mqtt_protocol_adapter_destroy_fn)(void *);

    int (*aws_mqtt_protocol_adapter_subscribe_fn)(void *, struct aws_protocol_adapter_subscribe_options *);

    int (*aws_mqtt_protocol_adapter_unsubscribe_fn)(void *, struct aws_protocol_adapter_unsubscribe_options *);

    int (*aws_mqtt_protocol_adapter_publish_fn)(void *, struct aws_protocol_adapter_publish_options *);

    bool (*aws_mqtt_protocol_adapter_is_connected_fn)(void *);

    struct aws_event_loop *(*aws_mqtt_protocol_adapter_get_event_loop_fn)(void *);
};

struct aws_mqtt_protocol_adapter {
    const struct aws_mqtt_protocol_adapter_vtable *vtable;
    void *impl;
};

AWS_EXTERN_C_BEGIN

/*
 * Creates a new request-response protocol adapter from an MQTT311 client
 */
AWS_MQTT_API struct aws_mqtt_protocol_adapter *aws_mqtt_protocol_adapter_new_from_311(
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter_options *options,
    struct aws_mqtt_client_connection *connection);

/*
 * Creates a new request-response protocol adapter from an MQTT5 client
 */
AWS_MQTT_API struct aws_mqtt_protocol_adapter *aws_mqtt_protocol_adapter_new_from_5(
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter_options *options,
    struct aws_mqtt5_client *client);

/*
 * Destroys a request-response protocol adapter.  Destruction is an asynchronous process and the caller must
 * wait for the termination callback to be invoked before assuming that no further callbacks will be invoked.
 */
AWS_MQTT_API void aws_mqtt_protocol_adapter_destroy(struct aws_mqtt_protocol_adapter *adapter);

/*
 * Asks the adapted protocol client to perform an MQTT subscribe operation
 */
AWS_MQTT_API int aws_mqtt_protocol_adapter_subscribe(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_subscribe_options *options);

/*
 * Asks the adapted protocol client to perform an MQTT unsubscribe operation
 */
AWS_MQTT_API int aws_mqtt_protocol_adapter_unsubscribe(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_unsubscribe_options *options);

/*
 * Asks the adapted protocol client to perform an MQTT publish operation
 */
AWS_MQTT_API int aws_mqtt_protocol_adapter_publish(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_publish_options *options);

/*
 * Synchronously checks the connection state of the adapted protocol client.  May only be called from the
 * protocol client's event loop.
 */
AWS_MQTT_API bool aws_mqtt_protocol_adapter_is_connected(struct aws_mqtt_protocol_adapter *adapter);

/*
 * Returns the event loop that the protocol client is bound to.
 */
AWS_MQTT_API struct aws_event_loop *aws_mqtt_protocol_adapter_get_event_loop(struct aws_mqtt_protocol_adapter *adapter);

AWS_MQTT_API const char *aws_protocol_adapter_subscription_event_type_to_c_str(
    enum aws_protocol_adapter_subscription_event_type type);

AWS_MQTT_API const char *aws_protocol_adapter_connection_event_type_to_c_str(
    enum aws_protocol_adapter_connection_event_type type);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_PRIVATE_REQUEST_RESPONSE_PROTOCOL_ADAPTER_H */
