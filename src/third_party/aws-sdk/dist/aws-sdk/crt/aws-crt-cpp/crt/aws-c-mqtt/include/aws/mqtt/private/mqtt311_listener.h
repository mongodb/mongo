/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_MQTT311_LISTENER_H
#define AWS_MQTT_MQTT311_LISTENER_H

#include <aws/mqtt/mqtt.h>

#include <aws/common/rw_lock.h>
#include <aws/mqtt/client.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Callback signature for when an mqtt311 listener has completely destroyed itself.
 */
typedef void(aws_mqtt311_listener_termination_completion_fn)(void *complete_ctx);

/**
 * A record that tracks MQTT311 client connection callbacks which can be dynamically injected via a listener.
 *
 * All the callbacks that are supported here are invoked only on the 311 connection's event loop.  With the
 * add/remove callback set also on the event loop, everything is correctly serialized without data races.
 *
 * If binding additional callbacks, they must only be invoked from the connection's event loop.
 *
 * We only listen to connection-success because the only connection-level event we care about is a failure
 * to rejoin a session (which invalidates all subscriptions that were considered valid)
 */
struct aws_mqtt311_callback_set {

    /* Called from s_packet_handler_publish which is event-loop invoked */
    aws_mqtt_client_publish_received_fn *publish_received_handler;

    /* Called from s_packet_handler_connack which is event-loop invoked */
    aws_mqtt_client_on_connection_success_fn *connection_success_handler;

    /* Called from s_mqtt_client_shutdown which is event-loop invoked */
    aws_mqtt_client_on_connection_interrupted_fn *connection_interrupted_handler;

    /* Called from s_mqtt_client_shutdown which is event-loop invoked */
    aws_mqtt_client_on_disconnect_fn *disconnect_handler;

    void *user_data;
};

/**
 * An internal type for managing chains of callbacks attached to an mqtt311 client connection.  Supports chains for
 * lifecycle events and incoming publish packet handling.
 *
 * Assumed to be owned and used only by an MQTT311 client connection.
 */
struct aws_mqtt311_callback_set_manager {
    struct aws_allocator *allocator;

    struct aws_mqtt_client_connection *connection;

    struct aws_linked_list callback_set_entries;

    uint64_t next_callback_set_entry_id;
};

/**
 * Configuration options for MQTT311 listener objects.
 */
struct aws_mqtt311_listener_config {

    /**
     * MQTT311 client connection to listen to events on
     */
    struct aws_mqtt_client_connection *connection;

    /**
     * Callbacks to invoke when events occur on the MQTT311 client connection
     */
    struct aws_mqtt311_callback_set listener_callbacks;

    /**
     * Listener destruction is asynchronous and thus requires a termination callback and associated user data
     * to notify the user that the listener has been fully destroyed and no further events will be received.
     */
    aws_mqtt311_listener_termination_completion_fn *termination_callback;
    void *termination_callback_user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a new MQTT311 listener object.  For as long as the listener lives, incoming publishes and lifecycle events
 * will be forwarded to the callbacks configured on the listener.
 *
 * @param allocator allocator to use
 * @param config listener configuration
 * @return a new aws_mqtt311_listener object
 */
AWS_MQTT_API struct aws_mqtt311_listener *aws_mqtt311_listener_new(
    struct aws_allocator *allocator,
    struct aws_mqtt311_listener_config *config);

/**
 * Adds a reference to an mqtt311 listener.
 *
 * @param listener listener to add a reference to
 * @return the listener object
 */
AWS_MQTT_API struct aws_mqtt311_listener *aws_mqtt311_listener_acquire(struct aws_mqtt311_listener *listener);

/**
 * Removes a reference to an mqtt311 listener.  When the reference count drops to zero, the listener's asynchronous
 * destruction will be started.
 *
 * @param listener listener to remove a reference from
 * @return NULL
 */
AWS_MQTT_API struct aws_mqtt311_listener *aws_mqtt311_listener_release(struct aws_mqtt311_listener *listener);

/**
 * Initializes a callback set manager
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_init(
    struct aws_mqtt311_callback_set_manager *manager,
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection *connection);

/**
 * Cleans up a callback set manager.
 *
 * aws_mqtt311_callback_set_manager_init must have been previously called or this will crash.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_clean_up(struct aws_mqtt311_callback_set_manager *manager);

/**
 * Adds a callback set to the front of the handler chain.  Returns an integer id that can be used to selectively
 * remove the callback set from the manager.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
uint64_t aws_mqtt311_callback_set_manager_push_front(
    struct aws_mqtt311_callback_set_manager *manager,
    struct aws_mqtt311_callback_set *callback_set);

/**
 * Removes a callback set from the handler chain.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_remove(
    struct aws_mqtt311_callback_set_manager *manager,
    uint64_t callback_set_id);

/**
 * Walks the incoming publish handler chain for an MQTT311 connection, invoking each in sequence.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_on_publish_received(
    struct aws_mqtt311_callback_set_manager *manager,
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain);

/**
 * Invokes a connection success event on each listener in the manager's collection of callback sets.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_on_connection_success(
    struct aws_mqtt311_callback_set_manager *manager,
    enum aws_mqtt_connect_return_code return_code,
    bool rejoined_session);

/**
 * Invokes a connection interrupted event on each listener in the manager's collection of callback sets.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_on_connection_interrupted(
    struct aws_mqtt311_callback_set_manager *manager,
    int error_code);

/**
 * Invokes a disconnection event on each listener in the manager's collection of callback sets.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt311_callback_set_manager_on_disconnect(struct aws_mqtt311_callback_set_manager *manager);

AWS_EXTERN_C_END

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_MQTT_MQTT311_LISTENER_H */
