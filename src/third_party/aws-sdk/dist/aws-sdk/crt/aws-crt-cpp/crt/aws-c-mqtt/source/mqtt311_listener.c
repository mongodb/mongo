/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/mqtt311_listener.h>

#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/io/event_loop.h>
#include <aws/mqtt/private/client_impl.h>
#include <aws/mqtt/private/client_impl_shared.h>

#include <inttypes.h>

static struct aws_event_loop *s_mqtt_client_connection_get_event_loop(
    const struct aws_mqtt_client_connection *connection) {
    AWS_FATAL_ASSERT(aws_mqtt_client_connection_get_impl_type(connection) == AWS_MQTT311_IT_311_CONNECTION);

    struct aws_mqtt_client_connection_311_impl *connection_impl = connection->impl;

    return connection_impl->loop;
}

struct aws_mqtt311_listener {
    struct aws_allocator *allocator;

    struct aws_ref_count ref_count;

    struct aws_mqtt311_listener_config config;

    uint64_t callback_set_id;

    struct aws_task initialize_task;
    struct aws_task terminate_task;
};

static void s_mqtt311_listener_destroy(struct aws_mqtt311_listener *listener) {

    aws_mqtt_client_connection_release(listener->config.connection);

    aws_mqtt311_listener_termination_completion_fn *termination_callback = listener->config.termination_callback;
    void *termination_callback_user_data = listener->config.termination_callback_user_data;

    aws_mem_release(listener->allocator, listener);

    if (termination_callback != NULL) {
        (*termination_callback)(termination_callback_user_data);
    }
}

static void s_mqtt311_listener_initialize_task_fn(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;

    struct aws_mqtt311_listener *listener = arg;

    if (task_status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_mqtt_client_connection_311_impl *connection_impl = listener->config.connection->impl;
        listener->callback_set_id = aws_mqtt311_callback_set_manager_push_front(
            &connection_impl->callback_manager, &listener->config.listener_callbacks);
        AWS_LOGF_INFO(
            AWS_LS_MQTT_GENERAL,
            "id=%p: Mqtt311 Listener initialized, listener id=%p",
            (void *)listener->config.connection,
            (void *)listener);
        aws_mqtt311_listener_release(listener);
    } else {
        s_mqtt311_listener_destroy(listener);
    }
}

static void s_mqtt311_listener_terminate_task_fn(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;

    struct aws_mqtt311_listener *listener = arg;

    if (task_status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_mqtt_client_connection_311_impl *connection_impl = listener->config.connection->impl;
        aws_mqtt311_callback_set_manager_remove(&connection_impl->callback_manager, listener->callback_set_id);
    }

    AWS_LOGF_INFO(
        AWS_LS_MQTT_GENERAL,
        "id=%p: Mqtt311 Listener terminated, listener id=%p",
        (void *)listener->config.connection,
        (void *)listener);

    s_mqtt311_listener_destroy(listener);
}

static void s_aws_mqtt311_listener_on_zero_ref_count(void *context) {
    struct aws_mqtt311_listener *listener = context;

    aws_event_loop_schedule_task_now(
        s_mqtt_client_connection_get_event_loop(listener->config.connection), &listener->terminate_task);
}

struct aws_mqtt311_listener *aws_mqtt311_listener_new(
    struct aws_allocator *allocator,
    struct aws_mqtt311_listener_config *config) {
    if (config->connection == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (aws_mqtt_client_connection_get_impl_type(config->connection) != AWS_MQTT311_IT_311_CONNECTION) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_mqtt311_listener *listener = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt311_listener));

    listener->allocator = allocator;
    listener->config = *config;

    aws_mqtt_client_connection_acquire(config->connection);
    aws_ref_count_init(&listener->ref_count, listener, s_aws_mqtt311_listener_on_zero_ref_count);

    aws_task_init(
        &listener->initialize_task, s_mqtt311_listener_initialize_task_fn, listener, "Mqtt311ListenerInitialize");
    aws_task_init(
        &listener->terminate_task, s_mqtt311_listener_terminate_task_fn, listener, "Mqtt311ListenerTerminate");

    aws_mqtt311_listener_acquire(listener);
    aws_event_loop_schedule_task_now(
        s_mqtt_client_connection_get_event_loop(config->connection), &listener->initialize_task);

    return listener;
}

struct aws_mqtt311_listener *aws_mqtt311_listener_acquire(struct aws_mqtt311_listener *listener) {
    if (listener != NULL) {
        aws_ref_count_acquire(&listener->ref_count);
    }

    return listener;
}

struct aws_mqtt311_listener *aws_mqtt311_listener_release(struct aws_mqtt311_listener *listener) {
    if (listener != NULL) {
        aws_ref_count_release(&listener->ref_count);
    }

    return NULL;
}

struct aws_mqtt311_callback_set_entry {
    struct aws_allocator *allocator;

    struct aws_linked_list_node node;

    uint64_t id;

    struct aws_mqtt311_callback_set callbacks;
};

void aws_mqtt311_callback_set_manager_init(
    struct aws_mqtt311_callback_set_manager *manager,
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection *connection) {

    manager->allocator = allocator;
    manager->connection = connection; /* no need to ref count, this is assumed to be owned by the client connection */
    manager->next_callback_set_entry_id = 1;

    aws_linked_list_init(&manager->callback_set_entries);
}

void aws_mqtt311_callback_set_manager_clean_up(struct aws_mqtt311_callback_set_manager *manager) {
    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        aws_linked_list_remove(&entry->node);
        aws_mem_release(entry->allocator, entry);
    }
}

static struct aws_mqtt311_callback_set_entry *s_new_311_callback_set_entry(
    struct aws_mqtt311_callback_set_manager *manager,
    struct aws_mqtt311_callback_set *callback_set) {
    struct aws_mqtt311_callback_set_entry *entry =
        aws_mem_calloc(manager->allocator, 1, sizeof(struct aws_mqtt311_callback_set_entry));

    entry->allocator = manager->allocator;
    entry->id = manager->next_callback_set_entry_id++;
    entry->callbacks = *callback_set;

    AWS_LOGF_INFO(
        AWS_LS_MQTT_GENERAL,
        "id=%p: MQTT311 callback manager created new entry id=%" PRIu64,
        (void *)manager->connection,
        entry->id);

    return entry;
}

uint64_t aws_mqtt311_callback_set_manager_push_front(
    struct aws_mqtt311_callback_set_manager *manager,
    struct aws_mqtt311_callback_set *callback_set) {

    AWS_FATAL_ASSERT(
        aws_event_loop_thread_is_callers_thread(s_mqtt_client_connection_get_event_loop(manager->connection)));

    struct aws_mqtt311_callback_set_entry *entry = s_new_311_callback_set_entry(manager, callback_set);

    aws_linked_list_push_front(&manager->callback_set_entries, &entry->node);

    return entry->id;
}

void aws_mqtt311_callback_set_manager_remove(
    struct aws_mqtt311_callback_set_manager *manager,
    uint64_t callback_set_id) {

    AWS_FATAL_ASSERT(
        aws_event_loop_thread_is_callers_thread(s_mqtt_client_connection_get_event_loop(manager->connection)));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        if (entry->id == callback_set_id) {
            aws_linked_list_remove(&entry->node);

            AWS_LOGF_INFO(
                AWS_LS_MQTT_GENERAL,
                "id=%p: MQTT311 callback manager removed entry id=%" PRIu64,
                (void *)manager->connection,
                entry->id);
            aws_mem_release(entry->allocator, entry);
            return;
        }
    }
    AWS_LOGF_INFO(
        AWS_LS_MQTT_GENERAL,
        "id=%p: MQTT311 callback manager failed to remove entry id=%" PRIu64 ", callback set id not found.",
        (void *)manager->connection,
        callback_set_id);
}

void aws_mqtt311_callback_set_manager_on_publish_received(
    struct aws_mqtt311_callback_set_manager *manager,
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain) {

    struct aws_mqtt_client_connection_311_impl *connection_impl = manager->connection->impl;
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(connection_impl->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt311_callback_set *callback_set = &entry->callbacks;
        if (callback_set->publish_received_handler != NULL) {
            (*callback_set->publish_received_handler)(
                manager->connection, topic, payload, dup, qos, retain, callback_set->user_data);
        }
    }
}

void aws_mqtt311_callback_set_manager_on_connection_success(
    struct aws_mqtt311_callback_set_manager *manager,
    enum aws_mqtt_connect_return_code return_code,
    bool rejoined_session) {

    struct aws_mqtt_client_connection_311_impl *connection_impl = manager->connection->impl;
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(connection_impl->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt311_callback_set *callback_set = &entry->callbacks;
        if (callback_set->connection_success_handler != NULL) {
            (*callback_set->connection_success_handler)(
                manager->connection, return_code, rejoined_session, callback_set->user_data);
        }
    }
}

void aws_mqtt311_callback_set_manager_on_connection_interrupted(
    struct aws_mqtt311_callback_set_manager *manager,
    int error_code) {

    struct aws_mqtt_client_connection_311_impl *connection_impl = manager->connection->impl;
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(connection_impl->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt311_callback_set *callback_set = &entry->callbacks;
        if (callback_set->connection_interrupted_handler != NULL) {
            (*callback_set->connection_interrupted_handler)(manager->connection, error_code, callback_set->user_data);
        }
    }
}

void aws_mqtt311_callback_set_manager_on_disconnect(struct aws_mqtt311_callback_set_manager *manager) {

    struct aws_mqtt_client_connection_311_impl *connection_impl = manager->connection->impl;
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(connection_impl->loop));

    struct aws_linked_list_node *node = aws_linked_list_begin(&manager->callback_set_entries);
    while (node != aws_linked_list_end(&manager->callback_set_entries)) {
        struct aws_mqtt311_callback_set_entry *entry =
            AWS_CONTAINER_OF(node, struct aws_mqtt311_callback_set_entry, node);
        node = aws_linked_list_next(node);

        struct aws_mqtt311_callback_set *callback_set = &entry->callbacks;
        if (callback_set->disconnect_handler != NULL) {
            (*callback_set->disconnect_handler)(manager->connection, callback_set->user_data);
        }
    }
}
