/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/v5/mqtt5_listener.h>

#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/io/event_loop.h>
#include <aws/mqtt/private/v5/mqtt5_client_impl.h>

struct aws_mqtt5_listener {
    struct aws_allocator *allocator;

    struct aws_ref_count ref_count;

    struct aws_mqtt5_listener_config config;

    uint64_t callback_set_id;

    struct aws_task initialize_task;
    struct aws_task terminate_task;
};

static void s_mqtt5_listener_destroy(struct aws_mqtt5_listener *listener) {

    aws_mqtt5_client_release(listener->config.client);

    aws_mqtt5_listener_termination_completion_fn *termination_callback = listener->config.termination_callback;
    void *temination_callback_user_data = listener->config.termination_callback_user_data;

    aws_mem_release(listener->allocator, listener);

    if (termination_callback != NULL) {
        (*termination_callback)(temination_callback_user_data);
    }
}

static void s_mqtt5_listener_initialize_task_fn(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;

    struct aws_mqtt5_listener *listener = arg;

    if (task_status == AWS_TASK_STATUS_RUN_READY) {
        listener->callback_set_id = aws_mqtt5_callback_set_manager_push_front(
            &listener->config.client->callback_manager, &listener->config.listener_callbacks);
        AWS_LOGF_INFO(
            AWS_LS_MQTT5_GENERAL,
            "id=%p: Mqtt5 Listener initialized, listener id=%p",
            (void *)listener->config.client,
            (void *)listener);
        aws_mqtt5_listener_release(listener);
    } else {
        s_mqtt5_listener_destroy(listener);
    }
}

static void s_mqtt5_listener_terminate_task_fn(struct aws_task *task, void *arg, enum aws_task_status task_status) {
    (void)task;

    struct aws_mqtt5_listener *listener = arg;

    if (task_status == AWS_TASK_STATUS_RUN_READY) {
        aws_mqtt5_callback_set_manager_remove(&listener->config.client->callback_manager, listener->callback_set_id);
    }

    AWS_LOGF_INFO(
        AWS_LS_MQTT5_GENERAL,
        "id=%p: Mqtt5 Listener terminated, listener id=%p",
        (void *)listener->config.client,
        (void *)listener);

    s_mqtt5_listener_destroy(listener);
}

static void s_aws_mqtt5_listener_on_zero_ref_count(void *context) {
    struct aws_mqtt5_listener *listener = context;

    aws_event_loop_schedule_task_now(listener->config.client->loop, &listener->terminate_task);
}

struct aws_mqtt5_listener *aws_mqtt5_listener_new(
    struct aws_allocator *allocator,
    struct aws_mqtt5_listener_config *config) {
    if (config->client == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_mqtt5_listener *listener = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_listener));

    listener->allocator = allocator;
    listener->config = *config;

    aws_mqtt5_client_acquire(config->client);
    aws_ref_count_init(&listener->ref_count, listener, s_aws_mqtt5_listener_on_zero_ref_count);

    aws_task_init(&listener->initialize_task, s_mqtt5_listener_initialize_task_fn, listener, "Mqtt5ListenerInitialize");
    aws_task_init(&listener->terminate_task, s_mqtt5_listener_terminate_task_fn, listener, "Mqtt5ListenerTerminate");

    aws_mqtt5_listener_acquire(listener);
    aws_event_loop_schedule_task_now(config->client->loop, &listener->initialize_task);

    return listener;
}

struct aws_mqtt5_listener *aws_mqtt5_listener_acquire(struct aws_mqtt5_listener *listener) {
    if (listener != NULL) {
        aws_ref_count_acquire(&listener->ref_count);
    }

    return listener;
}

struct aws_mqtt5_listener *aws_mqtt5_listener_release(struct aws_mqtt5_listener *listener) {
    if (listener != NULL) {
        aws_ref_count_release(&listener->ref_count);
    }

    return NULL;
}
