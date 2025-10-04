/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/common/clock.h>
#include <aws/common/rw_lock.h>

#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/mqtt_subscription_set.h>
#include <aws/mqtt/private/v5/mqtt5_client_impl.h>
#include <aws/mqtt/private/v5/mqtt5_to_mqtt3_adapter_impl.h>
#include <aws/mqtt/v5/mqtt5_listener.h>

/*
 * A best-effort-but-not-100%-accurate translation from mqtt5 error codes to mqtt311 error codes.
 */
static int s_translate_mqtt5_error_code_to_mqtt311(int error_code) {
    switch (error_code) {
        case AWS_ERROR_MQTT5_ENCODE_FAILURE:
        case AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR:
            return AWS_ERROR_MQTT_PROTOCOL_ERROR;

        case AWS_ERROR_MQTT5_CONNACK_CONNECTION_REFUSED:
            return AWS_ERROR_MQTT_PROTOCOL_ERROR; /* a decidedly strange choice by the 311 implementation */

        case AWS_ERROR_MQTT5_CONNACK_TIMEOUT:
        case AWS_ERROR_MQTT5_PING_RESPONSE_TIMEOUT:
            return AWS_ERROR_MQTT_TIMEOUT;

        case AWS_ERROR_MQTT5_USER_REQUESTED_STOP:
        case AWS_ERROR_MQTT5_CLIENT_TERMINATED:
            return AWS_IO_SOCKET_CLOSED;

        case AWS_ERROR_MQTT5_DISCONNECT_RECEIVED:
            return AWS_ERROR_MQTT_UNEXPECTED_HANGUP;

        case AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY:
            return AWS_ERROR_MQTT_CANCELLED_FOR_CLEAN_SESSION;

        case AWS_ERROR_MQTT5_ENCODE_SIZE_UNSUPPORTED_PACKET_TYPE:
            return AWS_ERROR_MQTT_INVALID_PACKET_TYPE;

        case AWS_ERROR_MQTT5_OPERATION_PROCESSING_FAILURE:
            return AWS_ERROR_MQTT_PROTOCOL_ERROR;

        case AWS_ERROR_MQTT5_INVALID_UTF8_STRING:
            return AWS_ERROR_MQTT_INVALID_TOPIC;

        default:
            return error_code;
    }
}

struct aws_mqtt_adapter_final_destroy_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection *connection;
};

static void s_mqtt_adapter_final_destroy_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;

    struct aws_mqtt_adapter_final_destroy_task *destroy_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = destroy_task->connection->impl;

    AWS_LOGF_DEBUG(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: Final destruction of mqtt3-to-5 adapter", (void *)adapter);

    aws_mqtt_client_on_connection_termination_fn *termination_handler = NULL;
    void *termination_handler_user_data = NULL;
    if (adapter->on_termination != NULL) {
        termination_handler = adapter->on_termination;
        termination_handler_user_data = adapter->on_termination_user_data;
    }

    if (adapter->client->config->websocket_handshake_transform_user_data == adapter) {
        /*
         * If the mqtt5 client is pointing to us for websocket transform, then erase that.  The callback
         * is invoked from our pinned event loop so this is safe.
         *
         * TODO: It is possible that multiple adapters may have sequentially side-affected the websocket handshake.
         * For now, in that case, subsequent connection attempts will probably not succeed.
         */
        adapter->client->config->websocket_handshake_transform = NULL;
        adapter->client->config->websocket_handshake_transform_user_data = NULL;
    }

    aws_mqtt_subscription_set_destroy(adapter->subscriptions);
    aws_mqtt5_to_mqtt3_adapter_operation_table_clean_up(&adapter->operational_state);

    adapter->client = aws_mqtt5_client_release(adapter->client);

    aws_mem_release(adapter->allocator, adapter);

    aws_mem_release(destroy_task->allocator, destroy_task);

    /* trigger the termination callback */
    if (termination_handler) {
        termination_handler(termination_handler_user_data);
    }
}

static struct aws_mqtt_adapter_final_destroy_task *s_aws_mqtt_adapter_final_destroy_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter) {

    struct aws_mqtt_adapter_final_destroy_task *destroy_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_adapter_final_destroy_task));

    aws_task_init(
        &destroy_task->task, s_mqtt_adapter_final_destroy_task_fn, (void *)destroy_task, "MqttAdapterFinalDestroy");
    destroy_task->allocator = adapter->allocator;
    destroy_task->connection = &adapter->base; /* Do not acquire, we're at zero external and internal ref counts */

    return destroy_task;
}

static void s_aws_mqtt_adapter_final_destroy(struct aws_mqtt_client_connection_5_impl *adapter) {

    struct aws_mqtt_adapter_final_destroy_task *task =
        s_aws_mqtt_adapter_final_destroy_task_new(adapter->allocator, adapter);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create adapter final destroy task, last_error: %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);
}

struct aws_mqtt_adapter_disconnect_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_on_disconnect_fn *on_disconnect;
    void *on_disconnect_user_data;
};

static void s_adapter_disconnect_task_fn(struct aws_task *task, void *arg, enum aws_task_status status);

static struct aws_mqtt_adapter_disconnect_task *s_aws_mqtt_adapter_disconnect_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_on_disconnect_fn *on_disconnect,
    void *on_disconnect_user_data) {

    struct aws_mqtt_adapter_disconnect_task *disconnect_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_adapter_disconnect_task));

    aws_task_init(
        &disconnect_task->task, s_adapter_disconnect_task_fn, (void *)disconnect_task, "AdapterDisconnectTask");
    disconnect_task->allocator = adapter->allocator;
    disconnect_task->adapter =
        (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);

    disconnect_task->on_disconnect = on_disconnect;
    disconnect_task->on_disconnect_user_data = on_disconnect_user_data;

    return disconnect_task;
}

static int s_aws_mqtt_client_connection_5_disconnect(
    void *impl,
    aws_mqtt_client_on_disconnect_fn *on_disconnect,
    void *on_disconnect_user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_adapter_disconnect_task *task =
        s_aws_mqtt_adapter_disconnect_task_new(adapter->allocator, adapter, on_disconnect, on_disconnect_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create adapter disconnect task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_adapter_connect_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    struct aws_byte_buf host_name;
    uint32_t port;
    struct aws_socket_options socket_options;
    struct aws_tls_connection_options *tls_options_ptr;
    struct aws_tls_connection_options tls_options;

    struct aws_byte_buf client_id;
    uint16_t keep_alive_time_secs;
    uint32_t ping_timeout_ms;
    uint32_t protocol_operation_timeout_ms;
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete;
    void *on_connection_complete_user_data;
    bool clean_session;
};

static void s_aws_mqtt_adapter_connect_task_destroy(struct aws_mqtt_adapter_connect_task *task) {
    if (task == NULL) {
        return;
    }

    aws_byte_buf_clean_up(&task->host_name);
    aws_byte_buf_clean_up(&task->client_id);

    if (task->tls_options_ptr) {
        aws_tls_connection_options_clean_up(task->tls_options_ptr);
    }

    aws_mem_release(task->allocator, task);
}

static void s_adapter_connect_task_fn(struct aws_task *task, void *arg, enum aws_task_status status);

static struct aws_mqtt_adapter_connect_task *s_aws_mqtt_adapter_connect_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    const struct aws_mqtt_connection_options *connection_options) {

    struct aws_mqtt_adapter_connect_task *connect_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_adapter_connect_task));

    aws_task_init(&connect_task->task, s_adapter_connect_task_fn, (void *)connect_task, "AdapterConnectTask");
    connect_task->allocator = adapter->allocator;

    aws_byte_buf_init_copy_from_cursor(&connect_task->host_name, allocator, connection_options->host_name);
    connect_task->port = connection_options->port;
    connect_task->socket_options = *connection_options->socket_options;
    if (connection_options->tls_options) {
        if (aws_tls_connection_options_copy(&connect_task->tls_options, connection_options->tls_options)) {
            goto error;
        }
        connect_task->tls_options_ptr = &connect_task->tls_options;

        /* Cheat and set the tls_options host_name to our copy if they're the same */
        if (!connect_task->tls_options.server_name) {
            struct aws_byte_cursor host_name_cur = aws_byte_cursor_from_buf(&connect_task->host_name);

            if (aws_tls_connection_options_set_server_name(
                    &connect_task->tls_options, connect_task->allocator, &host_name_cur)) {
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                    "id=%p: mqtt3-to-5-adapter - Failed to set TLS Connection Options server name",
                    (void *)adapter);
                goto error;
            }
        }
    }
    connect_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    aws_byte_buf_init_copy_from_cursor(&connect_task->client_id, allocator, connection_options->client_id);

    connect_task->keep_alive_time_secs = connection_options->keep_alive_time_secs;
    connect_task->ping_timeout_ms = connection_options->ping_timeout_ms;
    connect_task->protocol_operation_timeout_ms = connection_options->protocol_operation_timeout_ms;
    connect_task->on_connection_complete = connection_options->on_connection_complete;
    connect_task->on_connection_complete_user_data = connection_options->user_data;
    connect_task->clean_session = connection_options->clean_session;

    return connect_task;

error:
    s_aws_mqtt_adapter_connect_task_destroy(connect_task);

    return NULL;
}

static int s_validate_adapter_connection_options(
    const struct aws_mqtt_connection_options *connection_options,
    struct aws_mqtt_client_connection_5_impl *adapter) {
    if (connection_options == NULL) {
        return aws_raise_error(AWS_ERROR_MQTT5_CLIENT_OPTIONS_VALIDATION);
    }

    if (connection_options->host_name.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter - host name not set in MQTT client configuration",
            (void *)adapter);
        return aws_raise_error(AWS_ERROR_MQTT5_CLIENT_OPTIONS_VALIDATION);
    }

    /* forbid no-timeout until someone convinces me otherwise */
    if (connection_options->socket_options != NULL) {
        if (connection_options->socket_options->type == AWS_SOCKET_DGRAM ||
            connection_options->socket_options->connect_timeout_ms == 0) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter - invalid socket options in MQTT client configuration",
                (void *)adapter);
            return aws_raise_error(AWS_ERROR_MQTT5_CLIENT_OPTIONS_VALIDATION);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt_client_connection_5_connect(
    void *impl,
    const struct aws_mqtt_connection_options *connection_options) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    /* The client will not behave properly if ping timeout is not significantly shorter than the keep alive interval */
    if (s_validate_adapter_connection_options(connection_options, adapter)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_adapter_connect_task *task =
        s_aws_mqtt_adapter_connect_task_new(adapter->allocator, adapter, connection_options);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter - failed to create adapter connect task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

static void s_aws_mqtt5_to_mqtt3_adapter_lifecycle_handler(const struct aws_mqtt5_client_lifecycle_event *event) {
    struct aws_mqtt_client_connection_5_impl *adapter = event->user_data;

    switch (event->event_type) {

        case AWS_MQTT5_CLET_CONNECTION_SUCCESS:
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter - received on connection success event from mqtt5 client, adapter in state "
                "(%d)",
                (void *)adapter,
                (int)adapter->adapter_state);
            if (adapter->adapter_state != AWS_MQTT_AS_STAY_DISCONNECTED) {
                if (adapter->on_connection_success != NULL) {
                    (*adapter->on_connection_success)(
                        &adapter->base, 0, event->settings->rejoined_session, adapter->on_connection_success_user_data);
                }

                if (adapter->adapter_state == AWS_MQTT_AS_FIRST_CONNECT) {
                    /*
                     * If the 311 view is that this is an initial connection attempt, then invoke the completion
                     * callback and move to the stay-connected state.
                     */
                    if (adapter->on_connection_complete != NULL) {
                        (*adapter->on_connection_complete)(
                            &adapter->base,
                            event->error_code,
                            0,
                            event->settings->rejoined_session,
                            adapter->on_connection_complete_user_data);

                        adapter->on_connection_complete = NULL;
                        adapter->on_connection_complete_user_data = NULL;
                    }
                    adapter->adapter_state = AWS_MQTT_AS_STAY_CONNECTED;
                } else if (adapter->adapter_state == AWS_MQTT_AS_STAY_CONNECTED) {
                    /*
                     * If the 311 view is that we're in the stay-connected state (ie we've successfully done or
                     * simulated an initial connection), then invoke the connection resumption callback.
                     */
                    if (adapter->on_resumed != NULL) {
                        (*adapter->on_resumed)(
                            &adapter->base, 0, event->settings->rejoined_session, adapter->on_resumed_user_data);
                    }
                }
            }
            break;

        case AWS_MQTT5_CLET_CONNECTION_FAILURE:
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter - received on connection failure event from mqtt5 client, adapter in state "
                "(%d)",
                (void *)adapter,
                (int)adapter->adapter_state);

            /*
             * The MQTT311 interface only cares about connection failures when it's the initial connection attempt
             * after a call to connect().  Since an adapter connect() can sever an existing connection (with an
             * error code of AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT) we only react to connection failures
             * if
             *   (1) the error code is not AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT and
             *   (2) we're in the FIRST_CONNECT state
             *
             * Only if both of these are true should we invoke the connection completion callback with a failure and
             * put the adapter into the "disconnected" state, simulating the way the 311 client stops after an
             * initial connection failure.
             */
            if (event->error_code != AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT) {
                if (adapter->adapter_state != AWS_MQTT_AS_STAY_DISCONNECTED) {
                    int mqtt311_error_code = s_translate_mqtt5_error_code_to_mqtt311(event->error_code);

                    if (adapter->on_connection_failure != NULL) {
                        (*adapter->on_connection_failure)(
                            &adapter->base, mqtt311_error_code, adapter->on_connection_failure_user_data);
                    }

                    if (adapter->adapter_state == AWS_MQTT_AS_FIRST_CONNECT) {
                        if (adapter->on_connection_complete != NULL) {
                            (*adapter->on_connection_complete)(
                                &adapter->base,
                                mqtt311_error_code,
                                0,
                                false,
                                adapter->on_connection_complete_user_data);

                            adapter->on_connection_complete = NULL;
                            adapter->on_connection_complete_user_data = NULL;
                        }

                        adapter->adapter_state = AWS_MQTT_AS_STAY_DISCONNECTED;
                    }
                }
            }

            break;

        case AWS_MQTT5_CLET_DISCONNECTION:
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter - received on disconnection event from mqtt5 client, adapter in state (%d), "
                "error code (%d)",
                (void *)adapter,
                (int)adapter->adapter_state,
                event->error_code);
            /*
             * If the 311 view is that we're in the stay-connected state (ie we've successfully done or simulated
             * an initial connection), then invoke the connection interrupted callback.
             */
            if (adapter->on_interrupted != NULL && adapter->adapter_state == AWS_MQTT_AS_STAY_CONNECTED &&
                event->error_code != AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT) {

                (*adapter->on_interrupted)(
                    &adapter->base,
                    s_translate_mqtt5_error_code_to_mqtt311(event->error_code),
                    adapter->on_interrupted_user_data);
            }
            break;

        case AWS_MQTT5_CLET_STOPPED:
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter - received on stopped event from mqtt5 client, adapter in state (%d)",
                (void *)adapter,
                (int)adapter->adapter_state);

            /* If an MQTT311-view user is waiting on a disconnect callback, invoke it */
            if (adapter->on_disconnect) {
                (*adapter->on_disconnect)(&adapter->base, adapter->on_disconnect_user_data);

                adapter->on_disconnect = NULL;
                adapter->on_disconnect_user_data = NULL;
            }

            if (adapter->on_closed) {
                (*adapter->on_closed)(&adapter->base, NULL, adapter->on_closed_user_data);
            }

            /*
             * Judgement call: If the mqtt5 client is stopped behind our back, it seems better to transition to the
             * disconnected state (which only requires a connect() to restart) then stay in the STAY_CONNECTED state
             * which currently requires a disconnect() and then a connect() to restore connectivity.
             *
             * ToDo: what if we disabled mqtt5 client start/stop somehow while the adapter is attached, preventing
             * the potential to backstab each other?  Unfortunately neither start() nor stop() have an error reporting
             * mechanism.
             */
            adapter->adapter_state = AWS_MQTT_AS_STAY_DISCONNECTED;
            break;

        default:
            break;
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_disconnect_handler(
    struct aws_mqtt_client_connection_5_impl *adapter,
    struct aws_mqtt_adapter_disconnect_task *disconnect_task) {

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter - performing disconnect safe callback, adapter in state (%d)",
        (void *)adapter,
        (int)adapter->adapter_state);

    /*
     * If we're already disconnected (from the 311 perspective only), then invoke the callback and return
     */
    if (adapter->adapter_state == AWS_MQTT_AS_STAY_DISCONNECTED) {
        if (disconnect_task->on_disconnect) {
            (*disconnect_task->on_disconnect)(&adapter->base, disconnect_task->on_disconnect_user_data);
        }

        return;
    }

    /*
     * If we had a pending first connect, then notify failure
     */
    if (adapter->adapter_state == AWS_MQTT_AS_FIRST_CONNECT) {
        if (adapter->on_connection_complete != NULL) {
            (*adapter->on_connection_complete)(
                &adapter->base,
                AWS_ERROR_MQTT_CONNECTION_SHUTDOWN,
                0,
                false,
                adapter->on_connection_complete_user_data);

            adapter->on_connection_complete = NULL;
            adapter->on_connection_complete_user_data = NULL;
        }
    }

    adapter->adapter_state = AWS_MQTT_AS_STAY_DISCONNECTED;

    bool invoke_callbacks = true;
    if (adapter->client->desired_state != AWS_MCS_STOPPED) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter - disconnect forwarding stop request to mqtt5 client",
            (void *)adapter);

        aws_mqtt5_client_change_desired_state(adapter->client, AWS_MCS_STOPPED, NULL);

        adapter->on_disconnect = disconnect_task->on_disconnect;
        adapter->on_disconnect_user_data = disconnect_task->on_disconnect_user_data;
        invoke_callbacks = false;
    }

    if (invoke_callbacks) {
        if (disconnect_task->on_disconnect != NULL) {
            (*disconnect_task->on_disconnect)(&adapter->base, disconnect_task->on_disconnect_user_data);
        }

        if (adapter->on_closed) {
            (*adapter->on_closed)(&adapter->base, NULL, adapter->on_closed_user_data);
        }
    }
}

static void s_adapter_disconnect_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_adapter_disconnect_task *disconnect_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = disconnect_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    s_aws_mqtt5_to_mqtt3_adapter_disconnect_handler(adapter, disconnect_task);

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(disconnect_task->allocator, disconnect_task);
}

static void s_aws_mqtt5_to_mqtt3_adapter_update_config_on_connect(
    struct aws_mqtt_client_connection_5_impl *adapter,
    struct aws_mqtt_adapter_connect_task *connect_task) {
    struct aws_mqtt5_client_options_storage *config = adapter->client->config;

    aws_string_destroy(config->host_name);
    config->host_name = aws_string_new_from_buf(adapter->allocator, &connect_task->host_name);
    config->port = connect_task->port;
    config->socket_options = connect_task->socket_options;

    if (config->tls_options_ptr) {
        aws_tls_connection_options_clean_up(&config->tls_options);
        config->tls_options_ptr = NULL;
    }

    if (connect_task->tls_options_ptr) {
        aws_tls_connection_options_copy(&config->tls_options, connect_task->tls_options_ptr);
        config->tls_options_ptr = &config->tls_options;
    }

    aws_byte_buf_clean_up(&adapter->client->negotiated_settings.client_id_storage);
    aws_byte_buf_init_copy_from_cursor(
        &adapter->client->negotiated_settings.client_id_storage,
        adapter->allocator,
        aws_byte_cursor_from_buf(&connect_task->client_id));

    config->connect->storage_view.keep_alive_interval_seconds = connect_task->keep_alive_time_secs;
    config->ping_timeout_ms = connect_task->ping_timeout_ms;

    /* Override timeout, rounding up as necessary */
    config->ack_timeout_seconds = (uint32_t)aws_timestamp_convert(
        connect_task->protocol_operation_timeout_ms + AWS_TIMESTAMP_MILLIS - 1,
        AWS_TIMESTAMP_MILLIS,
        AWS_TIMESTAMP_SECS,
        NULL);

    if (connect_task->clean_session) {
        config->session_behavior = AWS_MQTT5_CSBT_CLEAN;
        config->connect->storage_view.session_expiry_interval_seconds = NULL;
    } else {
        config->session_behavior = AWS_MQTT5_CSBT_REJOIN_ALWAYS;
        /* This is a judgement call to translate session expiry to the maximum possible allowed by AWS IoT Core */
        config->connect->session_expiry_interval_seconds = 7 * 24 * 60 * 60;
        config->connect->storage_view.session_expiry_interval_seconds =
            &config->connect->session_expiry_interval_seconds;
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_connect_handler(
    struct aws_mqtt_client_connection_5_impl *adapter,
    struct aws_mqtt_adapter_connect_task *connect_task) {

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter - performing connect safe callback, adapter in state (%d)",
        (void *)adapter,
        (int)adapter->adapter_state);

    if (adapter->adapter_state != AWS_MQTT_AS_STAY_DISCONNECTED) {
        if (connect_task->on_connection_complete) {
            (*connect_task->on_connection_complete)(
                &adapter->base,
                AWS_ERROR_MQTT_ALREADY_CONNECTED,
                0,
                false,
                connect_task->on_connection_complete_user_data);
        }

        return;
    }

    if (adapter->on_disconnect) {
        (*adapter->on_disconnect)(&adapter->base, adapter->on_disconnect_user_data);

        adapter->on_disconnect = NULL;
        adapter->on_disconnect_user_data = NULL;
    }

    adapter->adapter_state = AWS_MQTT_AS_FIRST_CONNECT;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter - resetting mqtt5 client connection and requesting start",
        (void *)adapter);

    /* Update mqtt5 config */
    s_aws_mqtt5_to_mqtt3_adapter_update_config_on_connect(adapter, connect_task);

    aws_mqtt5_client_reset_connection(adapter->client);

    aws_mqtt5_client_change_desired_state(adapter->client, AWS_MCS_CONNECTED, NULL);

    adapter->on_connection_complete = connect_task->on_connection_complete;
    adapter->on_connection_complete_user_data = connect_task->on_connection_complete_user_data;
}

static void s_adapter_connect_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_adapter_connect_task *connect_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = connect_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    s_aws_mqtt5_to_mqtt3_adapter_connect_handler(adapter, connect_task);

done:

    aws_ref_count_release(&adapter->internal_refs);

    s_aws_mqtt_adapter_connect_task_destroy(connect_task);
}

static bool s_aws_mqtt5_listener_publish_received_adapter(
    const struct aws_mqtt5_packet_publish_view *publish,
    void *user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = user_data;
    struct aws_mqtt_client_connection *connection = &adapter->base;

    struct aws_mqtt_subscription_set_publish_received_options incoming_publish_options = {
        .connection = connection,
        .topic = publish->topic,
        .qos = (enum aws_mqtt_qos)publish->qos,
        .retain = publish->retain,
        .dup = publish->duplicate,
        .payload = publish->payload,
    };

    aws_mqtt_subscription_set_on_publish_received(adapter->subscriptions, &incoming_publish_options);

    if (adapter->on_any_publish) {
        (*adapter->on_any_publish)(
            connection,
            &publish->topic,
            &publish->payload,
            publish->duplicate,
            (enum aws_mqtt_qos)publish->qos,
            publish->retain,
            adapter->on_any_publish_user_data);
    }

    return false;
}

struct aws_mqtt_set_interruption_handlers_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted;
    void *on_interrupted_user_data;

    aws_mqtt_client_on_connection_resumed_fn *on_resumed;
    void *on_resumed_user_data;
};

static void s_set_interruption_handlers_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_interruption_handlers_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->on_interrupted = set_task->on_interrupted;
    adapter->on_interrupted_user_data = set_task->on_interrupted_user_data;
    adapter->on_resumed = set_task->on_resumed;
    adapter->on_resumed_user_data = set_task->on_resumed_user_data;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_interruption_handlers_task *s_aws_mqtt_set_interruption_handlers_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted,
    void *on_interrupted_user_data,
    aws_mqtt_client_on_connection_resumed_fn *on_resumed,
    void *on_resumed_user_data) {

    struct aws_mqtt_set_interruption_handlers_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_interruption_handlers_task));

    aws_task_init(
        &set_task->task, s_set_interruption_handlers_task_fn, (void *)set_task, "SetInterruptionHandlersTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->on_interrupted = on_interrupted;
    set_task->on_interrupted_user_data = on_interrupted_user_data;
    set_task->on_resumed = on_resumed;
    set_task->on_resumed_user_data = on_resumed_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_interruption_handlers(
    void *impl,
    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted,
    void *on_interrupted_user_data,
    aws_mqtt_client_on_connection_resumed_fn *on_resumed,
    void *on_resumed_user_data) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_interruption_handlers_task *task = s_aws_mqtt_set_interruption_handlers_task_new(
        adapter->allocator, adapter, on_interrupted, on_interrupted_user_data, on_resumed, on_resumed_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set interruption handlers task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_connection_result_handlers_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_on_connection_success_fn *on_connection_success;
    void *on_connection_success_user_data;

    aws_mqtt_client_on_connection_failure_fn *on_connection_failure;
    void *on_connection_failure_user_data;
};

static void s_set_connection_result_handlers_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_connection_result_handlers_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->on_connection_success = set_task->on_connection_success;
    adapter->on_connection_success_user_data = set_task->on_connection_success_user_data;
    adapter->on_connection_failure = set_task->on_connection_failure;
    adapter->on_connection_failure_user_data = set_task->on_connection_failure_user_data;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_connection_result_handlers_task *s_aws_mqtt_set_connection_result_handlers_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_on_connection_success_fn *on_connection_success,
    void *on_connection_success_user_data,
    aws_mqtt_client_on_connection_failure_fn *on_connection_failure,
    void *on_connection_failure_user_data) {

    struct aws_mqtt_set_connection_result_handlers_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_connection_result_handlers_task));

    aws_task_init(
        &set_task->task, s_set_connection_result_handlers_task_fn, (void *)set_task, "SetConnectionResultHandlersTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->on_connection_success = on_connection_success;
    set_task->on_connection_success_user_data = on_connection_success_user_data;
    set_task->on_connection_failure = on_connection_failure;
    set_task->on_connection_failure_user_data = on_connection_failure_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_connection_result_handlers(
    void *impl,
    aws_mqtt_client_on_connection_success_fn *on_connection_success,
    void *on_connection_success_user_data,
    aws_mqtt_client_on_connection_failure_fn *on_connection_failure,
    void *on_connection_failure_user_data) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_connection_result_handlers_task *task = s_aws_mqtt_set_connection_result_handlers_task_new(
        adapter->allocator,
        adapter,
        on_connection_success,
        on_connection_success_user_data,
        on_connection_failure,
        on_connection_failure_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set connection result handlers task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_on_closed_handler_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_on_connection_closed_fn *on_closed;
    void *on_closed_user_data;
};

static void s_set_on_closed_handler_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_on_closed_handler_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->on_closed = set_task->on_closed;
    adapter->on_closed_user_data = set_task->on_closed_user_data;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_on_closed_handler_task *s_aws_mqtt_set_on_closed_handler_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_on_connection_closed_fn *on_closed,
    void *on_closed_user_data) {

    struct aws_mqtt_set_on_closed_handler_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_on_closed_handler_task));

    aws_task_init(&set_task->task, s_set_on_closed_handler_task_fn, (void *)set_task, "SetOnClosedHandlerTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->on_closed = on_closed;
    set_task->on_closed_user_data = on_closed_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_on_closed_handler(
    void *impl,
    aws_mqtt_client_on_connection_closed_fn *on_closed,
    void *on_closed_user_data) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_on_closed_handler_task *task =
        s_aws_mqtt_set_on_closed_handler_task_new(adapter->allocator, adapter, on_closed, on_closed_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set on closed handler task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_on_termination_handlers_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_on_connection_termination_fn *on_termination_callback;
    void *on_termination_ud;
};

static void s_set_on_termination_handlers_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct aws_mqtt_set_on_termination_handlers_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->on_termination = set_task->on_termination_callback;
    adapter->on_termination_user_data = set_task->on_termination_ud;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_on_termination_handlers_task *s_aws_mqtt_set_on_termination_handler_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_on_connection_termination_fn *on_termination,
    void *on_termination_user_data) {

    struct aws_mqtt_set_on_termination_handlers_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_on_termination_handlers_task));

    aws_task_init(&set_task->task, s_set_on_termination_handlers_task_fn, (void *)set_task, "SetOnClosedHandlerTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->on_termination_callback = on_termination;
    set_task->on_termination_ud = on_termination_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_termination_handler(
    void *impl,
    aws_mqtt_client_on_connection_termination_fn *on_termination,
    void *on_termination_ud) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_on_termination_handlers_task *task =
        s_aws_mqtt_set_on_termination_handler_task_new(adapter->allocator, adapter, on_termination, on_termination_ud);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set on closed handler task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_on_any_publish_handler_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_client_publish_received_fn *on_any_publish;
    void *on_any_publish_user_data;
};

static void s_set_on_any_publish_handler_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_on_any_publish_handler_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->on_any_publish = set_task->on_any_publish;
    adapter->on_any_publish_user_data = set_task->on_any_publish_user_data;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_on_any_publish_handler_task *s_aws_mqtt_set_on_any_publish_handler_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_client_publish_received_fn *on_any_publish,
    void *on_any_publish_user_data) {

    struct aws_mqtt_set_on_any_publish_handler_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_on_any_publish_handler_task));

    aws_task_init(
        &set_task->task, s_set_on_any_publish_handler_task_fn, (void *)set_task, "SetOnAnyPublishHandlerTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->on_any_publish = on_any_publish;
    set_task->on_any_publish_user_data = on_any_publish_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_on_any_publish_handler(
    void *impl,
    aws_mqtt_client_publish_received_fn *on_any_publish,
    void *on_any_publish_user_data) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_on_any_publish_handler_task *task = s_aws_mqtt_set_on_any_publish_handler_task_new(
        adapter->allocator, adapter, on_any_publish, on_any_publish_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set on any publish task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_reconnect_timeout_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    uint64_t min_timeout;
    uint64_t max_timeout;
};

static void s_set_reconnect_timeout_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_reconnect_timeout_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    /* we're in the mqtt5 client's event loop; it's safe to access internal state */
    adapter->client->config->min_reconnect_delay_ms = set_task->min_timeout;
    adapter->client->config->max_reconnect_delay_ms = set_task->max_timeout;
    adapter->client->current_reconnect_delay_ms = set_task->min_timeout;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_reconnect_timeout_task *s_aws_mqtt_set_reconnect_timeout_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    uint64_t min_timeout,
    uint64_t max_timeout) {

    struct aws_mqtt_set_reconnect_timeout_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_reconnect_timeout_task));

    aws_task_init(&set_task->task, s_set_reconnect_timeout_task_fn, (void *)set_task, "SetReconnectTimeoutTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->min_timeout = aws_min_u64(min_timeout, max_timeout);
    set_task->max_timeout = aws_max_u64(min_timeout, max_timeout);

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_reconnect_timeout(
    void *impl,
    uint64_t min_timeout,
    uint64_t max_timeout) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_reconnect_timeout_task *task =
        s_aws_mqtt_set_reconnect_timeout_task_new(adapter->allocator, adapter, min_timeout, max_timeout);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set reconnect timeout task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_http_proxy_options_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    struct aws_http_proxy_config *proxy_config;
};

static void s_set_http_proxy_options_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_http_proxy_options_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    /* we're in the mqtt5 client's event loop; it's safe to access internal state */
    aws_http_proxy_config_destroy(adapter->client->config->http_proxy_config);

    /* move the proxy config from the set task to the client's config */
    adapter->client->config->http_proxy_config = set_task->proxy_config;
    if (adapter->client->config->http_proxy_config != NULL) {
        aws_http_proxy_options_init_from_config(
            &adapter->client->config->http_proxy_options, adapter->client->config->http_proxy_config);
    }

    /* don't clean up the proxy config if it was successfully assigned to the mqtt5 client */
    set_task->proxy_config = NULL;

done:

    aws_ref_count_release(&adapter->internal_refs);

    /* If the task was canceled we need to clean this up because it didn't get assigned to the mqtt5 client */
    aws_http_proxy_config_destroy(set_task->proxy_config);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_http_proxy_options_task *s_aws_mqtt_set_http_proxy_options_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    struct aws_http_proxy_options *proxy_options) {

    struct aws_http_proxy_config *proxy_config =
        aws_http_proxy_config_new_tunneling_from_proxy_options(allocator, proxy_options);
    if (proxy_config == NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_mqtt_set_http_proxy_options_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_http_proxy_options_task));

    aws_task_init(&set_task->task, s_set_http_proxy_options_task_fn, (void *)set_task, "SetHttpProxyOptionsTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->proxy_config = proxy_config;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_http_proxy_options(
    void *impl,
    struct aws_http_proxy_options *proxy_options) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_http_proxy_options_task *task =
        s_aws_mqtt_set_http_proxy_options_task_new(adapter->allocator, adapter, proxy_options);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set http proxy options task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_use_websockets_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    aws_mqtt_transform_websocket_handshake_fn *transformer;
    void *transformer_user_data;
};

static void s_aws_mqtt5_adapter_websocket_handshake_completion_fn(
    struct aws_http_message *request,
    int error_code,
    void *complete_ctx) {

    struct aws_mqtt_client_connection_5_impl *adapter = complete_ctx;

    (*adapter->mqtt5_websocket_handshake_completion_function)(
        request,
        s_translate_mqtt5_error_code_to_mqtt311(error_code),
        adapter->mqtt5_websocket_handshake_completion_user_data);

    aws_ref_count_release(&adapter->internal_refs);
}

static void s_aws_mqtt5_adapter_transform_websocket_handshake_fn(
    struct aws_http_message *request,
    void *user_data,
    aws_mqtt5_transform_websocket_handshake_complete_fn *complete_fn,
    void *complete_ctx) {

    struct aws_mqtt_client_connection_5_impl *adapter = user_data;

    if (adapter->websocket_handshake_transformer == NULL) {
        (*complete_fn)(request, AWS_ERROR_SUCCESS, complete_ctx);
    } else {
        aws_ref_count_acquire(&adapter->internal_refs);
        adapter->mqtt5_websocket_handshake_completion_function = complete_fn;
        adapter->mqtt5_websocket_handshake_completion_user_data = complete_ctx;

        (*adapter->websocket_handshake_transformer)(
            request,
            adapter->websocket_handshake_transformer_user_data,
            s_aws_mqtt5_adapter_websocket_handshake_completion_fn,
            adapter);
    }
}

static void s_set_use_websockets_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_use_websockets_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    adapter->websocket_handshake_transformer = set_task->transformer;
    adapter->websocket_handshake_transformer_user_data = set_task->transformer_user_data;

    /* we're in the mqtt5 client's event loop; it's safe to access its internal state */
    adapter->client->config->websocket_handshake_transform = s_aws_mqtt5_adapter_transform_websocket_handshake_fn;
    adapter->client->config->websocket_handshake_transform_user_data = adapter;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_use_websockets_task *s_aws_mqtt_set_use_websockets_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    aws_mqtt_transform_websocket_handshake_fn *transformer,
    void *transformer_user_data) {

    struct aws_mqtt_set_use_websockets_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_use_websockets_task));

    aws_task_init(&set_task->task, s_set_use_websockets_task_fn, (void *)set_task, "SetUseWebsocketsTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->transformer = transformer;
    set_task->transformer_user_data = transformer_user_data;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_use_websockets(
    void *impl,
    aws_mqtt_transform_websocket_handshake_fn *transformer,
    void *transformer_user_data,
    aws_mqtt_validate_websocket_handshake_fn *validator,
    void *validator_user_data) {

    /* mqtt5 doesn't use these */
    (void)validator;
    (void)validator_user_data;

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_use_websockets_task *task =
        s_aws_mqtt_set_use_websockets_task_new(adapter->allocator, adapter, transformer, transformer_user_data);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set use websockets task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_host_resolution_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    struct aws_host_resolution_config host_resolution_config;
};

static void s_set_host_resolution_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_host_resolution_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    /* we're in the mqtt5 client's event loop; it's safe to access internal state */
    adapter->client->config->host_resolution_override = set_task->host_resolution_config;

done:

    aws_ref_count_release(&adapter->internal_refs);

    aws_mem_release(set_task->allocator, set_task);
}

static struct aws_mqtt_set_host_resolution_task *s_aws_mqtt_set_host_resolution_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    const struct aws_host_resolution_config *host_resolution_config) {

    struct aws_mqtt_set_host_resolution_task *set_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_host_resolution_task));

    aws_task_init(&set_task->task, s_set_host_resolution_task_fn, (void *)set_task, "SetHostResolutionTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);
    set_task->host_resolution_config = *host_resolution_config;

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_host_resolution_options(
    void *impl,
    const struct aws_host_resolution_config *host_resolution_config) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_host_resolution_task *task =
        s_aws_mqtt_set_host_resolution_task_new(adapter->allocator, adapter, host_resolution_config);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set reconnect timeout task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_will_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    struct aws_byte_buf topic_buffer;
    enum aws_mqtt_qos qos;
    bool retain;
    struct aws_byte_buf payload_buffer;
};

static void s_aws_mqtt_set_will_task_destroy(struct aws_mqtt_set_will_task *task) {
    if (task == NULL) {
        return;
    }

    aws_byte_buf_clean_up(&task->topic_buffer);
    aws_byte_buf_clean_up(&task->payload_buffer);

    aws_mem_release(task->allocator, task);
}

static void s_set_will_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_will_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    /* we're in the mqtt5 client's event loop; it's safe to access internal state */
    struct aws_mqtt5_packet_connect_storage *connect = adapter->client->config->connect;

    /* clean up the old will if necessary */
    if (connect->will != NULL) {
        aws_mqtt5_packet_publish_storage_clean_up(connect->will);
        aws_mem_release(connect->allocator, connect->will);
        connect->will = NULL;
    }

    struct aws_mqtt5_packet_publish_view will = {
        .topic = aws_byte_cursor_from_buf(&set_task->topic_buffer),
        .qos = (enum aws_mqtt5_qos)set_task->qos,
        .retain = set_task->retain,
        .payload = aws_byte_cursor_from_buf(&set_task->payload_buffer),
    };

    /* make a new will */
    connect->will = aws_mem_calloc(connect->allocator, 1, sizeof(struct aws_mqtt5_packet_publish_storage));
    aws_mqtt5_packet_publish_storage_init(connect->will, connect->allocator, &will);

    /* manually update the storage view's will reference */
    connect->storage_view.will = &connect->will->storage_view;

done:

    aws_ref_count_release(&adapter->internal_refs);

    s_aws_mqtt_set_will_task_destroy(set_task);
}

static struct aws_mqtt_set_will_task *s_aws_mqtt_set_will_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload) {

    if (topic == NULL) {
        return NULL;
    }

    struct aws_mqtt_set_will_task *set_task = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_will_task));

    aws_task_init(&set_task->task, s_set_will_task_fn, (void *)set_task, "SetWillTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);

    set_task->qos = qos;
    set_task->retain = retain;
    aws_byte_buf_init_copy_from_cursor(&set_task->topic_buffer, allocator, *topic);
    if (payload != NULL) {
        aws_byte_buf_init_copy_from_cursor(&set_task->payload_buffer, allocator, *payload);
    }

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_will(
    void *impl,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    /* check qos */
    if (qos < 0 || qos > AWS_MQTT_QOS_EXACTLY_ONCE) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invalid qos for will", (void *)adapter);
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_QOS);
    }

    /* check topic */
    if (!aws_mqtt_is_valid_topic(topic)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invalid topic for will", (void *)adapter);
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
    }

    struct aws_mqtt_set_will_task *task =
        s_aws_mqtt_set_will_task_new(adapter->allocator, adapter, topic, qos, retain, payload);
    if (task == NULL) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: failed to create set will task", (void *)adapter);
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

struct aws_mqtt_set_login_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_5_impl *adapter;

    struct aws_byte_buf username_buffer;
    struct aws_byte_buf password_buffer;
};

static void s_aws_mqtt_set_login_task_destroy(struct aws_mqtt_set_login_task *task) {
    if (task == NULL) {
        return;
    }

    aws_byte_buf_clean_up_secure(&task->username_buffer);
    aws_byte_buf_clean_up_secure(&task->password_buffer);

    aws_mem_release(task->allocator, task);
}

static void s_set_login_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_set_login_task *set_task = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = set_task->adapter;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    struct aws_byte_cursor username_cursor = aws_byte_cursor_from_buf(&set_task->username_buffer);
    struct aws_byte_cursor password_cursor = aws_byte_cursor_from_buf(&set_task->password_buffer);

    /* we're in the mqtt5 client's event loop; it's safe to access internal state */
    struct aws_mqtt5_packet_connect_storage *old_connect = adapter->client->config->connect;

    /*
     * Packet storage stores binary data in a single buffer.  The safest way to replace some binary data is
     * to make a new storage from the old storage, deleting the old storage after construction is complete.
     */
    struct aws_mqtt5_packet_connect_view new_connect_view = old_connect->storage_view;

    if (set_task->username_buffer.len > 0) {
        new_connect_view.username = &username_cursor;
    } else {
        new_connect_view.username = NULL;
    }

    if (set_task->password_buffer.len > 0) {
        new_connect_view.password = &password_cursor;
    } else {
        new_connect_view.password = NULL;
    }

    if (aws_mqtt5_packet_connect_view_validate(&new_connect_view)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter - invalid CONNECT username or password",
            (void *)adapter);
        goto done;
    }

    struct aws_mqtt5_packet_connect_storage *new_connect =
        aws_mem_calloc(adapter->allocator, 1, sizeof(struct aws_mqtt5_packet_connect_storage));
    aws_mqtt5_packet_connect_storage_init(new_connect, adapter->allocator, &new_connect_view);

    adapter->client->config->connect = new_connect;
    aws_mqtt5_packet_connect_storage_clean_up(old_connect);
    aws_mem_release(old_connect->allocator, old_connect);

done:

    aws_ref_count_release(&adapter->internal_refs);

    s_aws_mqtt_set_login_task_destroy(set_task);
}

static struct aws_mqtt_set_login_task *s_aws_mqtt_set_login_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client_connection_5_impl *adapter,
    const struct aws_byte_cursor *username,
    const struct aws_byte_cursor *password) {

    struct aws_mqtt_set_login_task *set_task = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_set_login_task));

    aws_task_init(&set_task->task, s_set_login_task_fn, (void *)set_task, "SetLoginTask");
    set_task->allocator = adapter->allocator;
    set_task->adapter = (struct aws_mqtt_client_connection_5_impl *)aws_ref_count_acquire(&adapter->internal_refs);

    if (username != NULL) {
        aws_byte_buf_init_copy_from_cursor(&set_task->username_buffer, allocator, *username);
    }

    if (password != NULL) {
        aws_byte_buf_init_copy_from_cursor(&set_task->password_buffer, allocator, *password);
    }

    return set_task;
}

static int s_aws_mqtt_client_connection_5_set_login(
    void *impl,
    const struct aws_byte_cursor *username,
    const struct aws_byte_cursor *password) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    struct aws_mqtt_set_login_task *task =
        s_aws_mqtt_set_login_task_new(adapter->allocator, adapter, username, password);
    if (task == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: failed to create set login task, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(adapter->loop, &task->task);

    return AWS_OP_SUCCESS;
}

static void s_aws_mqtt5_to_mqtt3_adapter_on_zero_internal_refs(void *context) {
    struct aws_mqtt_client_connection_5_impl *adapter = context;

    s_aws_mqtt_adapter_final_destroy(adapter);
}

static void s_aws_mqtt5_to_mqtt3_adapter_on_listener_detached(void *context) {
    struct aws_mqtt_client_connection_5_impl *adapter = context;

    /*
     * Release the single internal reference that we started with.  Only ephemeral references for cross-thread
     * tasks might remain, and they will disappear quickly.
     */
    aws_ref_count_release(&adapter->internal_refs);
}

static struct aws_mqtt_client_connection *s_aws_mqtt_client_connection_5_acquire(void *impl) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    aws_ref_count_acquire(&adapter->external_refs);

    return &adapter->base;
}

static void s_aws_mqtt5_to_mqtt3_adapter_on_zero_external_refs(void *impl) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    /*
     * When the adapter's exernal ref count goes to zero, here's what we want to do:
     *
     *  (1) Release the client listener, starting its asynchronous shutdown process (since we're the only user
     *      of it)
     *  (2) Wait for the client listener to notify us that asynchronous shutdown is over.  At this point we
     *      are guaranteed that no more callbacks from the mqtt5 client will reach us.
     *  (3) Release the single internal ref we started with when the adapter was created.
     *  (4) On last internal ref, we can safely release the mqtt5 client and synchronously clean up all other
     *      resources
     *
     *  Step (1) is done here.
     *  Steps (2) and (3) are accomplished by s_aws_mqtt5_to_mqtt3_adapter_on_listener_detached
     *  Step (4) is completed by s_aws_mqtt5_to_mqtt3_adapter_on_zero_internal_refs
     */
    aws_mqtt5_listener_release(adapter->listener);
}

static void s_aws_mqtt_client_connection_5_release(void *impl) {
    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    aws_ref_count_release(&adapter->external_refs);
}

/*
 * When submitting an operation (across threads), we not only need to keep the adapter alive, we also need to keep
 * the operation alive since it's technically already being tracked within the adapter's operational state.
 *
 * Note: we may not truly need the operation ref but it's safer to keep it.
 */
static void s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation) {
    if (!operation->holding_adapter_ref) {
        operation->holding_adapter_ref = true;
        aws_ref_count_acquire(&operation->adapter->internal_refs);
    }

    aws_mqtt5_to_mqtt3_adapter_operation_acquire(operation);
}

/*
 * Once an operation has been received on the adapter's event loop, whether reject or accepted, we must release the
 * transient references to the operation and adapter
 */
static void s_aws_mqtt5_to_mqtt3_adapter_operation_release_cross_thread_refs(
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation) {
    if (operation->holding_adapter_ref) {
        operation->holding_adapter_ref = false;
        aws_ref_count_release(&operation->adapter->internal_refs);
    }

    aws_mqtt5_to_mqtt3_adapter_operation_release(operation);
}

static void s_adapter_publish_operation_destroy(void *context) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = context;
    if (operation == NULL) {
        return;
    }

    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *publish_op = operation->impl;

    struct aws_mqtt_client_connection_5_impl *adapter_to_release = NULL;
    if (publish_op->base.holding_adapter_ref) {
        adapter_to_release = publish_op->base.adapter;
    }

    /* We're going away before our MQTT5 operation, make sure it doesn't try to call us back when it completes */
    publish_op->publish_op->completion_options.completion_callback = NULL;
    publish_op->publish_op->completion_options.completion_user_data = NULL;

    aws_mqtt5_operation_release(&publish_op->publish_op->base);

    aws_mem_release(operation->allocator, operation);

    if (adapter_to_release != NULL) {
        aws_ref_count_release(&adapter_to_release->internal_refs);
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_publish_completion_fn(
    enum aws_mqtt5_packet_type packet_type,
    const void *packet,
    int error_code,
    void *complete_ctx) {

    int error_code_final = error_code;

    if (error_code_final == AWS_ERROR_SUCCESS && packet_type == AWS_MQTT5_PT_PUBACK) {
        const struct aws_mqtt5_packet_puback_view *puback_view = packet;
        if (puback_view->reason_code >= 128) {
            error_code_final = AWS_ERROR_MQTT_ACK_REASON_CODE_FAILURE;
        }
    }

    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *publish_op = complete_ctx;

    if (publish_op->on_publish_complete != NULL) {
        (*publish_op->on_publish_complete)(
            &publish_op->base.adapter->base,
            publish_op->base.id,
            error_code_final,
            publish_op->on_publish_complete_user_data);
    }

    aws_mqtt5_to_mqtt3_adapter_operation_table_remove_operation(
        &publish_op->base.adapter->operational_state, publish_op->base.id);
}

static void s_fail_publish(void *impl, int error_code) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *publish_op = impl;

    if (publish_op->on_publish_complete != NULL) {
        (*publish_op->on_publish_complete)(
            &publish_op->base.adapter->base,
            publish_op->base.id,
            error_code,
            publish_op->on_publish_complete_user_data);
    }
}

static struct aws_mqtt5_to_mqtt3_adapter_operation_vtable s_publish_vtable = {
    .fail_fn = s_fail_publish,
};

struct aws_mqtt5_to_mqtt3_adapter_operation_publish *aws_mqtt5_to_mqtt3_adapter_operation_new_publish(
    struct aws_allocator *allocator,
    const struct aws_mqtt5_to_mqtt3_adapter_publish_options *options) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *publish_op =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_to_mqtt3_adapter_operation_publish));

    publish_op->base.allocator = allocator;
    aws_ref_count_init(&publish_op->base.ref_count, publish_op, s_adapter_publish_operation_destroy);
    publish_op->base.impl = publish_op;
    publish_op->base.vtable = &s_publish_vtable;
    publish_op->base.type = AWS_MQTT5TO3_AOT_PUBLISH;
    publish_op->base.adapter = options->adapter;
    publish_op->base.holding_adapter_ref = false;

    struct aws_mqtt5_packet_publish_view publish_view = {
        .topic = options->topic,
        .qos = (enum aws_mqtt5_qos)options->qos,
        .payload = options->payload,
        .retain = options->retain,
    };

    struct aws_mqtt5_publish_completion_options publish_completion_options = {
        .completion_callback = s_aws_mqtt5_to_mqtt3_adapter_publish_completion_fn,
        .completion_user_data = publish_op,
    };

    publish_op->publish_op = aws_mqtt5_operation_publish_new(
        allocator, options->adapter->client, &publish_view, &publish_completion_options);
    if (publish_op->publish_op == NULL) {
        goto error;
    }

    publish_op->on_publish_complete = options->on_complete;
    publish_op->on_publish_complete_user_data = options->on_complete_userdata;

    return publish_op;

error:

    aws_mqtt5_to_mqtt3_adapter_operation_release(&publish_op->base);

    return NULL;
}

void s_adapter_publish_submission_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *operation = arg;

    struct aws_mqtt_client_connection_5_impl *adapter = operation->base.adapter;

    aws_mqtt5_client_submit_operation_internal(
        adapter->client, &operation->publish_op->base, status != AWS_TASK_STATUS_RUN_READY);

    /*
     * The operation has been submitted in-thread.  We can release the transient refs (operation, adapter) needed to
     * keep things alive during the handover
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_release_cross_thread_refs(&operation->base);
}

static uint16_t s_aws_mqtt_client_connection_5_publish(
    void *impl,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload,
    aws_mqtt_op_complete_fn *on_complete,
    void *userdata) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;
    AWS_LOGF_DEBUG(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invoking publish API", (void *)adapter);

    /* check qos */
    if (qos < 0 || qos > AWS_MQTT_QOS_EXACTLY_ONCE) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invalid qos for publish", (void *)adapter);
        aws_raise_error(AWS_ERROR_MQTT_INVALID_QOS);
        return 0;
    }

    if (!aws_mqtt_is_valid_topic(topic)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invalid topic for publish", (void *)adapter);
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    struct aws_byte_cursor topic_cursor = *topic;
    struct aws_byte_cursor payload_cursor;
    AWS_ZERO_STRUCT(payload_cursor);
    if (payload != NULL) {
        payload_cursor = *payload;
    }

    struct aws_mqtt5_to_mqtt3_adapter_publish_options publish_options = {
        .adapter = adapter,
        .topic = topic_cursor,
        .qos = qos,
        .retain = retain,
        .payload = payload_cursor,
        .on_complete = on_complete,
        .on_complete_userdata = userdata,
    };

    struct aws_mqtt5_to_mqtt3_adapter_operation_publish *operation =
        aws_mqtt5_to_mqtt3_adapter_operation_new_publish(adapter->allocator, &publish_options);
    if (operation == NULL) {
        return 0;
    }

    if (aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(&adapter->operational_state, &operation->base)) {
        goto error;
    }

    uint16_t synthetic_id = operation->base.id;

    /*
     * While in-transit to the adapter event loop, we take refs to both the operation and the adapter so that we
     * are guaranteed they are still alive when the cross-thread submission task is run.
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(&operation->base);

    aws_task_init(
        &operation->base.submission_task,
        s_adapter_publish_submission_fn,
        operation,
        "Mqtt5ToMqtt3AdapterPublishSubmission");

    aws_event_loop_schedule_task_now(adapter->loop, &operation->base.submission_task);

    return synthetic_id;

error:

    aws_mqtt5_to_mqtt3_adapter_operation_release(&operation->base);

    return 0;
}

static void s_adapter_subscribe_operation_destroy(void *context) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = context;
    if (operation == NULL) {
        return;
    }

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op = operation->impl;

    size_t subscription_count = aws_array_list_length(&subscribe_op->subscriptions);
    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt_subscription_set_subscription_record *record = NULL;
        aws_array_list_get_at(&subscribe_op->subscriptions, &record, i);

        aws_mqtt_subscription_set_subscription_record_destroy(record);
    }
    aws_array_list_clean_up(&subscribe_op->subscriptions);

    struct aws_mqtt_client_connection_5_impl *adapter_to_release = NULL;
    if (subscribe_op->base.holding_adapter_ref) {
        adapter_to_release = subscribe_op->base.adapter;
    }

    /* We're going away before our MQTT5 operation, make sure it doesn't try to call us back when it completes */
    if (subscribe_op->subscribe_op != NULL) {
        subscribe_op->subscribe_op->completion_options.completion_callback = NULL;
        subscribe_op->subscribe_op->completion_options.completion_user_data = NULL;

        aws_mqtt5_operation_release(&subscribe_op->subscribe_op->base);
    }

    aws_mem_release(operation->allocator, operation);

    if (adapter_to_release != NULL) {
        aws_ref_count_release(&adapter_to_release->internal_refs);
    }
}

static enum aws_mqtt_qos s_convert_mqtt5_suback_reason_code_to_mqtt3_granted_qos(
    enum aws_mqtt5_suback_reason_code reason_code) {
    switch (reason_code) {
        case AWS_MQTT5_SARC_GRANTED_QOS_0:
        case AWS_MQTT5_SARC_GRANTED_QOS_1:
        case AWS_MQTT5_SARC_GRANTED_QOS_2:
            return (enum aws_mqtt_qos)reason_code;

        default:
            return AWS_MQTT_QOS_FAILURE;
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_subscribe_completion_helper(
    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op,
    const struct aws_mqtt5_packet_suback_view *suback,
    int error_code) {

    struct aws_mqtt_client_connection_5_impl *adapter = subscribe_op->base.adapter;
    struct aws_mqtt_subscription_set_subscription_record *record = NULL;

    if (subscribe_op->on_suback != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, completing single-topic subscribe",
            (void *)adapter);

        struct aws_byte_cursor topic_filter;
        AWS_ZERO_STRUCT(topic_filter);

        enum aws_mqtt_qos granted_qos = AWS_MQTT_QOS_AT_MOST_ONCE;

        size_t subscription_count = aws_array_list_length(&subscribe_op->subscriptions);
        if (subscription_count > 0) {
            aws_array_list_get_at(&subscribe_op->subscriptions, &record, 0);
            topic_filter = record->subscription_view.topic_filter;
        }

        if (suback != NULL) {
            if (suback->reason_code_count > 0) {
                granted_qos = s_convert_mqtt5_suback_reason_code_to_mqtt3_granted_qos(suback->reason_codes[0]);
            }
        } else {
            granted_qos = AWS_MQTT_QOS_FAILURE;
        }
        (*subscribe_op->on_suback)(
            &adapter->base,
            subscribe_op->base.id,
            &topic_filter,
            granted_qos,
            error_code,
            subscribe_op->on_suback_user_data);
    }

    if (subscribe_op->on_multi_suback != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, completing multi-topic subscribe",
            (void *)adapter);

        if (suback == NULL) {
            (*subscribe_op->on_multi_suback)(
                &adapter->base, subscribe_op->base.id, NULL, error_code, subscribe_op->on_multi_suback_user_data);
        } else {
            AWS_VARIABLE_LENGTH_ARRAY(
                struct aws_mqtt_topic_subscription, multi_sub_subscription_buf, suback->reason_code_count);
            AWS_VARIABLE_LENGTH_ARRAY(
                struct aws_mqtt_topic_subscription *, multi_sub_subscription_ptr_buf, suback->reason_code_count);
            struct aws_mqtt_topic_subscription *subscription_ptr =
                (struct aws_mqtt_topic_subscription *)multi_sub_subscription_buf;

            struct aws_array_list multi_sub_list;
            aws_array_list_init_static(
                &multi_sub_list,
                multi_sub_subscription_ptr_buf,
                suback->reason_code_count,
                sizeof(struct aws_mqtt_topic_subscription *));

            size_t subscription_count = aws_array_list_length(&subscribe_op->subscriptions);

            for (size_t i = 0; i < suback->reason_code_count; ++i) {
                struct aws_mqtt_topic_subscription *subscription = subscription_ptr + i;
                AWS_ZERO_STRUCT(*subscription);

                subscription->qos = s_convert_mqtt5_suback_reason_code_to_mqtt3_granted_qos(suback->reason_codes[i]);

                if (i < subscription_count) {
                    aws_array_list_get_at(&subscribe_op->subscriptions, &record, i);

                    subscription->topic = record->subscription_view.topic_filter;
                    subscription->on_publish = record->subscription_view.on_publish_received;
                    subscription->on_publish_ud = record->subscription_view.callback_user_data;
                    subscription->on_cleanup = record->subscription_view.on_cleanup;
                }

                aws_array_list_push_back(&multi_sub_list, &subscription);
            }
            (*subscribe_op->on_multi_suback)(
                &adapter->base,
                subscribe_op->base.id,
                &multi_sub_list,
                error_code,
                subscribe_op->on_multi_suback_user_data);
        }
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_subscribe_completion_fn(
    const struct aws_mqtt5_packet_suback_view *suback,
    int error_code,
    void *complete_ctx) {

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op = complete_ctx;
    struct aws_mqtt_client_connection_5_impl *adapter = subscribe_op->base.adapter;

    s_aws_mqtt5_to_mqtt3_adapter_subscribe_completion_helper(subscribe_op, suback, error_code);

    aws_mqtt5_to_mqtt3_adapter_operation_table_remove_operation(&adapter->operational_state, subscribe_op->base.id);
}

static int s_validate_adapter_subscribe_options(
    size_t subscription_count,
    struct aws_mqtt_topic_subscription *subscriptions,
    struct aws_mqtt_client_connection_5_impl *adapter) {
    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt_topic_subscription *subscription = subscriptions + i;

        /* check qos */
        if (subscription->qos < 0 || subscription->qos > AWS_MQTT_QOS_EXACTLY_ONCE) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, invalid qos for subscribe", (void *)adapter);
            return aws_raise_error(AWS_ERROR_MQTT_INVALID_QOS);
        }

        /* check topic */
        if (!aws_mqtt_is_valid_topic_filter(&subscription->topic)) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
                "id=%p: mqtt3-to-5-adapter, invalid topic filter for subscribe",
                (void *)adapter);
            return aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_mqtt5_to_mqtt3_adapter_build_subscribe(
    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op,
    size_t subscription_count,
    struct aws_mqtt_topic_subscription *subscriptions) {
    struct aws_allocator *allocator = subscribe_op->base.allocator;

    /* make persistent adapter sub array */
    aws_array_list_init_dynamic(
        &subscribe_op->subscriptions,
        allocator,
        subscription_count,
        sizeof(struct aws_mqtt_subscription_set_subscription_record *));

    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt_topic_subscription *subscription_options = &subscriptions[i];

        struct aws_mqtt_subscription_set_subscription_options subscription_record_options = {
            .topic_filter = subscription_options->topic,
            .qos = (enum aws_mqtt5_qos)subscription_options->qos,
            .on_publish_received = subscription_options->on_publish,
            .callback_user_data = subscription_options->on_publish_ud,
            .on_cleanup = subscription_options->on_cleanup,
        };
        struct aws_mqtt_subscription_set_subscription_record *record =
            aws_mqtt_subscription_set_subscription_record_new(allocator, &subscription_record_options);

        aws_array_list_push_back(&subscribe_op->subscriptions, &record);
    }

    /* make temp mqtt5 subscription view array */
    AWS_VARIABLE_LENGTH_ARRAY(struct aws_mqtt5_subscription_view, mqtt5_subscription_buffer, subscription_count);
    struct aws_mqtt5_subscription_view *subscription_ptr = mqtt5_subscription_buffer;
    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt5_subscription_view *subscription = subscription_ptr + i;
        AWS_ZERO_STRUCT(*subscription);

        subscription->topic_filter = subscriptions[i].topic;
        subscription->qos = (enum aws_mqtt5_qos)subscriptions[i].qos;
    }

    struct aws_mqtt5_packet_subscribe_view subscribe_view = {
        .subscriptions = subscription_ptr,
        .subscription_count = subscription_count,
    };

    struct aws_mqtt5_subscribe_completion_options subscribe_completion_options = {
        .completion_callback = s_aws_mqtt5_to_mqtt3_adapter_subscribe_completion_fn,
        .completion_user_data = subscribe_op,
    };

    subscribe_op->subscribe_op = aws_mqtt5_operation_subscribe_new(
        allocator, subscribe_op->base.adapter->client, &subscribe_view, &subscribe_completion_options);

    if (subscribe_op->subscribe_op == NULL) {
        /* subscribe options validation will have been raised as the error */
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_fail_subscribe(void *impl, int error_code) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op = impl;

    s_aws_mqtt5_to_mqtt3_adapter_subscribe_completion_helper(subscribe_op, NULL, error_code);
}

static struct aws_mqtt5_to_mqtt3_adapter_operation_vtable s_subscribe_vtable = {
    .fail_fn = s_fail_subscribe,
};

struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *aws_mqtt5_to_mqtt3_adapter_operation_new_subscribe(
    struct aws_allocator *allocator,
    const struct aws_mqtt5_to_mqtt3_adapter_subscribe_options *options,
    struct aws_mqtt_client_connection_5_impl *adapter) {

    if (s_validate_adapter_subscribe_options(options->subscription_count, options->subscriptions, adapter)) {
        return NULL;
    }

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe));

    subscribe_op->base.allocator = allocator;
    aws_ref_count_init(&subscribe_op->base.ref_count, subscribe_op, s_adapter_subscribe_operation_destroy);
    subscribe_op->base.impl = subscribe_op;
    subscribe_op->base.vtable = &s_subscribe_vtable;
    subscribe_op->base.type = AWS_MQTT5TO3_AOT_SUBSCRIBE;
    subscribe_op->base.adapter = options->adapter;
    subscribe_op->base.holding_adapter_ref = false;

    /*
     * If we're a regular subscribe, build the mqtt5 operation now.  Otherwise, we have to wait until
     * we're on the event loop thread and it's safe to query the subscription set.
     */
    if (options->subscription_count > 0) {
        if (s_aws_mqtt5_to_mqtt3_adapter_build_subscribe(
                subscribe_op, options->subscription_count, options->subscriptions)) {
            goto error;
        }
    }

    subscribe_op->on_suback = options->on_suback;
    subscribe_op->on_suback_user_data = options->on_suback_user_data;
    subscribe_op->on_multi_suback = options->on_multi_suback;
    subscribe_op->on_multi_suback_user_data = options->on_multi_suback_user_data;

    return subscribe_op;

error:

    aws_mqtt5_to_mqtt3_adapter_operation_release(&subscribe_op->base);

    return NULL;
}

static int s_aws_mqtt5_to_mqtt3_adapter_build_resubscribe(
    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *subscribe_op,
    struct aws_array_list *full_subscriptions) {
    size_t subscription_count = aws_array_list_length(full_subscriptions);

    AWS_VARIABLE_LENGTH_ARRAY(struct aws_mqtt_topic_subscription, multi_sub_subscriptions, subscription_count);

    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt_subscription_set_subscription_options *existing_subscription = NULL;
        aws_array_list_get_at_ptr(full_subscriptions, (void **)&existing_subscription, i);

        multi_sub_subscriptions[i].topic = existing_subscription->topic_filter;
        multi_sub_subscriptions[i].qos = (enum aws_mqtt_qos)existing_subscription->qos;
        multi_sub_subscriptions[i].on_publish = existing_subscription->on_publish_received;
        multi_sub_subscriptions[i].on_cleanup = existing_subscription->on_cleanup;
        multi_sub_subscriptions[i].on_publish_ud = existing_subscription->callback_user_data;
    }

    return s_aws_mqtt5_to_mqtt3_adapter_build_subscribe(subscribe_op, subscription_count, multi_sub_subscriptions);
}

void s_adapter_subscribe_submission_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *operation = arg;
    struct aws_mqtt_client_connection_5_impl *adapter = operation->base.adapter;

    struct aws_array_list full_subscriptions;
    AWS_ZERO_STRUCT(full_subscriptions);

    /* If we're a re-subscribe, it's now safe to build the subscription set and MQTT5 subscribe op */
    if (operation->subscribe_op == NULL) {
        aws_mqtt_subscription_set_get_subscriptions(adapter->subscriptions, &full_subscriptions);
        size_t subscription_count = aws_array_list_length(&full_subscriptions);
        if (subscription_count == 0 || s_aws_mqtt5_to_mqtt3_adapter_build_resubscribe(operation, &full_subscriptions)) {
            /* There's either nothing to do (no subscriptions) or we failed to build the op (should never happen) */
            int error_code = aws_last_error();
            if (subscription_count == 0) {
                error_code = AWS_ERROR_MQTT_CONNECTION_RESUBSCRIBE_NO_TOPICS;
            }

            if (operation->on_multi_suback) {
                (*operation->on_multi_suback)(
                    &adapter->base, operation->base.id, NULL, error_code, operation->on_multi_suback_user_data);
            }

            /*
             * Remove the persistent ref represented by being seated in the incomplete operations table.
             * The other (transient) ref gets released at the end of the function.
             */
            aws_mqtt5_to_mqtt3_adapter_operation_table_remove_operation(
                &adapter->operational_state, operation->base.id);
            goto complete;
        }
    }

    size_t subscription_count = aws_array_list_length(&operation->subscriptions);
    for (size_t i = 0; i < subscription_count; ++i) {
        struct aws_mqtt_subscription_set_subscription_record *record = NULL;
        aws_array_list_get_at(&operation->subscriptions, &record, i);

        aws_mqtt_subscription_set_add_subscription(adapter->subscriptions, &record->subscription_view);
    }

    aws_mqtt5_client_submit_operation_internal(
        adapter->client, &operation->subscribe_op->base, status != AWS_TASK_STATUS_RUN_READY);

complete:

    aws_array_list_clean_up(&full_subscriptions);

    /*
     * The operation has been submitted in-thread.  We can release the transient refs (operation, adapter) needed to
     * keep things alive during the handover
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_release_cross_thread_refs(&operation->base);
}

static uint16_t s_aws_mqtt_client_connection_5_subscribe(
    void *impl,
    const struct aws_byte_cursor *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_client_publish_received_fn *on_publish,
    void *on_publish_ud,
    aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
    aws_mqtt_suback_fn *on_suback,
    void *on_suback_user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;
    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, single-topic subscribe API invoked",
        (void *)adapter);

    struct aws_mqtt_topic_subscription subscription = {
        .topic = *topic_filter,
        .qos = qos,
        .on_publish = on_publish,
        .on_cleanup = on_ud_cleanup,
        .on_publish_ud = on_publish_ud,
    };

    struct aws_mqtt5_to_mqtt3_adapter_subscribe_options subscribe_options = {
        .adapter = adapter,
        .subscriptions = &subscription,
        .subscription_count = 1,
        .on_suback = on_suback,
        .on_suback_user_data = on_suback_user_data,
    };

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *operation =
        aws_mqtt5_to_mqtt3_adapter_operation_new_subscribe(adapter->allocator, &subscribe_options, adapter);
    if (operation == NULL) {
        return 0;
    }

    if (aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(&adapter->operational_state, &operation->base)) {
        goto error;
    }

    uint16_t synthetic_id = operation->base.id;

    /*
     * While in-transit to the adapter event loop, we take refs to both the operation and the adapter so that we
     * are guaranteed they are still alive when the cross-thread submission task is run.
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(&operation->base);

    aws_task_init(
        &operation->base.submission_task,
        s_adapter_subscribe_submission_fn,
        operation,
        "Mqtt5ToMqtt3AdapterSubscribeSubmission");

    aws_event_loop_schedule_task_now(adapter->loop, &operation->base.submission_task);

    return synthetic_id;

error:

    ;
    int error_code = aws_last_error();
    AWS_LOGF_ERROR(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, single-topic subscribe failed synchronously, error code %d(%s)",
        (void *)adapter,
        error_code,
        aws_error_debug_str(error_code));

    aws_mqtt5_to_mqtt3_adapter_operation_release(&operation->base);

    return 0;
}

static uint16_t s_aws_mqtt_client_connection_5_subscribe_multiple(
    void *impl,
    const struct aws_array_list *topic_filters,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, multi-topic subscribe API invoked", (void *)adapter);

    if (topic_filters == NULL || aws_array_list_length(topic_filters) == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter multi-topic subscribe empty", (void *)adapter);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return 0;
    }

    struct aws_mqtt_topic_subscription *subscriptions = topic_filters->data;

    struct aws_mqtt5_to_mqtt3_adapter_subscribe_options subscribe_options = {
        .adapter = adapter,
        .subscriptions = subscriptions,
        .subscription_count = aws_array_list_length(topic_filters),
        .on_multi_suback = on_suback,
        .on_multi_suback_user_data = on_suback_user_data,
    };

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *operation =
        aws_mqtt5_to_mqtt3_adapter_operation_new_subscribe(adapter->allocator, &subscribe_options, adapter);
    if (operation == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, multi-topic subscribe operation creation failed, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return 0;
    }

    if (aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(&adapter->operational_state, &operation->base)) {
        goto error;
    }

    uint16_t synthetic_id = operation->base.id;

    /*
     * While in-transit to the adapter event loop, we take refs to both the operation and the adapter so that we
     * are guaranteed they are still alive when the cross-thread submission task is run.
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(&operation->base);

    aws_task_init(
        &operation->base.submission_task,
        s_adapter_subscribe_submission_fn,
        operation,
        "Mqtt5ToMqtt3AdapterSubscribeMultipleSubmission");

    aws_event_loop_schedule_task_now(adapter->loop, &operation->base.submission_task);

    return synthetic_id;

error:

    ;
    int error_code = aws_last_error();
    AWS_LOGF_ERROR(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, multi-topic subscribe failed, error code %d(%s)",
        (void *)adapter,
        error_code,
        aws_error_debug_str(error_code));

    aws_mqtt5_to_mqtt3_adapter_operation_release(&operation->base);

    return 0;
}

static void s_adapter_unsubscribe_operation_destroy(void *context) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = context;
    if (operation == NULL) {
        return;
    }

    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *unsubscribe_op = operation->impl;

    aws_byte_buf_clean_up(&unsubscribe_op->topic_filter);

    struct aws_mqtt_client_connection_5_impl *adapter_to_release = NULL;
    if (unsubscribe_op->base.holding_adapter_ref) {
        adapter_to_release = unsubscribe_op->base.adapter;
    }

    /* We're going away before our MQTT5 operation, make sure it doesn't try to call us back when it completes */
    unsubscribe_op->unsubscribe_op->completion_options.completion_callback = NULL;
    unsubscribe_op->unsubscribe_op->completion_options.completion_user_data = NULL;

    aws_mqtt5_operation_release(&unsubscribe_op->unsubscribe_op->base);

    aws_mem_release(operation->allocator, operation);

    if (adapter_to_release != NULL) {
        aws_ref_count_release(&adapter_to_release->internal_refs);
    }
}

static void s_aws_mqtt5_to_mqtt3_adapter_unsubscribe_completion_fn(
    const struct aws_mqtt5_packet_unsuback_view *unsuback,
    int error_code,
    void *complete_ctx) {
    (void)unsuback;

    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *unsubscribe_op = complete_ctx;

    if (unsubscribe_op->on_unsuback != NULL) {
        (*unsubscribe_op->on_unsuback)(
            &unsubscribe_op->base.adapter->base,
            unsubscribe_op->base.id,
            error_code,
            unsubscribe_op->on_unsuback_user_data);
    }

    aws_mqtt5_to_mqtt3_adapter_operation_table_remove_operation(
        &unsubscribe_op->base.adapter->operational_state, unsubscribe_op->base.id);
}

static void s_fail_unsubscribe(void *impl, int error_code) {
    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *unsubscribe_op = impl;

    if (unsubscribe_op->on_unsuback != NULL) {
        (*unsubscribe_op->on_unsuback)(
            &unsubscribe_op->base.adapter->base,
            unsubscribe_op->base.id,
            error_code,
            unsubscribe_op->on_unsuback_user_data);
    }
}

static struct aws_mqtt5_to_mqtt3_adapter_operation_vtable s_unsubscribe_vtable = {
    .fail_fn = s_fail_unsubscribe,
};

struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *aws_mqtt5_to_mqtt3_adapter_operation_new_unsubscribe(
    struct aws_allocator *allocator,
    const struct aws_mqtt5_to_mqtt3_adapter_unsubscribe_options *options) {

    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *unsubscribe_op =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe));

    unsubscribe_op->base.allocator = allocator;
    aws_ref_count_init(&unsubscribe_op->base.ref_count, unsubscribe_op, s_adapter_unsubscribe_operation_destroy);
    unsubscribe_op->base.impl = unsubscribe_op;
    unsubscribe_op->base.vtable = &s_unsubscribe_vtable;
    unsubscribe_op->base.type = AWS_MQTT5TO3_AOT_UNSUBSCRIBE;
    unsubscribe_op->base.adapter = options->adapter;
    unsubscribe_op->base.holding_adapter_ref = false;

    struct aws_mqtt5_packet_unsubscribe_view unsubscribe_view = {
        .topic_filters = &options->topic_filter,
        .topic_filter_count = 1,
    };

    struct aws_mqtt5_unsubscribe_completion_options unsubscribe_completion_options = {
        .completion_callback = s_aws_mqtt5_to_mqtt3_adapter_unsubscribe_completion_fn,
        .completion_user_data = unsubscribe_op,
    };

    unsubscribe_op->unsubscribe_op = aws_mqtt5_operation_unsubscribe_new(
        allocator, options->adapter->client, &unsubscribe_view, &unsubscribe_completion_options);
    if (unsubscribe_op->unsubscribe_op == NULL) {
        goto error;
    }

    unsubscribe_op->on_unsuback = options->on_unsuback;
    unsubscribe_op->on_unsuback_user_data = options->on_unsuback_user_data;

    aws_byte_buf_init_copy_from_cursor(&unsubscribe_op->topic_filter, allocator, options->topic_filter);

    return unsubscribe_op;

error:

    aws_mqtt5_to_mqtt3_adapter_operation_release(&unsubscribe_op->base);

    return NULL;
}

void s_adapter_unsubscribe_submission_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *operation = arg;

    struct aws_mqtt_client_connection_5_impl *adapter = operation->base.adapter;

    aws_mqtt_subscription_set_remove_subscription(
        adapter->subscriptions, aws_byte_cursor_from_buf(&operation->topic_filter));

    aws_mqtt5_client_submit_operation_internal(
        adapter->client, &operation->unsubscribe_op->base, status != AWS_TASK_STATUS_RUN_READY);

    /*
     * The operation has been submitted in-thread.  We can release the transient refs (operation, adapter) needed to
     * keep things alive during the handover
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_release_cross_thread_refs(&operation->base);
}

static uint16_t s_aws_mqtt_client_connection_5_unsubscribe(
    void *impl,
    const struct aws_byte_cursor *topic_filter,
    aws_mqtt_op_complete_fn *on_unsuback,
    void *on_unsuback_user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    AWS_LOGF_DEBUG(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, unsubscribe called", (void *)adapter);

    if (!aws_mqtt_is_valid_topic_filter(topic_filter)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, unsubscribe failed, invalid topic filter",
            (void *)adapter);
        aws_raise_error(AWS_ERROR_MQTT_INVALID_TOPIC);
        return 0;
    }

    struct aws_mqtt5_to_mqtt3_adapter_unsubscribe_options unsubscribe_options = {
        .adapter = adapter,
        .topic_filter = *topic_filter,
        .on_unsuback = on_unsuback,
        .on_unsuback_user_data = on_unsuback_user_data,
    };

    struct aws_mqtt5_to_mqtt3_adapter_operation_unsubscribe *operation =
        aws_mqtt5_to_mqtt3_adapter_operation_new_unsubscribe(adapter->allocator, &unsubscribe_options);
    if (operation == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, unsubscribe operation creation failed, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return 0;
    }

    if (aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(&adapter->operational_state, &operation->base)) {
        goto error;
    }

    uint16_t synthetic_id = operation->base.id;

    /*
     * While in-transit to the adapter event loop, we take refs to both the operation and the adapter so that we
     * are guaranteed they are still alive when the cross-thread submission task is run.
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(&operation->base);

    aws_task_init(
        &operation->base.submission_task,
        s_adapter_unsubscribe_submission_fn,
        operation,
        "Mqtt5ToMqtt3AdapterUnsubscribeSubmission");

    aws_event_loop_schedule_task_now(adapter->loop, &operation->base.submission_task);

    return synthetic_id;

error:

    ;
    int error_code = aws_last_error();
    AWS_LOGF_ERROR(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, unsubscribe failed, error code %d(%s)",
        (void *)adapter,
        error_code,
        aws_error_debug_str(error_code));

    aws_mqtt5_to_mqtt3_adapter_operation_release(&operation->base);

    return 0;
}

static int s_aws_mqtt_client_connection_5_reconnect(
    void *impl,
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete,
    void *userdata) {
    (void)impl;
    (void)on_connection_complete;
    (void)userdata;

    /* DEPRECATED, connection will reconnect automatically now. */
    AWS_LOGF_ERROR(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "aws_mqtt_client_connection_reconnect has been DEPRECATED.");
    return aws_raise_error(AWS_ERROR_UNSUPPORTED_OPERATION);
}

static int s_aws_mqtt_client_connection_5_get_stats(
    void *impl,
    struct aws_mqtt_connection_operation_statistics *stats) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    // Error checking
    if (!adapter) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "Invalid MQTT3-to-5 adapter used when trying to get operation statistics");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT5_TO_MQTT3_ADAPTER, "id=%p: mqtt3-to-5-adapter, get_stats invoked", (void *)adapter);

    if (!stats) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: Invalid MQTT311 statistics struct used when trying to get operation statistics",
            (void *)adapter);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_mqtt5_client_operation_statistics mqtt5_stats;
    AWS_ZERO_STRUCT(mqtt5_stats);
    aws_mqtt5_client_get_stats(adapter->client, &mqtt5_stats);

    stats->incomplete_operation_count = mqtt5_stats.incomplete_operation_count;
    stats->incomplete_operation_size = mqtt5_stats.incomplete_operation_size;
    stats->unacked_operation_count = mqtt5_stats.unacked_operation_count;
    stats->unacked_operation_size = mqtt5_stats.unacked_operation_size;

    return AWS_OP_SUCCESS;
}

static uint16_t s_aws_mqtt_5_resubscribe_existing_topics(
    void *impl,
    aws_mqtt_suback_multi_fn *on_suback,
    void *on_suback_user_data) {

    struct aws_mqtt_client_connection_5_impl *adapter = impl;

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, resubscribe_existing_topics invoked",
        (void *)adapter);

    struct aws_mqtt5_to_mqtt3_adapter_subscribe_options subscribe_options = {
        .adapter = adapter,
        .subscriptions = NULL,
        .subscription_count = 0,
        .on_multi_suback = on_suback,
        .on_multi_suback_user_data = on_suback_user_data,
    };

    struct aws_mqtt5_to_mqtt3_adapter_operation_subscribe *operation =
        aws_mqtt5_to_mqtt3_adapter_operation_new_subscribe(adapter->allocator, &subscribe_options, adapter);
    if (operation == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
            "id=%p: mqtt3-to-5-adapter, resubscribe_existing_topics failed on operation creation, error code %d(%s)",
            (void *)adapter,
            error_code,
            aws_error_debug_str(error_code));
        return 0;
    }

    if (aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(&adapter->operational_state, &operation->base)) {
        goto error;
    }

    uint16_t synthetic_id = operation->base.id;

    /*
     * While in-transit to the adapter event loop, we take refs to both the operation and the adapter so that we
     * are guaranteed they are still alive when the cross-thread submission task is run.
     */
    s_aws_mqtt5_to_mqtt3_adapter_operation_acquire_cross_thread_refs(&operation->base);

    aws_task_init(
        &operation->base.submission_task,
        s_adapter_subscribe_submission_fn,
        operation,
        "Mqtt5ToMqtt3AdapterSubscribeResubscribe");

    aws_event_loop_schedule_task_now(adapter->loop, &operation->base.submission_task);

    return synthetic_id;

error:

    ;
    int error_code = aws_last_error();
    AWS_LOGF_ERROR(
        AWS_LS_MQTT5_TO_MQTT3_ADAPTER,
        "id=%p: mqtt3-to-5-adapter, resubscribe_existing_topics failed, error code %d(%s)",
        (void *)adapter,
        error_code,
        aws_error_debug_str(error_code));

    aws_mqtt5_to_mqtt3_adapter_operation_release(&operation->base);

    return 0;
}

static enum aws_mqtt311_impl_type s_aws_mqtt_client_connection_5_get_impl(const void *impl) {
    (void)impl;

    return AWS_MQTT311_IT_5_ADAPTER;
}

static struct aws_event_loop *s_aws_mqtt_client_connection_5_get_event_loop(const void *impl) {
    const struct aws_mqtt_client_connection_5_impl *adapter = impl;

    return adapter->client->loop;
}

static struct aws_mqtt_client_connection_vtable s_aws_mqtt_client_connection_5_vtable = {
    .acquire_fn = s_aws_mqtt_client_connection_5_acquire,
    .release_fn = s_aws_mqtt_client_connection_5_release,
    .set_will_fn = s_aws_mqtt_client_connection_5_set_will,
    .set_login_fn = s_aws_mqtt_client_connection_5_set_login,
    .use_websockets_fn = s_aws_mqtt_client_connection_5_use_websockets,
    .set_http_proxy_options_fn = s_aws_mqtt_client_connection_5_set_http_proxy_options,
    .set_host_resolution_options_fn = s_aws_mqtt_client_connection_5_set_host_resolution_options,
    .set_reconnect_timeout_fn = s_aws_mqtt_client_connection_5_set_reconnect_timeout,
    .set_connection_result_handlers = s_aws_mqtt_client_connection_5_set_connection_result_handlers,
    .set_connection_interruption_handlers_fn = s_aws_mqtt_client_connection_5_set_interruption_handlers,
    .set_connection_closed_handler_fn = s_aws_mqtt_client_connection_5_set_on_closed_handler,
    .set_connection_termination_handler_fn = s_aws_mqtt_client_connection_5_set_termination_handler,
    .set_on_any_publish_handler_fn = s_aws_mqtt_client_connection_5_set_on_any_publish_handler,
    .connect_fn = s_aws_mqtt_client_connection_5_connect,
    .reconnect_fn = s_aws_mqtt_client_connection_5_reconnect,
    .disconnect_fn = s_aws_mqtt_client_connection_5_disconnect,
    .subscribe_multiple_fn = s_aws_mqtt_client_connection_5_subscribe_multiple,
    .subscribe_fn = s_aws_mqtt_client_connection_5_subscribe,
    .resubscribe_existing_topics_fn = s_aws_mqtt_5_resubscribe_existing_topics,
    .unsubscribe_fn = s_aws_mqtt_client_connection_5_unsubscribe,
    .publish_fn = s_aws_mqtt_client_connection_5_publish,
    .get_stats_fn = s_aws_mqtt_client_connection_5_get_stats,
    .get_impl_type = s_aws_mqtt_client_connection_5_get_impl,
    .get_event_loop = s_aws_mqtt_client_connection_5_get_event_loop,
};

static struct aws_mqtt_client_connection_vtable *s_aws_mqtt_client_connection_5_vtable_ptr =
    &s_aws_mqtt_client_connection_5_vtable;

struct aws_mqtt_client_connection *aws_mqtt_client_connection_new_from_mqtt5_client(struct aws_mqtt5_client *client) {
    struct aws_allocator *allocator = client->allocator;
    struct aws_mqtt_client_connection_5_impl *adapter =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_client_connection_5_impl));

    adapter->allocator = allocator;

    adapter->base.vtable = s_aws_mqtt_client_connection_5_vtable_ptr;
    adapter->base.impl = adapter;

    adapter->client = aws_mqtt5_client_acquire(client);
    adapter->loop = client->loop;
    adapter->adapter_state = AWS_MQTT_AS_STAY_DISCONNECTED;

    aws_ref_count_init(&adapter->external_refs, adapter, s_aws_mqtt5_to_mqtt3_adapter_on_zero_external_refs);
    aws_ref_count_init(&adapter->internal_refs, adapter, s_aws_mqtt5_to_mqtt3_adapter_on_zero_internal_refs);

    aws_mqtt5_to_mqtt3_adapter_operation_table_init(&adapter->operational_state, allocator);

    adapter->subscriptions = aws_mqtt_subscription_set_new(allocator);

    struct aws_mqtt5_listener_config listener_config = {
        .client = client,
        .listener_callbacks =
            {
                .listener_publish_received_handler = s_aws_mqtt5_listener_publish_received_adapter,
                .listener_publish_received_handler_user_data = adapter,
                .lifecycle_event_handler = s_aws_mqtt5_to_mqtt3_adapter_lifecycle_handler,
                .lifecycle_event_handler_user_data = adapter,
            },
        .termination_callback = s_aws_mqtt5_to_mqtt3_adapter_on_listener_detached,
        .termination_callback_user_data = adapter,
    };
    adapter->listener = aws_mqtt5_listener_new(allocator, &listener_config);

    return &adapter->base;
}

#define DEFAULT_MQTT_ADAPTER_OPERATION_TABLE_SIZE 100

void aws_mqtt5_to_mqtt3_adapter_operation_table_init(
    struct aws_mqtt5_to_mqtt3_adapter_operation_table *table,
    struct aws_allocator *allocator) {
    aws_mutex_init(&table->lock);
    aws_hash_table_init(
        &table->operations,
        allocator,
        DEFAULT_MQTT_ADAPTER_OPERATION_TABLE_SIZE,
        aws_mqtt_hash_uint16_t,
        aws_mqtt_compare_uint16_t_eq,
        NULL,
        NULL);
    table->next_id = 1;
}

static int s_adapter_operation_fail(void *context, struct aws_hash_element *operation_element) {
    (void)context;

    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = operation_element->value;

    (*operation->vtable->fail_fn)(operation->impl, AWS_ERROR_MQTT_CONNECTION_DESTROYED);

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_adapter_operation_clean_up(void *context, struct aws_hash_element *operation_element) {
    (void)context;

    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = operation_element->value;

    aws_mqtt5_to_mqtt3_adapter_operation_release(operation);

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

void aws_mqtt5_to_mqtt3_adapter_operation_table_clean_up(struct aws_mqtt5_to_mqtt3_adapter_operation_table *table) {
    aws_hash_table_foreach(&table->operations, s_adapter_operation_fail, table);
    aws_hash_table_foreach(&table->operations, s_adapter_operation_clean_up, table);

    aws_hash_table_clean_up(&table->operations);

    aws_mutex_clean_up(&table->lock);
}

static uint16_t s_next_adapter_id(uint16_t current_id) {
    if (++current_id == 0) {
        current_id = 1;
    }

    return current_id;
}

int aws_mqtt5_to_mqtt3_adapter_operation_table_add_operation(
    struct aws_mqtt5_to_mqtt3_adapter_operation_table *table,
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation) {

    operation->id = 0;

    aws_mutex_lock(&table->lock);

    uint16_t current_id = table->next_id;
    struct aws_hash_element *elem = NULL;
    for (uint16_t i = 0; i < UINT16_MAX; ++i) {
        aws_hash_table_find(&table->operations, &current_id, &elem);

        if (elem == NULL) {
            operation->id = current_id;
            table->next_id = s_next_adapter_id(current_id);

            if (aws_hash_table_put(&table->operations, &operation->id, operation, NULL)) {
                operation->id = 0;
            }

            goto done;
        }

        current_id = s_next_adapter_id(current_id);
    }

done:

    aws_mutex_unlock(&table->lock);

    return (operation->id != 0) ? AWS_OP_SUCCESS : aws_raise_error(AWS_ERROR_MQTT_QUEUE_FULL);
}

void aws_mqtt5_to_mqtt3_adapter_operation_table_remove_operation(
    struct aws_mqtt5_to_mqtt3_adapter_operation_table *table,
    uint16_t operation_id) {
    struct aws_hash_element existing_element;
    AWS_ZERO_STRUCT(existing_element);

    aws_mutex_lock(&table->lock);
    aws_hash_table_remove(&table->operations, &operation_id, &existing_element, NULL);
    aws_mutex_unlock(&table->lock);

    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation = existing_element.value;
    if (operation != NULL) {
        aws_mqtt5_to_mqtt3_adapter_operation_release(operation);
    }
}

struct aws_mqtt5_to_mqtt3_adapter_operation_base *aws_mqtt5_to_mqtt3_adapter_operation_release(
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation) {
    if (operation != NULL) {
        aws_ref_count_release(&operation->ref_count);
    }

    return NULL;
}

struct aws_mqtt5_to_mqtt3_adapter_operation_base *aws_mqtt5_to_mqtt3_adapter_operation_acquire(
    struct aws_mqtt5_to_mqtt3_adapter_operation_base *operation) {
    if (operation != NULL) {
        aws_ref_count_acquire(&operation->ref_count);
    }

    return operation;
}
