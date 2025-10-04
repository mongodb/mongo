/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_MQTT5_LISTENER_H
#define AWS_MQTT_MQTT5_LISTENER_H

#include <aws/mqtt/mqtt.h>

#include <aws/mqtt/v5/mqtt5_client.h>

AWS_PUSH_SANE_WARNING_LEVEL

/*
 * Callback signature for when an mqtt5 listener has completely destroyed itself.
 */
typedef void(aws_mqtt5_listener_termination_completion_fn)(void *complete_ctx);

/**
 * A record that tracks MQTT5 client callbacks which can be dynamically injected via a listener.
 */
struct aws_mqtt5_callback_set {
    aws_mqtt5_listener_publish_received_fn *listener_publish_received_handler;
    void *listener_publish_received_handler_user_data;

    aws_mqtt5_client_connection_event_callback_fn *lifecycle_event_handler;
    void *lifecycle_event_handler_user_data;
};

/**
 * Configuration options for MQTT5 listener objects.
 */
struct aws_mqtt5_listener_config {

    /**
     * MQTT5 client to listen to events on
     */
    struct aws_mqtt5_client *client;

    /**
     * Callbacks to invoke when events occur on the MQTT5 client
     */
    struct aws_mqtt5_callback_set listener_callbacks;

    /**
     * Listener destruction is asynchronous and thus requires a termination callback and associated user data
     * to notify the user that the listener has been fully destroyed and no further events will be received.
     */
    aws_mqtt5_listener_termination_completion_fn *termination_callback;
    void *termination_callback_user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a new MQTT5 listener object.  For as long as the listener lives, incoming publishes and lifecycle events
 * will be forwarded to the callbacks configured on the listener.
 *
 * @param allocator allocator to use
 * @param config listener configuration
 * @return a new aws_mqtt5_listener object
 */
AWS_MQTT_API struct aws_mqtt5_listener *aws_mqtt5_listener_new(
    struct aws_allocator *allocator,
    struct aws_mqtt5_listener_config *config);

/**
 * Adds a reference to an mqtt5 listener.
 *
 * @param listener listener to add a reference to
 * @return the listener object
 */
AWS_MQTT_API struct aws_mqtt5_listener *aws_mqtt5_listener_acquire(struct aws_mqtt5_listener *listener);

/**
 * Removes a reference to an mqtt5 listener.  When the reference count drops to zero, the listener's asynchronous
 * destruction will be started.
 *
 * @param listener listener to remove a reference from
 * @return NULL
 */
AWS_MQTT_API struct aws_mqtt5_listener *aws_mqtt5_listener_release(struct aws_mqtt5_listener *listener);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_MQTT_MQTT5_LISTENER_H */
