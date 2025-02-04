/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef AWS_MQTT_MQTT5_CALLBACKS_H
#define AWS_MQTT_MQTT5_CALLBACKS_H

#include <aws/mqtt/mqtt.h>

#include <aws/common/linked_list.h>
#include <aws/mqtt/v5/mqtt5_client.h>

struct aws_mqtt5_callback_set;

/*
 * An internal type for managing chains of callbacks attached to an mqtt5 client.  Supports chains for
 * lifecycle event handling and incoming publish packet handling.
 *
 * Assumed to be owned and used only by an MQTT5 client.
 */
struct aws_mqtt5_callback_set_manager {
    struct aws_mqtt5_client *client;

    struct aws_linked_list callback_set_entries;

    uint64_t next_callback_set_entry_id;
};

AWS_EXTERN_C_BEGIN

/*
 * Initializes a callback set manager
 */
AWS_MQTT_API
void aws_mqtt5_callback_set_manager_init(
    struct aws_mqtt5_callback_set_manager *manager,
    struct aws_mqtt5_client *client);

/*
 * Cleans up a callback set manager.
 *
 * aws_mqtt5_callback_set_manager_init must have been previously called or this will crash.
 */
AWS_MQTT_API
void aws_mqtt5_callback_set_manager_clean_up(struct aws_mqtt5_callback_set_manager *manager);

/*
 * Adds a callback set to the front of the handler chain.  Returns an integer id that can be used to selectively
 * remove the callback set from the manager.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
uint64_t aws_mqtt5_callback_set_manager_push_front(
    struct aws_mqtt5_callback_set_manager *manager,
    struct aws_mqtt5_callback_set *callback_set);

/*
 * Removes a callback set from the handler chain.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt5_callback_set_manager_remove(struct aws_mqtt5_callback_set_manager *manager, uint64_t callback_set_id);

/*
 * Walks the handler chain for an MQTT5 client's incoming publish messages.  The chain's callbacks will be invoked
 * until either the end is reached or one of the callbacks returns true.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt5_callback_set_manager_on_publish_received(
    struct aws_mqtt5_callback_set_manager *manager,
    const struct aws_mqtt5_packet_publish_view *publish_view);

/*
 * Walks the handler chain for an MQTT5 client's lifecycle events.
 *
 * May only be called on the client's event loop thread.
 */
AWS_MQTT_API
void aws_mqtt5_callback_set_manager_on_lifecycle_event(
    struct aws_mqtt5_callback_set_manager *manager,
    const struct aws_mqtt5_client_lifecycle_event *lifecycle_event);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_MQTT5_CALLBACKS_H */
