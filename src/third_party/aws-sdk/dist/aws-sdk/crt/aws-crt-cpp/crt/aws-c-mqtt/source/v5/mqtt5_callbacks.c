/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/v5/mqtt5_callbacks.h>

#include <aws/io/event_loop.h>
#include <aws/mqtt/private/v5/mqtt5_client_impl.h>
#include <aws/mqtt/v5/mqtt5_listener.h>

#include <inttypes.h>

struct aws_mqtt5_callback_set_entry {
    struct aws_allocator *allocator;

    struct aws_linked_list_node node;

    uint64_t id;

    struct aws_mqtt5_callback_set callbacks;
};

void aws_mqtt5_callback_set_manager_init(
    struct aws_mqtt5_callback_set_manager *manager,
    struct aws_mqtt5_client *client) {

    manager->client = client; /* no need to ref count, it's assumed to be owned by the client */
    manager->next_callback_set_entry_id = 1;

    aws_linked_list_init(&manager->callback_set_entries);
}

void aws_mqtt5_callback_set_manager_clean_up(struct aws_mqtt5_callback_set_manager *manager) {
    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt5_callback_set_entry *entry = AWS_CONTAINER_OF(node, struct aws_mqtt5_callback_set_entry, node);
        node = aws_linked_list_next(node);

        aws_linked_list_remove(&entry->node);
        aws_mem_release(entry->allocator, entry);
    }
}

static struct aws_mqtt5_callback_set_entry *s_new_callback_set_entry(
    struct aws_mqtt5_callback_set_manager *manager,
    struct aws_mqtt5_callback_set *callback_set) {
    struct aws_mqtt5_callback_set_entry *entry =
        aws_mem_calloc(manager->client->allocator, 1, sizeof(struct aws_mqtt5_callback_set_entry));

    entry->allocator = manager->client->allocator;
    entry->id = manager->next_callback_set_entry_id++;
    entry->callbacks = *callback_set;

    AWS_LOGF_INFO(
        AWS_LS_MQTT5_GENERAL,
        "id=%p: callback manager created new entry :%" PRIu64,
        (void *)manager->client,
        entry->id);

    return entry;
}

uint64_t aws_mqtt5_callback_set_manager_push_front(
    struct aws_mqtt5_callback_set_manager *manager,
    struct aws_mqtt5_callback_set *callback_set) {

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(manager->client->loop));

    struct aws_mqtt5_callback_set_entry *entry = s_new_callback_set_entry(manager, callback_set);

    aws_linked_list_push_front(&manager->callback_set_entries, &entry->node);

    return entry->id;
}

void aws_mqtt5_callback_set_manager_remove(struct aws_mqtt5_callback_set_manager *manager, uint64_t callback_set_id) {

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(manager->client->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt5_callback_set_entry *entry = AWS_CONTAINER_OF(node, struct aws_mqtt5_callback_set_entry, node);
        node = aws_linked_list_next(node);

        if (entry->id == callback_set_id) {
            aws_linked_list_remove(&entry->node);

            AWS_LOGF_INFO(
                AWS_LS_MQTT5_GENERAL,
                "id=%p: callback manager removed entry id=%" PRIu64,
                (void *)manager->client,
                entry->id);
            aws_mem_release(entry->allocator, entry);
            return;
        }
    }
    AWS_LOGF_INFO(
        AWS_LS_MQTT5_GENERAL,
        "id=%p: callback manager failed to remove entry id=%" PRIu64 ", callback set id not found.",
        (void *)manager->client,
        callback_set_id);
}

void aws_mqtt5_callback_set_manager_on_publish_received(
    struct aws_mqtt5_callback_set_manager *manager,
    const struct aws_mqtt5_packet_publish_view *publish_view) {

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(manager->client->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt5_callback_set_entry *entry = AWS_CONTAINER_OF(node, struct aws_mqtt5_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt5_callback_set *callback_set = &entry->callbacks;
        if (callback_set->listener_publish_received_handler != NULL) {
            bool handled = (*callback_set->listener_publish_received_handler)(
                publish_view, callback_set->listener_publish_received_handler_user_data);
            if (handled) {
                return;
            }
        }
    }

    if (manager->client->config->publish_received_handler != NULL) {
        (*manager->client->config->publish_received_handler)(
            publish_view, manager->client->config->publish_received_handler_user_data);
    }
}

void aws_mqtt5_callback_set_manager_on_lifecycle_event(
    struct aws_mqtt5_callback_set_manager *manager,
    const struct aws_mqtt5_client_lifecycle_event *lifecycle_event) {

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(manager->client->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt5_callback_set_entry *entry = AWS_CONTAINER_OF(node, struct aws_mqtt5_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt5_callback_set *callback_set = &entry->callbacks;

        if (callback_set->lifecycle_event_handler != NULL) {
            struct aws_mqtt5_client_lifecycle_event listener_copy = *lifecycle_event;
            listener_copy.user_data = callback_set->lifecycle_event_handler_user_data;

            (*callback_set->lifecycle_event_handler)(&listener_copy);
        }
    }

    struct aws_mqtt5_client_lifecycle_event client_copy = *lifecycle_event;
    client_copy.user_data = manager->client->config->lifecycle_event_handler_user_data;

    if (manager->client->config->lifecycle_event_handler != NULL) {
        (*manager->client->config->lifecycle_event_handler)(&client_copy);
    }
}
