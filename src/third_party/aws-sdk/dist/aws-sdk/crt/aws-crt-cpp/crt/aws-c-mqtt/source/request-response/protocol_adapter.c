/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/request-response/protocol_adapter.h>

#include <aws/common/clock.h>
#include <aws/io/event_loop.h>
#include <aws/mqtt/private/client_impl.h>
#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/mqtt311_listener.h>
#include <aws/mqtt/private/v5/mqtt5_client_impl.h>
#include <aws/mqtt/private/v5/mqtt5_to_mqtt3_adapter_impl.h>
#include <aws/mqtt/v5/mqtt5_client.h>
#include <aws/mqtt/v5/mqtt5_listener.h>

/*
 * Basic API contract
 *
 * Invariant 1: Subscribe is only called from the RR subscription manager when going from 0 to 1 pending operations
 * Invariant 2: Unsubscribe is only called from the RR subscription manager when there are 0 pending operations, not
 *   necessarily on the exact transition to zero though.
 *
 * Additional Notes
 *
 * Entries are not tracked with the exception of the eventstream impl which needs the stream handles to close.
 *
 * A subscribe failure does not trigger an unsubscribe, a status event.
 *
 * The sub manager is responsible for calling Unsubscribe on all its entries when shutting down
 * (before releasing hold of the adapter).
 *
 * Retries, when appropriate, are the responsibility of the caller.
 */

enum aws_mqtt_protocol_adapter_operation_type {
    AMPAOT_SUBSCRIBE_UNSUBSCRIBE,
    AMPAOT_PUBLISH,
};

struct aws_mqtt_protocol_adapter_sub_unsub_data {
    struct aws_byte_buf topic_filter;
};

struct aws_mqtt_protocol_adapter_publish_data {
    void (*completion_callback_fn)(int, void *);
    void *user_data;
};

struct aws_mqtt_protocol_adapter_operation_userdata {
    struct aws_allocator *allocator;

    struct aws_linked_list_node node;
    void *adapter;

    enum aws_mqtt_protocol_adapter_operation_type operation_type;

    union {
        struct aws_mqtt_protocol_adapter_sub_unsub_data sub_unsub_data;
        struct aws_mqtt_protocol_adapter_publish_data publish_data;
    } operation_data;
};

static struct aws_mqtt_protocol_adapter_operation_userdata *s_aws_mqtt_protocol_adapter_sub_unsub_data_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor topic_filter,
    void *adapter) {

    struct aws_mqtt_protocol_adapter_operation_userdata *subscribe_data =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_protocol_adapter_operation_userdata));

    subscribe_data->allocator = allocator;
    subscribe_data->operation_type = AMPAOT_SUBSCRIBE_UNSUBSCRIBE;
    subscribe_data->adapter = adapter;
    aws_byte_buf_init_copy_from_cursor(
        &subscribe_data->operation_data.sub_unsub_data.topic_filter, allocator, topic_filter);

    return subscribe_data;
}

static struct aws_mqtt_protocol_adapter_operation_userdata *s_aws_mqtt_protocol_adapter_publish_data_new(
    struct aws_allocator *allocator,
    const struct aws_protocol_adapter_publish_options *publish_options,
    void *adapter) {

    struct aws_mqtt_protocol_adapter_operation_userdata *publish_data =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_protocol_adapter_operation_userdata));

    publish_data->allocator = allocator;
    publish_data->operation_type = AMPAOT_PUBLISH;
    publish_data->adapter = adapter;

    publish_data->operation_data.publish_data.completion_callback_fn = publish_options->completion_callback_fn;
    publish_data->operation_data.publish_data.user_data = publish_options->user_data;

    return publish_data;
}

static void s_aws_mqtt_protocol_adapter_operation_user_data_destroy(
    struct aws_mqtt_protocol_adapter_operation_userdata *userdata) {
    if (userdata == NULL) {
        return;
    }

    if (aws_linked_list_node_next_is_valid(&userdata->node) && aws_linked_list_node_prev_is_valid(&userdata->node)) {
        aws_linked_list_remove(&userdata->node);
    }

    if (userdata->operation_type == AMPAOT_SUBSCRIBE_UNSUBSCRIBE) {
        aws_byte_buf_clean_up(&userdata->operation_data.sub_unsub_data.topic_filter);
    }

    aws_mem_release(userdata->allocator, userdata);
}

/*****************************************************************************************************************/

struct aws_mqtt_protocol_adapter_311_impl {
    struct aws_allocator *allocator;
    struct aws_mqtt_protocol_adapter base;

    struct aws_linked_list incomplete_operations;
    struct aws_mqtt_protocol_adapter_options config;

    struct aws_event_loop *loop;
    struct aws_mqtt_client_connection *connection;
    struct aws_mqtt311_listener *listener;
};

static void s_aws_mqtt_protocol_adapter_311_destroy(void *impl) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;

    // all the real cleanup is done in the listener termination callback
    aws_mqtt311_listener_release(adapter->listener);
}

/* Subscribe */

static void s_protocol_adapter_311_subscribe_completion(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    int error_code,
    void *userdata) {
    (void)connection;
    (void)topic;
    (void)packet_id;

    struct aws_mqtt_protocol_adapter_operation_userdata *subscribe_data = userdata;
    struct aws_mqtt_protocol_adapter_311_impl *adapter = subscribe_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    if (error_code == AWS_ERROR_SUCCESS) {
        if (qos >= 128) {
            error_code = AWS_ERROR_MQTT_PROTOCOL_ADAPTER_FAILING_REASON_CODE;
        }
    }

    struct aws_protocol_adapter_subscription_event subscribe_event = {
        .topic_filter = aws_byte_cursor_from_buf(&subscribe_data->operation_data.sub_unsub_data.topic_filter),
        .event_type = AWS_PASET_SUBSCRIBE,
        .error_code = error_code,
        .retryable = true,
    };

    (*adapter->config.subscription_event_callback)(&subscribe_event, adapter->config.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(subscribe_data);
}

int s_aws_mqtt_protocol_adapter_311_subscribe(void *impl, struct aws_protocol_adapter_subscribe_options *options) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;
    struct aws_mqtt_client_connection_311_impl *connection_impl = adapter->connection->impl;

    struct aws_mqtt_protocol_adapter_operation_userdata *subscribe_data =
        s_aws_mqtt_protocol_adapter_sub_unsub_data_new(adapter->allocator, options->topic_filter, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &subscribe_data->node);

    uint64_t timeout_nanos =
        aws_timestamp_convert(options->ack_timeout_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    if (aws_mqtt_client_connection_311_subscribe(
            connection_impl,
            &options->topic_filter,
            AWS_MQTT_QOS_AT_LEAST_ONCE,
            NULL,
            NULL,
            NULL,
            s_protocol_adapter_311_subscribe_completion,
            subscribe_data,
            timeout_nanos) == 0) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(subscribe_data);

    return AWS_OP_ERR;
}

/* Unsubscribe */

static bool s_is_retryable_unsubscribe311(int error_code) {
    return error_code == AWS_ERROR_MQTT_TIMEOUT;
}

static void s_protocol_adapter_311_unsubscribe_completion(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {
    (void)connection;
    (void)packet_id;

    struct aws_mqtt_protocol_adapter_operation_userdata *unsubscribe_data = userdata;
    struct aws_mqtt_protocol_adapter_311_impl *adapter = unsubscribe_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    struct aws_protocol_adapter_subscription_event unsubscribe_event = {
        .topic_filter = aws_byte_cursor_from_buf(&unsubscribe_data->operation_data.sub_unsub_data.topic_filter),
        .event_type = AWS_PASET_UNSUBSCRIBE,
        .error_code = error_code,
        .retryable = s_is_retryable_unsubscribe311(error_code),
    };

    (*adapter->config.subscription_event_callback)(&unsubscribe_event, adapter->config.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(unsubscribe_data);
}

int s_aws_mqtt_protocol_adapter_311_unsubscribe(void *impl, struct aws_protocol_adapter_unsubscribe_options *options) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;
    struct aws_mqtt_client_connection_311_impl *connection_impl = adapter->connection->impl;

    struct aws_mqtt_protocol_adapter_operation_userdata *unsubscribe_data =
        s_aws_mqtt_protocol_adapter_sub_unsub_data_new(adapter->allocator, options->topic_filter, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &unsubscribe_data->node);

    uint64_t timeout_nanos =
        aws_timestamp_convert(options->ack_timeout_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    if (aws_mqtt_client_connection_311_unsubscribe(
            connection_impl,
            &options->topic_filter,
            s_protocol_adapter_311_unsubscribe_completion,
            unsubscribe_data,
            timeout_nanos) == 0) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(unsubscribe_data);

    return AWS_OP_ERR;
}

/* Publish */

static void s_protocol_adapter_311_publish_completion(
    struct aws_mqtt_client_connection *connection,
    uint16_t packet_id,
    int error_code,
    void *userdata) {

    (void)connection;
    (void)packet_id;

    struct aws_mqtt_protocol_adapter_operation_userdata *publish_data = userdata;
    struct aws_mqtt_protocol_adapter_311_impl *adapter = publish_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    (*publish_data->operation_data.publish_data.completion_callback_fn)(
        error_code, publish_data->operation_data.publish_data.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(publish_data);
}

int s_aws_mqtt_protocol_adapter_311_publish(void *impl, struct aws_protocol_adapter_publish_options *options) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;
    struct aws_mqtt_client_connection_311_impl *connection_impl = adapter->connection->impl;

    struct aws_mqtt_protocol_adapter_operation_userdata *publish_data =
        s_aws_mqtt_protocol_adapter_publish_data_new(adapter->allocator, options, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &publish_data->node);

    uint64_t timeout_nanos =
        aws_timestamp_convert(options->ack_timeout_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    if (aws_mqtt_client_connection_311_publish(
            connection_impl,
            &options->topic,
            AWS_MQTT_QOS_AT_LEAST_ONCE,
            false,
            &options->payload,
            s_protocol_adapter_311_publish_completion,
            publish_data,
            timeout_nanos) == 0) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(publish_data);

    return AWS_OP_ERR;
}

static void s_protocol_adapter_mqtt311_listener_publish_received(
    struct aws_mqtt_client_connection *connection,
    const struct aws_byte_cursor *topic,
    const struct aws_byte_cursor *payload,
    bool dup,
    enum aws_mqtt_qos qos,
    bool retain,
    void *userdata) {

    (void)connection;
    (void)dup;
    (void)qos;
    (void)retain;

    struct aws_mqtt_protocol_adapter_311_impl *adapter = userdata;

    struct aws_protocol_adapter_incoming_publish_event publish_event = {
        .topic = *topic,
        .payload = *payload,
    };

    (*adapter->config.incoming_publish_callback)(&publish_event, adapter->config.user_data);
}

static void s_protocol_adapter_mqtt311_listener_connection_success(
    struct aws_mqtt_client_connection *connection,
    enum aws_mqtt_connect_return_code return_code,
    bool session_present,
    void *userdata) {
    (void)connection;
    (void)return_code;

    struct aws_mqtt_protocol_adapter_311_impl *adapter = userdata;

    if (adapter->config.connection_event_callback != NULL) {
        struct aws_protocol_adapter_connection_event connection_event = {
            .event_type = AWS_PACET_CONNECTED,
            .joined_session = session_present,
        };

        (*adapter->config.connection_event_callback)(&connection_event, adapter->config.user_data);
    }
}

static void s_protocol_adapter_mqtt311_emit_disconnect_event(struct aws_mqtt_protocol_adapter_311_impl *adapter) {
    if (adapter->config.connection_event_callback != NULL) {
        struct aws_protocol_adapter_connection_event connection_event = {
            .event_type = AWS_PACET_DISCONNECTED,
        };

        (*adapter->config.connection_event_callback)(&connection_event, adapter->config.user_data);
    }
}

static void s_protocol_adapter_mqtt311_listener_connection_interrupted(
    struct aws_mqtt_client_connection *connection,
    int error_code,
    void *userdata) {
    (void)connection;
    (void)error_code;

    s_protocol_adapter_mqtt311_emit_disconnect_event(userdata);
}

static void s_aws_mqtt_protocol_adapter_311_disconnect_fn(
    struct aws_mqtt_client_connection *connection,
    void *userdata) {
    (void)connection;

    s_protocol_adapter_mqtt311_emit_disconnect_event(userdata);
}

static bool s_aws_mqtt_protocol_adapter_311_is_connected(void *impl) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;
    struct aws_mqtt_client_connection_311_impl *connection_impl = adapter->connection->impl;

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(connection_impl->loop));

    mqtt_connection_lock_synced_data(connection_impl);
    enum aws_mqtt_client_connection_state current_state = connection_impl->synced_data.state;
    mqtt_connection_unlock_synced_data(connection_impl);

    return current_state == AWS_MQTT_CLIENT_STATE_CONNECTED;
}

static void s_release_incomplete_operations(struct aws_linked_list *incomplete_operations) {
    struct aws_linked_list dummy_list;
    aws_linked_list_init(&dummy_list);
    aws_linked_list_swap_contents(incomplete_operations, &dummy_list);

    while (!aws_linked_list_empty(&dummy_list)) {
        struct aws_linked_list_node *head = aws_linked_list_pop_front(&dummy_list);
        struct aws_mqtt_protocol_adapter_operation_userdata *userdata =
            AWS_CONTAINER_OF(head, struct aws_mqtt_protocol_adapter_operation_userdata, node);

        userdata->adapter = NULL;

        if (userdata->operation_type == AMPAOT_PUBLISH) {
            struct aws_mqtt_protocol_adapter_publish_data *publish_data = &userdata->operation_data.publish_data;
            if (publish_data->completion_callback_fn != NULL) {
                (*userdata->operation_data.publish_data.completion_callback_fn)(
                    AWS_ERROR_MQTT_REQUEST_RESPONSE_CLIENT_SHUT_DOWN, publish_data->user_data);
            }
        }
    }
}

static void s_protocol_adapter_mqtt311_listener_termination_callback(void *user_data) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = user_data;
    struct aws_mqtt_client_connection_311_impl *impl = adapter->connection->impl;

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(impl->loop));

    s_release_incomplete_operations(&adapter->incomplete_operations);

    aws_mqtt_client_connection_release(adapter->connection);

    aws_protocol_adapter_terminate_callback_fn *terminate_callback = adapter->config.terminate_callback;
    void *terminate_user_data = adapter->config.user_data;

    aws_mem_release(adapter->allocator, adapter);

    if (terminate_callback) {
        (*terminate_callback)(terminate_user_data);
    }
}

static struct aws_event_loop *s_aws_mqtt_protocol_adapter_311_get_event_loop(void *impl) {
    struct aws_mqtt_protocol_adapter_311_impl *adapter = impl;

    return adapter->loop;
}

static struct aws_mqtt_protocol_adapter_vtable s_protocol_adapter_mqtt311_vtable = {
    .aws_mqtt_protocol_adapter_destroy_fn = s_aws_mqtt_protocol_adapter_311_destroy,
    .aws_mqtt_protocol_adapter_subscribe_fn = s_aws_mqtt_protocol_adapter_311_subscribe,
    .aws_mqtt_protocol_adapter_unsubscribe_fn = s_aws_mqtt_protocol_adapter_311_unsubscribe,
    .aws_mqtt_protocol_adapter_publish_fn = s_aws_mqtt_protocol_adapter_311_publish,
    .aws_mqtt_protocol_adapter_is_connected_fn = s_aws_mqtt_protocol_adapter_311_is_connected,
    .aws_mqtt_protocol_adapter_get_event_loop_fn = s_aws_mqtt_protocol_adapter_311_get_event_loop,
};

struct aws_mqtt_protocol_adapter *aws_mqtt_protocol_adapter_new_from_311(
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter_options *options,
    struct aws_mqtt_client_connection *connection) {

    if (options == NULL || connection == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (aws_mqtt_client_connection_get_impl_type(connection) != AWS_MQTT311_IT_311_CONNECTION) {
        struct aws_mqtt_client_connection_5_impl *adapter_impl = connection->impl;
        return aws_mqtt_protocol_adapter_new_from_5(allocator, options, adapter_impl->client);
    }

    struct aws_mqtt_client_connection_311_impl *impl = connection->impl;

    struct aws_mqtt_protocol_adapter_311_impl *adapter =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_protocol_adapter_311_impl));

    adapter->allocator = allocator;
    adapter->base.impl = adapter;
    adapter->base.vtable = &s_protocol_adapter_mqtt311_vtable;
    aws_linked_list_init(&adapter->incomplete_operations);
    adapter->config = *options;
    adapter->loop = impl->loop;
    adapter->connection = aws_mqtt_client_connection_acquire(connection);

    struct aws_mqtt311_listener_config listener_options = {
        .connection = connection,
        .listener_callbacks =
            {
                .publish_received_handler = s_protocol_adapter_mqtt311_listener_publish_received,
                .connection_success_handler = s_protocol_adapter_mqtt311_listener_connection_success,
                .connection_interrupted_handler = s_protocol_adapter_mqtt311_listener_connection_interrupted,
                .disconnect_handler = s_aws_mqtt_protocol_adapter_311_disconnect_fn,
                .user_data = adapter,
            },
        .termination_callback = s_protocol_adapter_mqtt311_listener_termination_callback,
        .termination_callback_user_data = adapter,
    };

    adapter->listener = aws_mqtt311_listener_new(allocator, &listener_options);

    return &adapter->base;
}

/******************************************************************************************************************/

struct aws_mqtt_protocol_adapter_5_impl {
    struct aws_allocator *allocator;
    struct aws_mqtt_protocol_adapter base;
    struct aws_linked_list incomplete_operations;
    struct aws_mqtt_protocol_adapter_options config;

    struct aws_event_loop *loop;
    struct aws_mqtt5_client *client;
    struct aws_mqtt5_listener *listener;
};

static void s_aws_mqtt_protocol_adapter_5_destroy(void *impl) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;

    // all the real cleanup is done in the listener termination callback
    aws_mqtt5_listener_release(adapter->listener);
}

/* Subscribe */

static bool s_is_retryable_subscribe(enum aws_mqtt5_suback_reason_code reason_code, int error_code) {
    if (error_code == AWS_ERROR_MQTT5_PACKET_VALIDATION || error_code == AWS_ERROR_MQTT5_SUBSCRIBE_OPTIONS_VALIDATION) {
        return false;
    } else if (error_code != AWS_ERROR_SUCCESS) {
        return true;
    }

    switch (reason_code) {
        case AWS_MQTT5_SARC_GRANTED_QOS_0:
        case AWS_MQTT5_SARC_GRANTED_QOS_1:
        case AWS_MQTT5_SARC_GRANTED_QOS_2:
        case AWS_MQTT5_SARC_UNSPECIFIED_ERROR:
        case AWS_MQTT5_SARC_PACKET_IDENTIFIER_IN_USE:
        case AWS_MQTT5_SARC_IMPLEMENTATION_SPECIFIC_ERROR:
        case AWS_MQTT5_SARC_QUOTA_EXCEEDED:
            return true;

        default:
            return false;
    }
}

static void s_protocol_adapter_5_subscribe_completion(
    const struct aws_mqtt5_packet_suback_view *suback,
    int error_code,
    void *complete_ctx) {
    struct aws_mqtt_protocol_adapter_operation_userdata *subscribe_data = complete_ctx;
    struct aws_mqtt_protocol_adapter_5_impl *adapter = subscribe_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    enum aws_mqtt5_suback_reason_code reason_code = AWS_MQTT5_SARC_GRANTED_QOS_0;
    if (suback != NULL && suback->reason_code_count > 0) {
        reason_code = suback->reason_codes[0];
    }
    bool is_retryable = s_is_retryable_subscribe(reason_code, error_code);

    if (error_code == AWS_ERROR_SUCCESS) {
        if (suback == NULL || suback->reason_code_count != 1 || suback->reason_codes[0] >= 128) {
            error_code = AWS_ERROR_MQTT_PROTOCOL_ADAPTER_FAILING_REASON_CODE;
        }
    }

    struct aws_protocol_adapter_subscription_event subscribe_event = {
        .topic_filter = aws_byte_cursor_from_buf(&subscribe_data->operation_data.sub_unsub_data.topic_filter),
        .event_type = AWS_PASET_SUBSCRIBE,
        .error_code = error_code,
        .retryable = is_retryable,
    };

    (*adapter->config.subscription_event_callback)(&subscribe_event, adapter->config.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(subscribe_data);
}

int s_aws_mqtt_protocol_adapter_5_subscribe(void *impl, struct aws_protocol_adapter_subscribe_options *options) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;

    struct aws_mqtt_protocol_adapter_operation_userdata *subscribe_data =
        s_aws_mqtt_protocol_adapter_sub_unsub_data_new(adapter->allocator, options->topic_filter, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &subscribe_data->node);

    struct aws_mqtt5_subscription_view subscription_view = {
        .qos = AWS_MQTT5_QOS_AT_LEAST_ONCE,
        .topic_filter = options->topic_filter,
    };

    struct aws_mqtt5_packet_subscribe_view subscribe_view = {
        .subscriptions = &subscription_view,
        .subscription_count = 1,
    };

    struct aws_mqtt5_subscribe_completion_options completion_options = {
        .ack_timeout_seconds_override = options->ack_timeout_seconds,
        .completion_callback = s_protocol_adapter_5_subscribe_completion,
        .completion_user_data = subscribe_data,
    };

    if (aws_mqtt5_client_subscribe(adapter->client, &subscribe_view, &completion_options)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(subscribe_data);

    return AWS_OP_ERR;
}

/* Unsubscribe */

static bool s_is_retryable_unsubscribe5(enum aws_mqtt5_unsuback_reason_code reason_code, int error_code) {
    if (error_code == AWS_ERROR_MQTT5_PACKET_VALIDATION ||
        error_code == AWS_ERROR_MQTT5_UNSUBSCRIBE_OPTIONS_VALIDATION) {
        return false;
    } else if (error_code == AWS_ERROR_MQTT_TIMEOUT) {
        return true;
    }

    switch (reason_code) {
        case AWS_MQTT5_UARC_UNSPECIFIED_ERROR:
        case AWS_MQTT5_UARC_IMPLEMENTATION_SPECIFIC_ERROR:
            return true;

        default:
            return false;
    }
}

static void s_protocol_adapter_5_unsubscribe_completion(
    const struct aws_mqtt5_packet_unsuback_view *unsuback,
    int error_code,
    void *complete_ctx) {
    struct aws_mqtt_protocol_adapter_operation_userdata *unsubscribe_data = complete_ctx;
    struct aws_mqtt_protocol_adapter_5_impl *adapter = unsubscribe_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    enum aws_mqtt5_unsuback_reason_code reason_code = AWS_MQTT5_UARC_SUCCESS;
    if (unsuback != NULL && unsuback->reason_code_count > 0) {
        reason_code = unsuback->reason_codes[0];
    }

    bool is_retryable = s_is_retryable_unsubscribe5(reason_code, error_code);

    if (error_code == AWS_ERROR_SUCCESS) {
        if (unsuback == NULL || unsuback->reason_code_count != 1 || unsuback->reason_codes[0] >= 128) {
            error_code = AWS_ERROR_MQTT_PROTOCOL_ADAPTER_FAILING_REASON_CODE;
        }
    }

    struct aws_protocol_adapter_subscription_event unsubscribe_event = {
        .topic_filter = aws_byte_cursor_from_buf(&unsubscribe_data->operation_data.sub_unsub_data.topic_filter),
        .event_type = AWS_PASET_UNSUBSCRIBE,
        .error_code = error_code,
        .retryable = is_retryable,
    };

    (*adapter->config.subscription_event_callback)(&unsubscribe_event, adapter->config.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(unsubscribe_data);
}

int s_aws_mqtt_protocol_adapter_5_unsubscribe(void *impl, struct aws_protocol_adapter_unsubscribe_options *options) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;

    struct aws_mqtt_protocol_adapter_operation_userdata *unsubscribe_data =
        s_aws_mqtt_protocol_adapter_sub_unsub_data_new(adapter->allocator, options->topic_filter, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &unsubscribe_data->node);

    struct aws_mqtt5_packet_unsubscribe_view unsubscribe_view = {
        .topic_filters = &options->topic_filter,
        .topic_filter_count = 1,
    };

    struct aws_mqtt5_unsubscribe_completion_options completion_options = {
        .ack_timeout_seconds_override = options->ack_timeout_seconds,
        .completion_callback = s_protocol_adapter_5_unsubscribe_completion,
        .completion_user_data = unsubscribe_data,
    };

    if (aws_mqtt5_client_unsubscribe(adapter->client, &unsubscribe_view, &completion_options)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(unsubscribe_data);

    return AWS_OP_ERR;
}

/* Publish */

static void s_protocol_adapter_5_publish_completion(
    enum aws_mqtt5_packet_type packet_type,
    const void *packet,
    int error_code,
    void *complete_ctx) {
    struct aws_mqtt_protocol_adapter_operation_userdata *publish_data = complete_ctx;
    struct aws_mqtt_protocol_adapter_5_impl *adapter = publish_data->adapter;

    if (adapter == NULL) {
        goto done;
    }

    if (error_code == AWS_ERROR_SUCCESS && packet_type == AWS_MQTT5_PT_PUBACK) {
        const struct aws_mqtt5_packet_puback_view *puback = packet;
        if (puback->reason_code >= 128) {
            error_code = AWS_ERROR_MQTT_PROTOCOL_ADAPTER_FAILING_REASON_CODE;
        }
    }

    (*publish_data->operation_data.publish_data.completion_callback_fn)(
        error_code, publish_data->operation_data.publish_data.user_data);

done:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(publish_data);
}

int s_aws_mqtt_protocol_adapter_5_publish(void *impl, struct aws_protocol_adapter_publish_options *options) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;
    struct aws_mqtt_protocol_adapter_operation_userdata *publish_data =
        s_aws_mqtt_protocol_adapter_publish_data_new(adapter->allocator, options, adapter);

    aws_linked_list_push_back(&adapter->incomplete_operations, &publish_data->node);

    struct aws_mqtt5_packet_publish_view publish_view = {
        .topic = options->topic, .qos = AWS_MQTT5_QOS_AT_LEAST_ONCE, .payload = options->payload};

    struct aws_mqtt5_publish_completion_options completion_options = {
        .ack_timeout_seconds_override = options->ack_timeout_seconds,
        .completion_callback = s_protocol_adapter_5_publish_completion,
        .completion_user_data = publish_data,
    };

    if (aws_mqtt5_client_publish(adapter->client, &publish_view, &completion_options)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    s_aws_mqtt_protocol_adapter_operation_user_data_destroy(publish_data);

    return AWS_OP_ERR;
}

static bool s_aws_mqtt_protocol_adapter_5_is_connected(void *impl) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(adapter->client->loop));

    enum aws_mqtt5_client_state current_state = adapter->client->current_state;

    return current_state == AWS_MCS_CONNECTED;
}

static bool s_protocol_adapter_mqtt5_listener_publish_received(
    const struct aws_mqtt5_packet_publish_view *publish,
    void *user_data) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = user_data;

    struct aws_protocol_adapter_incoming_publish_event publish_event = {
        .topic = publish->topic,
        .payload = publish->payload,
    };

    (*adapter->config.incoming_publish_callback)(&publish_event, adapter->config.user_data);

    return false;
}

static void s_protocol_adapter_mqtt5_lifecycle_event_callback(const struct aws_mqtt5_client_lifecycle_event *event) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = event->user_data;

    switch (event->event_type) {
        case AWS_MQTT5_CLET_CONNECTION_SUCCESS: {
            struct aws_protocol_adapter_connection_event connection_event = {
                .event_type = AWS_PACET_CONNECTED,
                .joined_session = event->settings->rejoined_session,
            };

            (*adapter->config.connection_event_callback)(&connection_event, adapter->config.user_data);
            break;
        }
        case AWS_MQTT5_CLET_DISCONNECTION: {
            struct aws_protocol_adapter_connection_event connection_event = {
                .event_type = AWS_PACET_DISCONNECTED,
            };

            (*adapter->config.connection_event_callback)(&connection_event, adapter->config.user_data);
            break;
        }
        default:
            break;
    }
}

static void s_protocol_adapter_mqtt5_listener_termination_callback(void *user_data) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = user_data;

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(adapter->client->loop));

    s_release_incomplete_operations(&adapter->incomplete_operations);

    aws_mqtt5_client_release(adapter->client);

    aws_protocol_adapter_terminate_callback_fn *terminate_callback = adapter->config.terminate_callback;
    void *terminate_user_data = adapter->config.user_data;

    aws_mem_release(adapter->allocator, adapter);

    if (terminate_callback) {
        (*terminate_callback)(terminate_user_data);
    }
}

static struct aws_event_loop *s_aws_mqtt_protocol_adapter_5_get_event_loop(void *impl) {
    struct aws_mqtt_protocol_adapter_5_impl *adapter = impl;

    return adapter->loop;
}

static struct aws_mqtt_protocol_adapter_vtable s_protocol_adapter_mqtt5_vtable = {
    .aws_mqtt_protocol_adapter_destroy_fn = s_aws_mqtt_protocol_adapter_5_destroy,
    .aws_mqtt_protocol_adapter_subscribe_fn = s_aws_mqtt_protocol_adapter_5_subscribe,
    .aws_mqtt_protocol_adapter_unsubscribe_fn = s_aws_mqtt_protocol_adapter_5_unsubscribe,
    .aws_mqtt_protocol_adapter_publish_fn = s_aws_mqtt_protocol_adapter_5_publish,
    .aws_mqtt_protocol_adapter_is_connected_fn = s_aws_mqtt_protocol_adapter_5_is_connected,
    .aws_mqtt_protocol_adapter_get_event_loop_fn = s_aws_mqtt_protocol_adapter_5_get_event_loop};

struct aws_mqtt_protocol_adapter *aws_mqtt_protocol_adapter_new_from_5(
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter_options *options,
    struct aws_mqtt5_client *client) {

    if (options == NULL || client == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_mqtt_protocol_adapter_5_impl *adapter =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_protocol_adapter_5_impl));

    adapter->allocator = allocator;
    adapter->base.impl = adapter;
    adapter->base.vtable = &s_protocol_adapter_mqtt5_vtable;
    aws_linked_list_init(&adapter->incomplete_operations);
    adapter->config = *options;
    adapter->loop = client->loop;
    adapter->client = aws_mqtt5_client_acquire(client);

    struct aws_mqtt5_listener_config listener_options = {
        .client = client,
        .listener_callbacks =
            {
                .listener_publish_received_handler = s_protocol_adapter_mqtt5_listener_publish_received,
                .listener_publish_received_handler_user_data = adapter,
                .lifecycle_event_handler = s_protocol_adapter_mqtt5_lifecycle_event_callback,
                .lifecycle_event_handler_user_data = adapter,
            },
        .termination_callback = s_protocol_adapter_mqtt5_listener_termination_callback,
        .termination_callback_user_data = adapter,
    };

    adapter->listener = aws_mqtt5_listener_new(allocator, &listener_options);

    return &adapter->base;
}

void aws_mqtt_protocol_adapter_destroy(struct aws_mqtt_protocol_adapter *adapter) {
    (*adapter->vtable->aws_mqtt_protocol_adapter_destroy_fn)(adapter->impl);
}

int aws_mqtt_protocol_adapter_subscribe(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_subscribe_options *options) {
    return (*adapter->vtable->aws_mqtt_protocol_adapter_subscribe_fn)(adapter->impl, options);
}

int aws_mqtt_protocol_adapter_unsubscribe(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_unsubscribe_options *options) {
    return (*adapter->vtable->aws_mqtt_protocol_adapter_unsubscribe_fn)(adapter->impl, options);
}

int aws_mqtt_protocol_adapter_publish(
    struct aws_mqtt_protocol_adapter *adapter,
    struct aws_protocol_adapter_publish_options *options) {
    return (*adapter->vtable->aws_mqtt_protocol_adapter_publish_fn)(adapter->impl, options);
}

bool aws_mqtt_protocol_adapter_is_connected(struct aws_mqtt_protocol_adapter *adapter) {
    return (*adapter->vtable->aws_mqtt_protocol_adapter_is_connected_fn)(adapter->impl);
}

struct aws_event_loop *aws_mqtt_protocol_adapter_get_event_loop(struct aws_mqtt_protocol_adapter *adapter) {
    return (*adapter->vtable->aws_mqtt_protocol_adapter_get_event_loop_fn)(adapter->impl);
}

const char *aws_protocol_adapter_subscription_event_type_to_c_str(
    enum aws_protocol_adapter_subscription_event_type type) {
    switch (type) {
        case AWS_PASET_SUBSCRIBE:
            return "Subscribe";

        case AWS_PASET_UNSUBSCRIBE:
            return "Unsubscribe";
    }

    return "Unknown";
}

const char *aws_protocol_adapter_connection_event_type_to_c_str(enum aws_protocol_adapter_connection_event_type type) {
    switch (type) {
        case AWS_PACET_CONNECTED:
            return "Connected";

        case AWS_PACET_DISCONNECTED:
            return "Disconnected";
    }

    return "Unknown";
}
