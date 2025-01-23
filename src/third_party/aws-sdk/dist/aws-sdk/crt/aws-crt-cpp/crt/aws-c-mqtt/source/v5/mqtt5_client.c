/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/v5/mqtt5_client.h>

#include <aws/common/clock.h>
#include <aws/common/string.h>
#include <aws/http/proxy.h>
#include <aws/http/request_response.h>
#include <aws/http/websocket.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/shared.h>
#include <aws/mqtt/private/v5/mqtt5_client_impl.h>
#include <aws/mqtt/private/v5/mqtt5_options_storage.h>
#include <aws/mqtt/private/v5/mqtt5_utils.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4232) /* function pointer to dll symbol */
#endif

#define AWS_MQTT5_IO_MESSAGE_DEFAULT_LENGTH 4096
#define AWS_MQTT5_DEFAULT_CONNACK_PACKET_TIMEOUT_MS 10000
#define DEFAULT_MQTT5_OPERATION_TABLE_SIZE 200

const char *aws_mqtt5_client_state_to_c_string(enum aws_mqtt5_client_state state) {
    switch (state) {
        case AWS_MCS_STOPPED:
            return "STOPPED";

        case AWS_MCS_CONNECTING:
            return "CONNECTING";

        case AWS_MCS_MQTT_CONNECT:
            return "MQTT_CONNECT";

        case AWS_MCS_CONNECTED:
            return "CONNECTED";

        case AWS_MCS_CLEAN_DISCONNECT:
            return "CLEAN_DISCONNECT";

        case AWS_MCS_CHANNEL_SHUTDOWN:
            return "CHANNEL_SHUTDOWN";

        case AWS_MCS_PENDING_RECONNECT:
            return "PENDING_RECONNECT";

        case AWS_MCS_TERMINATED:
            return "TERMINATED";

        default:
            return "UNKNOWN";
    }
}

static bool s_aws_mqtt5_operation_is_retainable(struct aws_mqtt5_operation *operation) {
    switch (operation->packet_type) {
        case AWS_MQTT5_PT_PUBLISH:
        case AWS_MQTT5_PT_SUBSCRIBE:
        case AWS_MQTT5_PT_UNSUBSCRIBE:
            return true;

        default:
            return false;
    }
}

static void s_init_statistics(struct aws_mqtt5_client_operation_statistics_impl *stats) {
    aws_atomic_store_int(&stats->incomplete_operation_count_atomic, 0);
    aws_atomic_store_int(&stats->incomplete_operation_size_atomic, 0);
    aws_atomic_store_int(&stats->unacked_operation_count_atomic, 0);
    aws_atomic_store_int(&stats->unacked_operation_size_atomic, 0);
}

static bool s_aws_mqtt5_operation_satisfies_offline_queue_retention_policy(
    struct aws_mqtt5_operation *operation,
    enum aws_mqtt5_client_operation_queue_behavior_type queue_behavior) {
    switch (aws_mqtt5_client_operation_queue_behavior_type_to_non_default(queue_behavior)) {
        case AWS_MQTT5_COQBT_FAIL_ALL_ON_DISCONNECT:
            return false;

        case AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT:
            if (!s_aws_mqtt5_operation_is_retainable(operation)) {
                return false;
            }

            if (operation->packet_type == AWS_MQTT5_PT_PUBLISH) {
                const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
                if (publish_view->qos == AWS_MQTT5_QOS_AT_MOST_ONCE) {
                    return false;
                }
            }

            return true;

        case AWS_MQTT5_COQBT_FAIL_NON_QOS1_PUBLISH_ON_DISCONNECT:
            if (!s_aws_mqtt5_operation_is_retainable(operation)) {
                return false;
            }

            if (operation->packet_type == AWS_MQTT5_PT_PUBLISH) {
                const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
                if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {
                    return true;
                }
            }

            return false;

        default:
            return false;
    }
}

typedef bool(mqtt5_operation_filter)(struct aws_mqtt5_operation *operation, void *filter_context);

static void s_filter_operation_list(
    struct aws_linked_list *source_operations,
    mqtt5_operation_filter *filter_fn,
    struct aws_linked_list *filtered_operations,
    void *filter_context) {
    struct aws_linked_list_node *node = aws_linked_list_begin(source_operations);
    while (node != aws_linked_list_end(source_operations)) {
        struct aws_mqtt5_operation *operation = AWS_CONTAINER_OF(node, struct aws_mqtt5_operation, node);
        node = aws_linked_list_next(node);

        if (filter_fn(operation, filter_context)) {
            aws_linked_list_remove(&operation->node);
            aws_linked_list_push_back(filtered_operations, &operation->node);
        }
    }
}

typedef void(mqtt5_operation_applicator)(struct aws_mqtt5_operation *operation, void *applicator_context);

static void s_apply_to_operation_list(
    struct aws_linked_list *operations,
    mqtt5_operation_applicator *applicator_fn,
    void *applicator_context) {
    struct aws_linked_list_node *node = aws_linked_list_begin(operations);
    while (node != aws_linked_list_end(operations)) {
        struct aws_mqtt5_operation *operation = AWS_CONTAINER_OF(node, struct aws_mqtt5_operation, node);
        node = aws_linked_list_next(node);

        applicator_fn(operation, applicator_context);
    }
}

static int s_aws_mqtt5_client_change_desired_state(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_client_state desired_state,
    struct aws_mqtt5_operation_disconnect *disconnect_operation);

static uint64_t s_aws_mqtt5_client_compute_operational_state_service_time(
    const struct aws_mqtt5_client_operational_state *client_operational_state,
    uint64_t now);

static int s_submit_operation(struct aws_mqtt5_client *client, struct aws_mqtt5_operation *operation);

static void s_complete_operation(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    int error_code,
    enum aws_mqtt5_packet_type packet_type,
    const void *view) {
    if (client != NULL) {
        aws_mqtt5_client_statistics_change_operation_statistic_state(client, operation, AWS_MQTT5_OSS_NONE);
        if (aws_priority_queue_node_is_in_queue(&operation->priority_queue_node)) {
            struct aws_mqtt5_operation *queued_operation = NULL;
            aws_priority_queue_remove(
                &client->operational_state.operations_by_ack_timeout,
                &queued_operation,
                &operation->priority_queue_node);
        }
    }

    aws_mqtt5_operation_complete(operation, error_code, packet_type, view);
    aws_mqtt5_operation_release(operation);
}

static void s_complete_operation_list(
    struct aws_mqtt5_client *client,
    struct aws_linked_list *operation_list,
    int error_code) {

    struct aws_linked_list_node *node = aws_linked_list_begin(operation_list);
    while (node != aws_linked_list_end(operation_list)) {
        struct aws_mqtt5_operation *operation = AWS_CONTAINER_OF(node, struct aws_mqtt5_operation, node);

        node = aws_linked_list_next(node);

        s_complete_operation(client, operation, error_code, AWS_MQTT5_PT_NONE, NULL);
    }

    /* we've released everything, so reset the list to empty */
    aws_linked_list_init(operation_list);
}

static void s_check_timeouts(struct aws_mqtt5_client *client, uint64_t now) {
    struct aws_priority_queue *timeout_queue = &client->operational_state.operations_by_ack_timeout;

    bool done = aws_priority_queue_size(timeout_queue) == 0;
    while (!done) {
        struct aws_mqtt5_operation **next_operation_by_timeout_ptr = NULL;
        aws_priority_queue_top(timeout_queue, (void **)&next_operation_by_timeout_ptr);
        AWS_FATAL_ASSERT(next_operation_by_timeout_ptr != NULL);
        struct aws_mqtt5_operation *next_operation_by_timeout = *next_operation_by_timeout_ptr;
        AWS_FATAL_ASSERT(next_operation_by_timeout != NULL);

        // If the top of the heap hasn't timed out than nothing has
        if (next_operation_by_timeout->ack_timeout_timepoint_ns > now) {
            break;
        }

        /* Ack timeout for this operation has been reached */
        aws_priority_queue_pop(timeout_queue, &next_operation_by_timeout);

        aws_mqtt5_packet_id_t packet_id = aws_mqtt5_operation_get_packet_id(next_operation_by_timeout);
        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: %s packet with id:%d has timed out",
            (void *)client,
            aws_mqtt5_packet_type_to_c_string(next_operation_by_timeout->packet_type),
            (int)packet_id);

        struct aws_hash_element *elem = NULL;
        aws_hash_table_find(&client->operational_state.unacked_operations_table, &packet_id, &elem);

        if (elem == NULL || elem->value == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT, "id=%p: timeout for unknown operation with id %d", (void *)client, (int)packet_id);
            return;
        }

        aws_linked_list_remove(&next_operation_by_timeout->node);
        aws_hash_table_remove(&client->operational_state.unacked_operations_table, &packet_id, NULL, NULL);

        s_complete_operation(client, next_operation_by_timeout, AWS_ERROR_MQTT_TIMEOUT, AWS_MQTT5_PT_NONE, NULL);

        done = aws_priority_queue_size(timeout_queue) == 0;
    }
}

static void s_mqtt5_client_final_destroy(struct aws_mqtt5_client *client) {
    if (client == NULL) {
        return;
    }

    aws_mqtt5_client_termination_completion_fn *client_termination_handler = NULL;
    void *client_termination_handler_user_data = NULL;
    if (client->config != NULL) {
        client_termination_handler = client->config->client_termination_handler;
        client_termination_handler_user_data = client->config->client_termination_handler_user_data;
    }

    aws_mqtt5_callback_set_manager_clean_up(&client->callback_manager);

    aws_mqtt5_client_operational_state_clean_up(&client->operational_state);

    aws_mqtt5_client_options_storage_destroy((struct aws_mqtt5_client_options_storage *)client->config);

    aws_mqtt5_negotiated_settings_clean_up(&client->negotiated_settings);

    aws_http_message_release(client->handshake);

    aws_mqtt5_encoder_clean_up(&client->encoder);
    aws_mqtt5_decoder_clean_up(&client->decoder);

    aws_mqtt5_inbound_topic_alias_resolver_clean_up(&client->inbound_topic_alias_resolver);
    aws_mqtt5_outbound_topic_alias_resolver_destroy(client->outbound_topic_alias_resolver);

    aws_mem_release(client->allocator, client);

    if (client_termination_handler != NULL) {
        (*client_termination_handler)(client_termination_handler_user_data);
    }
}

static void s_on_mqtt5_client_zero_ref_count(void *user_data) {
    struct aws_mqtt5_client *client = user_data;

    s_aws_mqtt5_client_change_desired_state(client, AWS_MCS_TERMINATED, NULL);
}

static void s_aws_mqtt5_client_emit_stopped_lifecycle_event(struct aws_mqtt5_client *client) {
    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: emitting stopped lifecycle event", (void *)client);

    struct aws_mqtt5_client_lifecycle_event event;
    AWS_ZERO_STRUCT(event);

    event.event_type = AWS_MQTT5_CLET_STOPPED;
    event.client = client;

    aws_mqtt5_callback_set_manager_on_lifecycle_event(&client->callback_manager, &event);
}

static void s_aws_mqtt5_client_emit_connecting_lifecycle_event(struct aws_mqtt5_client *client) {
    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: emitting connecting lifecycle event", (void *)client);

    client->lifecycle_state = AWS_MQTT5_LS_CONNECTING;

    struct aws_mqtt5_client_lifecycle_event event;
    AWS_ZERO_STRUCT(event);

    event.event_type = AWS_MQTT5_CLET_ATTEMPTING_CONNECT;
    event.client = client;

    aws_mqtt5_callback_set_manager_on_lifecycle_event(&client->callback_manager, &event);
}

static void s_aws_mqtt5_client_emit_connection_success_lifecycle_event(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_connack_view *connack_view) {

    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: emitting connection success lifecycle event", (void *)client);

    client->lifecycle_state = AWS_MQTT5_LS_CONNECTED;

    struct aws_mqtt5_client_lifecycle_event event;
    AWS_ZERO_STRUCT(event);

    event.event_type = AWS_MQTT5_CLET_CONNECTION_SUCCESS;
    event.client = client;
    event.settings = &client->negotiated_settings;
    event.connack_data = connack_view;

    aws_mqtt5_callback_set_manager_on_lifecycle_event(&client->callback_manager, &event);
}

/*
 * Emits either a CONNECTION_FAILED or DISCONNECT event based on the current life cycle state.  Once a "final"
 * event is emitted by the client, it must attempt to reconnect before another one will be emitted, since the
 * lifecycle state check will early out until then.  It is expected that this function may get called unnecessarily
 * often during various channel shutdown or disconnection/failure flows.  This will not affect overall correctness.
 */
static void s_aws_mqtt5_client_emit_final_lifecycle_event(
    struct aws_mqtt5_client *client,
    int error_code,
    const struct aws_mqtt5_packet_connack_view *connack_view,
    const struct aws_mqtt5_packet_disconnect_view *disconnect_view) {

    if (client->lifecycle_state == AWS_MQTT5_LS_NONE) {
        /* we already emitted a final event earlier */
        return;
    }

    struct aws_mqtt5_client_lifecycle_event event;
    AWS_ZERO_STRUCT(event);

    if (client->lifecycle_state == AWS_MQTT5_LS_CONNECTING) {
        AWS_FATAL_ASSERT(disconnect_view == NULL);
        event.event_type = AWS_MQTT5_CLET_CONNECTION_FAILURE;

        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: emitting connection failure lifecycle event with error code %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));
    } else {
        AWS_FATAL_ASSERT(client->lifecycle_state == AWS_MQTT5_LS_CONNECTED);
        AWS_FATAL_ASSERT(connack_view == NULL);
        event.event_type = AWS_MQTT5_CLET_DISCONNECTION;

        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: emitting disconnection lifecycle event with error code %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));
    }

    event.error_code = error_code;
    event.connack_data = connack_view;
    event.disconnect_data = disconnect_view;

    client->lifecycle_state = AWS_MQTT5_LS_NONE;

    aws_mqtt5_callback_set_manager_on_lifecycle_event(&client->callback_manager, &event);
}

/*
 * next_service_time == 0 means to not service the client, i.e. a state that only cares about external events
 *
 * This includes connecting and channel shutdown.  Terminated is also included, but it's a state that only exists
 * instantaneously before final destruction.
 */
static uint64_t s_compute_next_service_time_client_stopped(struct aws_mqtt5_client *client, uint64_t now) {
    /* have we been told to connect or terminate? */
    if (client->desired_state != AWS_MCS_STOPPED) {
        return now;
    }

    return 0;
}

static uint64_t s_compute_next_service_time_client_connecting(struct aws_mqtt5_client *client, uint64_t now) {
    (void)client;
    (void)now;

    return 0;
}

static uint64_t s_compute_next_service_time_client_mqtt_connect(struct aws_mqtt5_client *client, uint64_t now) {
    /* This state is interruptable by a stop/terminate */
    if (client->desired_state != AWS_MCS_CONNECTED) {
        return now;
    }

    uint64_t operation_processing_time =
        s_aws_mqtt5_client_compute_operational_state_service_time(&client->operational_state, now);
    if (operation_processing_time == 0) {
        return client->next_mqtt_connect_packet_timeout_time;
    }

    return aws_min_u64(client->next_mqtt_connect_packet_timeout_time, operation_processing_time);
}

/*
 * Returns the minimum of two numbers, ignoring zero.  Zero is returned only if both are zero.  Useful when we're
 * computing (next service) timepoints and zero means "no timepoint"
 */
static uint64_t s_min_non_zero_u64(uint64_t a, uint64_t b) {
    if (a == 0) {
        return b;
    }

    if (b == 0) {
        return a;
    }

    return aws_min_u64(a, b);
}

/*
 * If there are unacked operations, returns the earliest point in time that one could timeout.
 */
static uint64_t s_get_unacked_operation_timeout_for_next_service_time(struct aws_mqtt5_client *client) {
    if (aws_priority_queue_size(&client->operational_state.operations_by_ack_timeout) > 0) {
        struct aws_mqtt5_operation **operation = NULL;
        aws_priority_queue_top(&client->operational_state.operations_by_ack_timeout, (void **)&operation);
        return (*operation)->ack_timeout_timepoint_ns;
    }

    return 0;
}

static uint64_t s_compute_next_service_time_client_connected(struct aws_mqtt5_client *client, uint64_t now) {

    /* ping and ping timeout */
    uint64_t next_service_time = client->next_ping_time;
    if (client->next_ping_timeout_time != 0) {
        next_service_time = aws_min_u64(next_service_time, client->next_ping_timeout_time);
    }

    next_service_time =
        s_min_non_zero_u64(next_service_time, s_get_unacked_operation_timeout_for_next_service_time(client));

    if (client->desired_state != AWS_MCS_CONNECTED) {
        next_service_time = now;
    }

    uint64_t operation_processing_time =
        s_aws_mqtt5_client_compute_operational_state_service_time(&client->operational_state, now);

    next_service_time = s_min_non_zero_u64(operation_processing_time, next_service_time);

    /* reset reconnect delay interval */
    next_service_time = s_min_non_zero_u64(client->next_reconnect_delay_reset_time_ns, next_service_time);

    return next_service_time;
}

static uint64_t s_compute_next_service_time_client_clean_disconnect(struct aws_mqtt5_client *client, uint64_t now) {
    uint64_t ack_timeout_time = s_get_unacked_operation_timeout_for_next_service_time(client);

    uint64_t operation_processing_time =
        s_aws_mqtt5_client_compute_operational_state_service_time(&client->operational_state, now);

    return s_min_non_zero_u64(ack_timeout_time, operation_processing_time);
}

static uint64_t s_compute_next_service_time_client_channel_shutdown(struct aws_mqtt5_client *client, uint64_t now) {
    (void)client;
    (void)now;

    return 0;
}

static uint64_t s_compute_next_service_time_client_pending_reconnect(struct aws_mqtt5_client *client, uint64_t now) {
    if (client->desired_state != AWS_MCS_CONNECTED) {
        return now;
    }

    return client->next_reconnect_time_ns;
}

static uint64_t s_compute_next_service_time_client_terminated(struct aws_mqtt5_client *client, uint64_t now) {
    (void)client;
    (void)now;

    return 0;
}

static uint64_t s_compute_next_service_time_by_current_state(struct aws_mqtt5_client *client, uint64_t now) {
    switch (client->current_state) {
        case AWS_MCS_STOPPED:
            return s_compute_next_service_time_client_stopped(client, now);
        case AWS_MCS_CONNECTING:
            return s_compute_next_service_time_client_connecting(client, now);
        case AWS_MCS_MQTT_CONNECT:
            return s_compute_next_service_time_client_mqtt_connect(client, now);
        case AWS_MCS_CONNECTED:
            return s_compute_next_service_time_client_connected(client, now);
        case AWS_MCS_CLEAN_DISCONNECT:
            return s_compute_next_service_time_client_clean_disconnect(client, now);
        case AWS_MCS_CHANNEL_SHUTDOWN:
            return s_compute_next_service_time_client_channel_shutdown(client, now);
        case AWS_MCS_PENDING_RECONNECT:
            return s_compute_next_service_time_client_pending_reconnect(client, now);
        case AWS_MCS_TERMINATED:
            return s_compute_next_service_time_client_terminated(client, now);
    }

    return 0;
}

static void s_reevaluate_service_task(struct aws_mqtt5_client *client) {
    /*
     * This causes the client to only reevaluate service schedule time at the end of the service call or in
     * a callback from an external event.
     */
    if (client->in_service) {
        return;
    }

    uint64_t now = (*client->vtable->get_current_time_fn)();
    uint64_t next_service_time = s_compute_next_service_time_by_current_state(client, now);

    /*
     * This catches both the case when there's an existing service schedule and we either want to not
     * perform it (next_service_time == 0) or need to run service at a different time than the current scheduled time.
     */
    if (next_service_time != client->next_service_task_run_time && client->next_service_task_run_time > 0) {
        aws_event_loop_cancel_task(client->loop, &client->service_task);
        client->next_service_task_run_time = 0;

        AWS_LOGF_TRACE(AWS_LS_MQTT5_CLIENT, "id=%p: cancelling previously scheduled service task", (void *)client);
    }

    if (next_service_time > 0 &&
        (next_service_time < client->next_service_task_run_time || client->next_service_task_run_time == 0)) {
        aws_event_loop_schedule_task_future(client->loop, &client->service_task, next_service_time);

        AWS_LOGF_TRACE(
            AWS_LS_MQTT5_CLIENT, "id=%p: scheduled service task for time %" PRIu64, (void *)client, next_service_time);
    }

    client->next_service_task_run_time = next_service_time;
}

static void s_enqueue_operation_back(struct aws_mqtt5_client *client, struct aws_mqtt5_operation *operation) {
    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: enqueuing %s operation to back",
        (void *)client,
        aws_mqtt5_packet_type_to_c_string(operation->packet_type));

    aws_linked_list_push_back(&client->operational_state.queued_operations, &operation->node);

    s_reevaluate_service_task(client);
}

static void s_enqueue_operation_front(struct aws_mqtt5_client *client, struct aws_mqtt5_operation *operation) {
    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: enqueuing %s operation to front",
        (void *)client,
        aws_mqtt5_packet_type_to_c_string(operation->packet_type));

    aws_linked_list_push_front(&client->operational_state.queued_operations, &operation->node);

    s_reevaluate_service_task(client);
}

static void s_aws_mqtt5_client_operational_state_reset(
    struct aws_mqtt5_client_operational_state *client_operational_state,
    int completion_error_code,
    bool is_final) {

    struct aws_mqtt5_client *client = client_operational_state->client;

    s_complete_operation_list(client, &client_operational_state->queued_operations, completion_error_code);
    s_complete_operation_list(client, &client_operational_state->write_completion_operations, completion_error_code);
    s_complete_operation_list(client, &client_operational_state->unacked_operations, completion_error_code);

    if (is_final) {
        aws_priority_queue_clean_up(&client_operational_state->operations_by_ack_timeout);
        aws_hash_table_clean_up(&client_operational_state->unacked_operations_table);
    } else {
        aws_priority_queue_clear(&client->operational_state.operations_by_ack_timeout);
        aws_hash_table_clear(&client_operational_state->unacked_operations_table);
    }
}

static void s_change_current_state(struct aws_mqtt5_client *client, enum aws_mqtt5_client_state next_state);

static void s_change_current_state_to_stopped(struct aws_mqtt5_client *client) {
    client->current_state = AWS_MCS_STOPPED;

    s_aws_mqtt5_client_operational_state_reset(&client->operational_state, AWS_ERROR_MQTT5_USER_REQUESTED_STOP, false);

    /* Stop works as a complete session wipe, and so the next time we connect, we want it to be clean */
    client->has_connected_successfully = false;

    s_aws_mqtt5_client_emit_stopped_lifecycle_event(client);
}

static void s_aws_mqtt5_client_shutdown_channel(struct aws_mqtt5_client *client, int error_code) {
    if (error_code == AWS_ERROR_SUCCESS) {
        error_code = AWS_ERROR_UNKNOWN;
    }

    s_aws_mqtt5_client_emit_final_lifecycle_event(client, error_code, NULL, NULL);

    if (client->current_state != AWS_MCS_MQTT_CONNECT && client->current_state != AWS_MCS_CONNECTED &&
        client->current_state != AWS_MCS_CLEAN_DISCONNECT) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: client channel shutdown invoked from unexpected state %d(%s)",
            (void *)client,
            (int)client->current_state,
            aws_mqtt5_client_state_to_c_string(client->current_state));
        return;
    }

    if (client->slot == NULL || client->slot->channel == NULL) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: client channel shutdown invoked without a channel", (void *)client);
        return;
    }

    s_change_current_state(client, AWS_MCS_CHANNEL_SHUTDOWN);
    (*client->vtable->channel_shutdown_fn)(client->slot->channel, error_code);
}

static void s_aws_mqtt5_client_shutdown_channel_with_disconnect(
    struct aws_mqtt5_client *client,
    int error_code,
    struct aws_mqtt5_operation_disconnect *disconnect_op) {
    if (client->current_state != AWS_MCS_CONNECTED && client->current_state != AWS_MCS_MQTT_CONNECT) {
        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    aws_linked_list_push_front(&client->operational_state.queued_operations, &disconnect_op->base.node);
    aws_mqtt5_operation_disconnect_acquire(disconnect_op);
    client->clean_disconnect_error_code = error_code;

    s_change_current_state(client, AWS_MCS_CLEAN_DISCONNECT);
}

static void s_on_disconnect_operation_complete(int error_code, void *user_data) {
    struct aws_mqtt5_client *client = user_data;

    s_aws_mqtt5_client_shutdown_channel(
        client, (error_code != AWS_ERROR_SUCCESS) ? error_code : client->clean_disconnect_error_code);
}

static void s_aws_mqtt5_client_shutdown_channel_clean(
    struct aws_mqtt5_client *client,
    int error_code,
    enum aws_mqtt5_disconnect_reason_code reason_code) {
    struct aws_mqtt5_packet_disconnect_view disconnect_options = {
        .reason_code = reason_code,
    };

    struct aws_mqtt5_disconnect_completion_options internal_completion_options = {
        .completion_callback = s_on_disconnect_operation_complete,
        .completion_user_data = client,
    };

    struct aws_mqtt5_operation_disconnect *disconnect_op =
        aws_mqtt5_operation_disconnect_new(client->allocator, &disconnect_options, NULL, &internal_completion_options);
    if (disconnect_op == NULL) {
        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    s_aws_mqtt5_client_shutdown_channel_with_disconnect(client, error_code, disconnect_op);
    aws_mqtt5_operation_disconnect_release(disconnect_op);
}

static void s_mqtt5_client_shutdown_final(int error_code, struct aws_mqtt5_client *client) {

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(client->loop));

    s_aws_mqtt5_client_emit_final_lifecycle_event(client, error_code, NULL, NULL);

    AWS_LOGF_INFO(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: channel tore down with error code %d(%s)",
        (void *)client,
        error_code,
        aws_error_debug_str(error_code));

    if (client->slot) {
        aws_channel_slot_remove(client->slot);
        AWS_LOGF_TRACE(AWS_LS_MQTT5_CLIENT, "id=%p: slot removed successfully", (void *)client);
        client->slot = NULL;
    }

    aws_mqtt5_client_on_disconnection_update_operational_state(client);

    if (client->desired_state == AWS_MCS_CONNECTED) {
        s_change_current_state(client, AWS_MCS_PENDING_RECONNECT);
    } else {
        s_change_current_state(client, AWS_MCS_STOPPED);
    }
}

static void s_mqtt5_client_shutdown(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)channel;

    struct aws_mqtt5_client *client = user_data;

    if (error_code == AWS_ERROR_SUCCESS) {
        error_code = AWS_ERROR_MQTT_UNEXPECTED_HANGUP;
    }

    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(client->loop));
    s_mqtt5_client_shutdown_final(error_code, client);
}

static void s_mqtt5_client_setup(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;

    /* Setup callback contract is: if error_code is non-zero then channel is NULL. */
    AWS_FATAL_ASSERT((error_code != 0) == (channel == NULL));
    struct aws_mqtt5_client *client = user_data;

    if (error_code != AWS_OP_SUCCESS) {
        /* client shutdown already handles this case, so just call that. */
        s_mqtt5_client_shutdown(bootstrap, error_code, channel, user_data);
        return;
    }

    AWS_FATAL_ASSERT(client->current_state == AWS_MCS_CONNECTING);
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(client->loop));

    if (client->desired_state != AWS_MCS_CONNECTED) {
        aws_raise_error(AWS_ERROR_MQTT5_USER_REQUESTED_STOP);
        goto error;
    }

    client->slot = aws_channel_slot_new(channel); /* allocs or crashes */

    if (aws_channel_slot_insert_end(channel, client->slot)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: Failed to insert slot into channel %p, error %d (%s).",
            (void *)client,
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    if (aws_channel_slot_set_handler(client->slot, &client->handler)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: Failed to set MQTT handler into slot on channel %p, error %d (%s).",
            (void *)client,
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    s_change_current_state(client, AWS_MCS_MQTT_CONNECT);

    return;

error:

    s_change_current_state(client, AWS_MCS_CHANNEL_SHUTDOWN);
    (*client->vtable->channel_shutdown_fn)(channel, aws_last_error());
}

static void s_on_websocket_shutdown(struct aws_websocket *websocket, int error_code, void *user_data) {
    struct aws_mqtt5_client *client = user_data;

    struct aws_channel *channel = client->slot ? client->slot->channel : NULL;

    s_mqtt5_client_shutdown(client->config->bootstrap, error_code, channel, client);

    if (websocket) {
        aws_websocket_release(websocket);
    }
}

static void s_on_websocket_setup(const struct aws_websocket_on_connection_setup_data *setup, void *user_data) {

    struct aws_mqtt5_client *client = user_data;
    client->handshake = aws_http_message_release(client->handshake);

    /* Setup callback contract is: if error_code is non-zero then websocket is NULL. */
    AWS_FATAL_ASSERT((setup->error_code != 0) == (setup->websocket == NULL));

    struct aws_channel *channel = NULL;

    if (setup->websocket) {
        channel = aws_websocket_get_channel(setup->websocket);
        AWS_ASSERT(channel);

        /* Websocket must be "converted" before the MQTT handler can be installed next to it. */
        if (aws_websocket_convert_to_midchannel_handler(setup->websocket)) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT,
                "id=%p: Failed converting websocket, error %d (%s)",
                (void *)client,
                aws_last_error(),
                aws_error_name(aws_last_error()));

            (*client->vtable->channel_shutdown_fn)(channel, aws_last_error());
            return;
        }
    }

    /* Call into the channel-setup callback, the rest of the logic is the same. */
    s_mqtt5_client_setup(client->config->bootstrap, setup->error_code, channel, client);
}

struct aws_mqtt5_websocket_transform_complete_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt5_client *client;
    int error_code;
    struct aws_http_message *handshake;
};

void s_websocket_transform_complete_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt5_websocket_transform_complete_task *websocket_transform_complete_task = arg;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    struct aws_mqtt5_client *client = websocket_transform_complete_task->client;

    aws_http_message_release(client->handshake);
    client->handshake = aws_http_message_acquire(websocket_transform_complete_task->handshake);

    int error_code = websocket_transform_complete_task->error_code;
    if (error_code == 0 && client->desired_state == AWS_MCS_CONNECTED) {

        struct aws_websocket_client_connection_options websocket_options = {
            .allocator = client->allocator,
            .bootstrap = client->config->bootstrap,
            .socket_options = &client->config->socket_options,
            .tls_options = client->config->tls_options_ptr,
            .host = aws_byte_cursor_from_string(client->config->host_name),
            .port = client->config->port,
            .handshake_request = websocket_transform_complete_task->handshake,
            .initial_window_size = 0, /* Prevent websocket data from arriving before the MQTT handler is installed */
            .user_data = client,
            .on_connection_setup = s_on_websocket_setup,
            .on_connection_shutdown = s_on_websocket_shutdown,
            .requested_event_loop = client->loop,
            .host_resolution_config = &client->config->host_resolution_override};

        if (client->config->http_proxy_config != NULL) {
            websocket_options.proxy_options = &client->config->http_proxy_options;
        }

        if (client->vtable->websocket_connect_fn(&websocket_options)) {
            AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: Failed to initiate websocket connection.", (void *)client);
            error_code = aws_last_error();
            goto error;
        }

        goto done;

    } else {
        if (error_code == AWS_ERROR_SUCCESS) {
            AWS_ASSERT(client->desired_state != AWS_MCS_CONNECTED);
            error_code = AWS_ERROR_MQTT5_USER_REQUESTED_STOP;
        }
    }

error:;
    struct aws_websocket_on_connection_setup_data websocket_setup = {.error_code = error_code};
    s_on_websocket_setup(&websocket_setup, client);

done:

    aws_http_message_release(websocket_transform_complete_task->handshake);
    aws_mqtt5_client_release(websocket_transform_complete_task->client);

    aws_mem_release(websocket_transform_complete_task->allocator, websocket_transform_complete_task);
}

static void s_websocket_handshake_transform_complete(
    struct aws_http_message *handshake_request,
    int error_code,
    void *complete_ctx) {

    struct aws_mqtt5_client *client = complete_ctx;

    struct aws_mqtt5_websocket_transform_complete_task *task =
        aws_mem_calloc(client->allocator, 1, sizeof(struct aws_mqtt5_websocket_transform_complete_task));

    aws_task_init(
        &task->task, s_websocket_transform_complete_task_fn, (void *)task, "WebsocketHandshakeTransformComplete");

    task->allocator = client->allocator;
    task->client = aws_mqtt5_client_acquire(client);
    task->error_code = error_code;
    task->handshake = handshake_request;

    aws_event_loop_schedule_task_now(client->loop, &task->task);
}

static int s_websocket_connect(struct aws_mqtt5_client *client) {
    AWS_ASSERT(client);
    AWS_ASSERT(client->config->websocket_handshake_transform);

    /* Build websocket handshake request */
    struct aws_http_message *handshake = aws_http_message_new_websocket_handshake_request(
        client->allocator, *g_websocket_handshake_default_path, aws_byte_cursor_from_string(client->config->host_name));

    if (handshake == NULL) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: Failed to generate websocket handshake request", (void *)client);
        return AWS_OP_ERR;
    }

    if (aws_http_message_add_header(handshake, *g_websocket_handshake_default_protocol_header)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT, "id=%p: Failed to add default header to websocket handshake request", (void *)client);
        goto on_error;
    }

    AWS_LOGF_TRACE(AWS_LS_MQTT5_CLIENT, "id=%p: Transforming websocket handshake request.", (void *)client);

    /*
     * There is no need to inc the client's ref count here since this state (AWS_MCS_CONNECTING) is uninterruptible by
     * the async destruction process.  Only a completion of the chain of connection establishment callbacks can cause
     * this state to be left by the client.
     */
    client->config->websocket_handshake_transform(
        handshake,
        client->config->websocket_handshake_transform_user_data,
        s_websocket_handshake_transform_complete,
        client);

    return AWS_OP_SUCCESS;

on_error:

    aws_http_message_release(handshake);

    return AWS_OP_ERR;
}

static void s_change_current_state_to_connecting(struct aws_mqtt5_client *client) {
    AWS_ASSERT(client->current_state == AWS_MCS_STOPPED || client->current_state == AWS_MCS_PENDING_RECONNECT);

    client->current_state = AWS_MCS_CONNECTING;
    client->clean_disconnect_error_code = AWS_ERROR_SUCCESS;
    client->should_reset_connection = false;

    s_aws_mqtt5_client_emit_connecting_lifecycle_event(client);

    int result = 0;
    if (client->config->websocket_handshake_transform != NULL) {
        result = s_websocket_connect(client);
    } else {
        struct aws_socket_channel_bootstrap_options channel_options;
        AWS_ZERO_STRUCT(channel_options);
        channel_options.bootstrap = client->config->bootstrap;
        channel_options.host_name = aws_string_c_str(client->config->host_name);
        channel_options.port = client->config->port;
        channel_options.socket_options = &client->config->socket_options;
        channel_options.tls_options = client->config->tls_options_ptr;
        channel_options.setup_callback = &s_mqtt5_client_setup;
        channel_options.shutdown_callback = &s_mqtt5_client_shutdown;
        channel_options.user_data = client;
        channel_options.requested_event_loop = client->loop;
        channel_options.host_resolution_override_config = &client->config->host_resolution_override;

        if (client->config->http_proxy_config == NULL) {
            result = (*client->vtable->client_bootstrap_new_socket_channel_fn)(&channel_options);
        } else {
            result = (*client->vtable->http_proxy_new_socket_channel_fn)(
                &channel_options, &client->config->http_proxy_options);
        }
    }

    if (result) {
        int error_code = aws_last_error();
        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to kick off connection with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        s_aws_mqtt5_client_emit_final_lifecycle_event(client, aws_last_error(), NULL, NULL);

        s_change_current_state(client, AWS_MCS_PENDING_RECONNECT);
    }
}

static int s_aws_mqtt5_client_set_current_operation(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation) {

    if (aws_mqtt5_operation_bind_packet_id(operation, &client->operational_state)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to bind mqtt packet id for current operation, with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        return AWS_OP_ERR;
    }

    if (aws_mqtt5_encoder_append_packet_encoding(&client->encoder, operation->packet_type, operation->packet_view)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to append packet encoding sequence for current operation with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        return AWS_OP_ERR;
    }

    client->operational_state.current_operation = operation;

    return AWS_OP_SUCCESS;
}

static void s_reset_ping(struct aws_mqtt5_client *client) {
    uint64_t now = (*client->vtable->get_current_time_fn)();
    uint16_t keep_alive_seconds = client->negotiated_settings.server_keep_alive;

    uint64_t keep_alive_interval_nanos =
        aws_timestamp_convert(keep_alive_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    if (keep_alive_interval_nanos == 0) {
        client->next_ping_time = UINT64_MAX;
    } else {
        client->next_ping_time = aws_add_u64_saturating(now, keep_alive_interval_nanos);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: next PINGREQ scheduled for time %" PRIu64, (void *)client, client->next_ping_time);
}

static void s_aws_mqtt5_on_socket_write_completion_mqtt_connect(struct aws_mqtt5_client *client, int error_code) {
    if (error_code != AWS_ERROR_SUCCESS) {
        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    s_reevaluate_service_task(client);
}

static void s_aws_mqtt5_on_socket_write_completion_connected(struct aws_mqtt5_client *client, int error_code) {
    if (error_code != AWS_ERROR_SUCCESS) {
        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    s_reevaluate_service_task(client);
}

static void s_aws_mqtt5_on_socket_write_completion(
    struct aws_channel *channel,
    struct aws_io_message *message,
    int error_code,
    void *user_data) {

    (void)channel;
    (void)message;

    struct aws_mqtt5_client *client = user_data;
    client->operational_state.pending_write_completion = false;

    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: socket write completion invoked with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));
    }

    switch (client->current_state) {
        case AWS_MCS_MQTT_CONNECT:
            s_aws_mqtt5_on_socket_write_completion_mqtt_connect(client, error_code);
            break;

        case AWS_MCS_CONNECTED:
            s_aws_mqtt5_on_socket_write_completion_connected(client, error_code);
            break;

        case AWS_MCS_CLEAN_DISCONNECT:
            /* the CONNECTED callback works just fine for CLEAN_DISCONNECT */
            s_aws_mqtt5_on_socket_write_completion_connected(client, error_code);
            break;

        default:
            break;
    }

    s_complete_operation_list(client, &client->operational_state.write_completion_operations, error_code);
}

static bool s_should_resume_session(const struct aws_mqtt5_client *client) {
    enum aws_mqtt5_client_session_behavior_type session_behavior =
        aws_mqtt5_client_session_behavior_type_to_non_default(client->config->session_behavior);

    return (session_behavior == AWS_MQTT5_CSBT_REJOIN_POST_SUCCESS && client->has_connected_successfully) ||
           (session_behavior == AWS_MQTT5_CSBT_REJOIN_ALWAYS);
}

static void s_change_current_state_to_mqtt_connect(struct aws_mqtt5_client *client) {
    AWS_FATAL_ASSERT(client->current_state == AWS_MCS_CONNECTING);
    AWS_FATAL_ASSERT(client->operational_state.current_operation == NULL);

    client->current_state = AWS_MCS_MQTT_CONNECT;
    if (client->should_reset_connection) {
        s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT);
        return;
    }

    client->operational_state.pending_write_completion = false;

    aws_mqtt5_encoder_reset(&client->encoder);
    aws_mqtt5_decoder_reset(&client->decoder);

    bool resume_session = s_should_resume_session(client);
    struct aws_mqtt5_packet_connect_view connect_view = client->config->connect->storage_view;
    connect_view.clean_start = !resume_session;

    if (aws_mqtt5_inbound_topic_alias_behavior_type_to_non_default(
            client->config->topic_aliasing_options.inbound_topic_alias_behavior) == AWS_MQTT5_CITABT_ENABLED) {
        connect_view.topic_alias_maximum = &client->config->topic_aliasing_options.inbound_alias_cache_size;
    }

    aws_mqtt5_negotiated_settings_reset(&client->negotiated_settings, &connect_view);
    connect_view.client_id = aws_byte_cursor_from_buf(&client->negotiated_settings.client_id_storage);

    struct aws_mqtt5_operation_connect *connect_op = aws_mqtt5_operation_connect_new(client->allocator, &connect_view);
    if (connect_op == NULL) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to create CONNECT operation with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    s_enqueue_operation_front(client, &connect_op->base);

    uint32_t timeout_ms = client->config->connack_timeout_ms;
    if (timeout_ms == 0) {
        timeout_ms = AWS_MQTT5_DEFAULT_CONNACK_PACKET_TIMEOUT_MS;
    }

    uint64_t now = (*client->vtable->get_current_time_fn)();
    client->next_mqtt_connect_packet_timeout_time =
        now + aws_timestamp_convert(timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: setting CONNECT timeout to %" PRIu64,
        (void *)client,
        client->next_mqtt_connect_packet_timeout_time);
}

static void s_reset_reconnection_delay_time(struct aws_mqtt5_client *client) {
    uint64_t now = (*client->vtable->get_current_time_fn)();
    uint64_t reset_reconnection_delay_time_nanos = aws_timestamp_convert(
        client->config->min_connected_time_to_reset_reconnect_delay_ms,
        AWS_TIMESTAMP_MILLIS,
        AWS_TIMESTAMP_NANOS,
        NULL);
    client->next_reconnect_delay_reset_time_ns = aws_add_u64_saturating(now, reset_reconnection_delay_time_nanos);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: reconnection delay reset time set to %" PRIu64,
        (void *)client,
        client->next_reconnect_delay_reset_time_ns);
}

static void s_change_current_state_to_connected(struct aws_mqtt5_client *client) {
    AWS_FATAL_ASSERT(client->current_state == AWS_MCS_MQTT_CONNECT);

    client->current_state = AWS_MCS_CONNECTED;

    aws_mqtt5_client_on_connection_update_operational_state(client);

    client->has_connected_successfully = true;
    client->next_ping_timeout_time = 0;
    s_reset_ping(client);
    s_reset_reconnection_delay_time(client);
}

static void s_change_current_state_to_clean_disconnect(struct aws_mqtt5_client *client) {
    (void)client;
    AWS_FATAL_ASSERT(client->current_state == AWS_MCS_MQTT_CONNECT || client->current_state == AWS_MCS_CONNECTED);

    client->current_state = AWS_MCS_CLEAN_DISCONNECT;
}

static void s_change_current_state_to_channel_shutdown(struct aws_mqtt5_client *client) {
    enum aws_mqtt5_client_state current_state = client->current_state;
    AWS_FATAL_ASSERT(
        current_state == AWS_MCS_MQTT_CONNECT || current_state == AWS_MCS_CONNECTING ||
        current_state == AWS_MCS_CONNECTED || current_state == AWS_MCS_CLEAN_DISCONNECT);

    client->current_state = AWS_MCS_CHANNEL_SHUTDOWN;

    /*
     * Critical requirement: The caller must invoke the channel shutdown function themselves (with the desired error
     * code) *after* changing state.
     *
     * The caller is the only one with the error context and we want to be safe and avoid the possibility of a
     * synchronous channel shutdown (mocks) leading to a situation where we get the shutdown callback before we've
     * transitioned into the CHANNEL_SHUTDOWN state.
     *
     * We could relax this if a synchronous channel shutdown is literally impossible even with mocked channels.
     */
}

/* TODO: refactor and reunify with internals of retry strategy to expose these as usable functions in aws-c-io */

static uint64_t s_aws_mqtt5_compute_reconnect_backoff_no_jitter(struct aws_mqtt5_client *client) {
    uint64_t retry_count = aws_min_u64(client->reconnect_count, 63);
    return aws_mul_u64_saturating((uint64_t)1 << retry_count, client->config->min_reconnect_delay_ms);
}

static uint64_t s_aws_mqtt5_compute_reconnect_backoff_full_jitter(struct aws_mqtt5_client *client) {
    uint64_t non_jittered = s_aws_mqtt5_compute_reconnect_backoff_no_jitter(client);
    return aws_mqtt5_client_random_in_range(0, non_jittered);
}

static uint64_t s_compute_deccorelated_jitter(struct aws_mqtt5_client *client) {
    uint64_t last_backoff_val = client->current_reconnect_delay_ms;

    if (!last_backoff_val) {
        return s_aws_mqtt5_compute_reconnect_backoff_full_jitter(client);
    }

    return aws_mqtt5_client_random_in_range(
        client->config->min_reconnect_delay_ms, aws_mul_u64_saturating(last_backoff_val, 3));
}

static void s_update_reconnect_delay_for_pending_reconnect(struct aws_mqtt5_client *client) {
    uint64_t delay_ms = 0;

    switch (client->config->retry_jitter_mode) {
        case AWS_EXPONENTIAL_BACKOFF_JITTER_DECORRELATED:
            delay_ms = s_compute_deccorelated_jitter(client);
            break;

        case AWS_EXPONENTIAL_BACKOFF_JITTER_NONE:
            delay_ms = s_aws_mqtt5_compute_reconnect_backoff_no_jitter(client);
            break;

        case AWS_EXPONENTIAL_BACKOFF_JITTER_FULL:
        case AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT:
        default:
            delay_ms = s_aws_mqtt5_compute_reconnect_backoff_full_jitter(client);
            break;
    }

    delay_ms = aws_min_u64(delay_ms, client->config->max_reconnect_delay_ms);
    uint64_t now = (*client->vtable->get_current_time_fn)();

    client->next_reconnect_time_ns =
        aws_add_u64_saturating(now, aws_timestamp_convert(delay_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL));

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: next connection attempt in %" PRIu64 " milliseconds", (void *)client, delay_ms);

    client->reconnect_count++;
}

static void s_change_current_state_to_pending_reconnect(struct aws_mqtt5_client *client) {
    client->current_state = AWS_MCS_PENDING_RECONNECT;

    s_update_reconnect_delay_for_pending_reconnect(client);
}

static void s_change_current_state_to_terminated(struct aws_mqtt5_client *client) {
    client->current_state = AWS_MCS_TERMINATED;

    s_mqtt5_client_final_destroy(client);
}

static void s_change_current_state(struct aws_mqtt5_client *client, enum aws_mqtt5_client_state next_state) {
    AWS_ASSERT(next_state != client->current_state);
    if (next_state == client->current_state) {
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: switching current state from %s to %s",
        (void *)client,
        aws_mqtt5_client_state_to_c_string(client->current_state),
        aws_mqtt5_client_state_to_c_string(next_state));

    if (client->vtable->on_client_state_change_callback_fn != NULL) {
        (*client->vtable->on_client_state_change_callback_fn)(
            client, client->current_state, next_state, client->vtable->vtable_user_data);
    }

    switch (next_state) {
        case AWS_MCS_STOPPED:
            s_change_current_state_to_stopped(client);
            break;
        case AWS_MCS_CONNECTING:
            s_change_current_state_to_connecting(client);
            break;
        case AWS_MCS_MQTT_CONNECT:
            s_change_current_state_to_mqtt_connect(client);
            break;
        case AWS_MCS_CONNECTED:
            s_change_current_state_to_connected(client);
            break;
        case AWS_MCS_CLEAN_DISCONNECT:
            s_change_current_state_to_clean_disconnect(client);
            break;
        case AWS_MCS_CHANNEL_SHUTDOWN:
            s_change_current_state_to_channel_shutdown(client);
            break;
        case AWS_MCS_PENDING_RECONNECT:
            s_change_current_state_to_pending_reconnect(client);
            break;
        case AWS_MCS_TERMINATED:
            s_change_current_state_to_terminated(client);
            return;
    }

    s_reevaluate_service_task(client);
}

static bool s_service_state_stopped(struct aws_mqtt5_client *client) {
    enum aws_mqtt5_client_state desired_state = client->desired_state;
    if (desired_state == AWS_MCS_CONNECTED) {
        s_change_current_state(client, AWS_MCS_CONNECTING);
    } else if (desired_state == AWS_MCS_TERMINATED) {
        s_change_current_state(client, AWS_MCS_TERMINATED);
        return true;
    }

    return false;
}

static void s_service_state_connecting(struct aws_mqtt5_client *client) {
    (void)client;
}

static void s_service_state_mqtt_connect(struct aws_mqtt5_client *client, uint64_t now) {
    enum aws_mqtt5_client_state desired_state = client->desired_state;
    if (desired_state != AWS_MCS_CONNECTED) {
        s_aws_mqtt5_client_emit_final_lifecycle_event(client, AWS_ERROR_MQTT5_USER_REQUESTED_STOP, NULL, NULL);
        s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT5_USER_REQUESTED_STOP);
        return;
    }

    if (now >= client->next_mqtt_connect_packet_timeout_time) {
        s_aws_mqtt5_client_emit_final_lifecycle_event(client, AWS_ERROR_MQTT5_CONNACK_TIMEOUT, NULL, NULL);

        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: shutting down channel due to CONNACK timeout", (void *)client);
        s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT5_CONNACK_TIMEOUT);
        return;
    }

    if (aws_mqtt5_client_service_operational_state(&client->operational_state)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to service outgoing CONNECT packet to channel with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }
}

static int s_aws_mqtt5_client_queue_ping(struct aws_mqtt5_client *client) {
    s_reset_ping(client);

    AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "id=%p: queuing PINGREQ", (void *)client);

    struct aws_mqtt5_operation_pingreq *pingreq_op = aws_mqtt5_operation_pingreq_new(client->allocator);
    s_enqueue_operation_front(client, &pingreq_op->base);

    return AWS_OP_SUCCESS;
}

static void s_service_state_connected(struct aws_mqtt5_client *client, uint64_t now) {
    enum aws_mqtt5_client_state desired_state = client->desired_state;
    if (desired_state != AWS_MCS_CONNECTED) {
        s_aws_mqtt5_client_emit_final_lifecycle_event(client, AWS_ERROR_MQTT5_USER_REQUESTED_STOP, NULL, NULL);

        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: channel shutdown due to user Stop request", (void *)client);
        s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT5_USER_REQUESTED_STOP);
        return;
    }

    if (now >= client->next_ping_timeout_time && client->next_ping_timeout_time != 0) {
        s_aws_mqtt5_client_emit_final_lifecycle_event(client, AWS_ERROR_MQTT5_PING_RESPONSE_TIMEOUT, NULL, NULL);

        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: channel shutdown due to PINGRESP timeout", (void *)client);

        s_aws_mqtt5_client_shutdown_channel_clean(
            client, AWS_ERROR_MQTT5_PING_RESPONSE_TIMEOUT, AWS_MQTT5_DRC_KEEP_ALIVE_TIMEOUT);
        return;
    }

    if (now >= client->next_ping_time) {
        if (s_aws_mqtt5_client_queue_ping(client)) {
            int error_code = aws_last_error();
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT,
                "id=%p: failed to queue PINGREQ with error %d(%s)",
                (void *)client,
                error_code,
                aws_error_debug_str(error_code));

            s_aws_mqtt5_client_shutdown_channel(client, error_code);
            return;
        }
    }

    if (now >= client->next_reconnect_delay_reset_time_ns && client->next_reconnect_delay_reset_time_ns != 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: connected sufficiently long that reconnect backoff delay has been reset back to "
            "minimum value",
            (void *)client);

        client->reconnect_count = 0;
        client->current_reconnect_delay_ms = 0;
        client->next_reconnect_delay_reset_time_ns = 0;
    }

    s_check_timeouts(client, now);

    if (aws_mqtt5_client_service_operational_state(&client->operational_state)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to service CONNECTED operation queue with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }
}

static void s_service_state_clean_disconnect(struct aws_mqtt5_client *client, uint64_t now) {
    if (aws_mqtt5_client_service_operational_state(&client->operational_state)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: failed to service CLEAN_DISCONNECT operation queue with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        s_aws_mqtt5_client_shutdown_channel(client, error_code);
        return;
    }

    s_check_timeouts(client, now);
}

static void s_service_state_channel_shutdown(struct aws_mqtt5_client *client) {
    (void)client;
}

static void s_service_state_pending_reconnect(struct aws_mqtt5_client *client, uint64_t now) {
    if (client->desired_state != AWS_MCS_CONNECTED) {
        s_change_current_state(client, AWS_MCS_STOPPED);
        return;
    }

    if (now >= client->next_reconnect_time_ns) {
        s_change_current_state(client, AWS_MCS_CONNECTING);
        return;
    }
}

static void s_mqtt5_service_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_mqtt5_client *client = arg;
    client->next_service_task_run_time = 0;
    client->in_service = true;

    uint64_t now = (*client->vtable->get_current_time_fn)();
    bool terminated = false;
    switch (client->current_state) {
        case AWS_MCS_STOPPED:
            terminated = s_service_state_stopped(client);
            break;
        case AWS_MCS_CONNECTING:
            s_service_state_connecting(client);
            break;
        case AWS_MCS_MQTT_CONNECT:
            s_service_state_mqtt_connect(client, now);
            break;
        case AWS_MCS_CONNECTED:
            s_service_state_connected(client, now);
            break;
        case AWS_MCS_CLEAN_DISCONNECT:
            s_service_state_clean_disconnect(client, now);
            break;
        case AWS_MCS_CHANNEL_SHUTDOWN:
            s_service_state_channel_shutdown(client);
            break;
        case AWS_MCS_PENDING_RECONNECT:
            s_service_state_pending_reconnect(client, now);
            break;
        default:
            break;
    }

    /*
     * We can only enter the terminated state from stopped.  If we do so, the client memory is now freed and we
     * will crash if we access anything anymore.
     */
    if (terminated) {
        return;
    }

    /* we're not scheduled anymore, reschedule as needed */
    client->in_service = false;
    s_reevaluate_service_task(client);
}

static bool s_should_client_disconnect_cleanly(struct aws_mqtt5_client *client) {
    enum aws_mqtt5_client_state current_state = client->current_state;

    return current_state == AWS_MCS_CONNECTED;
}

static int s_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    struct aws_mqtt5_client *client = handler->impl;

    if (message->message_type != AWS_IO_MESSAGE_APPLICATION_DATA) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: unexpected io message data", (void *)client);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    AWS_LOGF_TRACE(
        AWS_LS_MQTT5_CLIENT, "id=%p: processing read message of size %zu", (void *)client, message->message_data.len);

    struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&message->message_data);

    int result = aws_mqtt5_decoder_on_data_received(&client->decoder, message_cursor);
    if (result != AWS_OP_SUCCESS) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: decode failure with error %d(%s)",
            (void *)client,
            error_code,
            aws_error_debug_str(error_code));

        if (error_code == AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR && s_should_client_disconnect_cleanly(client)) {
            s_aws_mqtt5_client_shutdown_channel_clean(client, error_code, AWS_MQTT5_DRC_PROTOCOL_ERROR);
        } else {
            s_aws_mqtt5_client_shutdown_channel(client, error_code);
        }

        goto done;
    }

    aws_channel_slot_increment_read_window(slot, message->message_data.len);

done:

    aws_mem_release(message->allocator, message);

    return AWS_OP_SUCCESS;
}

static int s_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {

    (void)handler;

    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resources_immediately);
}

static size_t s_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;

    return SIZE_MAX;
}

static void s_destroy(struct aws_channel_handler *handler) {
    (void)handler;
}

static size_t s_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;

    return 0;
}

static struct aws_channel_handler_vtable s_mqtt5_channel_handler_vtable = {
    .process_read_message = &s_process_read_message,
    .process_write_message = NULL,
    .increment_read_window = NULL,
    .shutdown = &s_shutdown,
    .initial_window_size = &s_initial_window_size,
    .message_overhead = &s_message_overhead,
    .destroy = &s_destroy,
};

static bool s_aws_is_successful_reason_code(int value) {
    return value < 128;
}

static void s_aws_mqtt5_client_on_connack(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_packet_connack_view *connack_view) {
    AWS_FATAL_ASSERT(client->current_state == AWS_MCS_MQTT_CONNECT);

    bool is_successful = s_aws_is_successful_reason_code((int)connack_view->reason_code);
    if (!is_successful) {
        s_aws_mqtt5_client_emit_final_lifecycle_event(
            client, AWS_ERROR_MQTT5_CONNACK_CONNECTION_REFUSED, connack_view, NULL);

        enum aws_mqtt5_connect_reason_code reason_code = connack_view->reason_code;

        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: connection refused (via failed CONNACK) by remote host with reason code %d(%s)",
            (void *)client,
            (int)reason_code,
            aws_mqtt5_connect_reason_code_to_c_string(reason_code));

        s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT5_CONNACK_CONNECTION_REFUSED);
        return;
    }

    aws_mqtt5_negotiated_settings_apply_connack(&client->negotiated_settings, connack_view);

    /* Check if a session is being rejoined and perform associated rejoin connect logic here */
    if (client->negotiated_settings.rejoined_session) {
        /* Disconnect if the server is attempting to connect the client to an unexpected session */
        if (!s_should_resume_session(client)) {
            s_aws_mqtt5_client_emit_final_lifecycle_event(
                client, AWS_ERROR_MQTT_CANCELLED_FOR_CLEAN_SESSION, connack_view, NULL);
            s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT_CANCELLED_FOR_CLEAN_SESSION);
            return;
        } else if (!client->has_connected_successfully) {
            /*
             * We were configured with REJOIN_ALWAYS and this is the first connection.  This is technically not safe
             * and so let's log a warning for future diagnostics should it cause the user problems.
             */
            AWS_LOGF_WARN(
                AWS_LS_MQTT5_CLIENT,
                "id=%p: initial connection rejoined existing session.  This may cause packet id collisions.",
                (void *)client);
        }
    }

    s_change_current_state(client, AWS_MCS_CONNECTED);
    s_aws_mqtt5_client_emit_connection_success_lifecycle_event(client, connack_view);
}

static void s_aws_mqtt5_client_log_received_packet(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_packet_type type,
    void *packet_view) {
    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: Received %s packet", (void *)client, aws_mqtt5_packet_type_to_c_string(type));

    switch (type) {
        case AWS_MQTT5_PT_CONNACK:
            aws_mqtt5_packet_connack_view_log(packet_view, AWS_LL_DEBUG);
            break;

        case AWS_MQTT5_PT_PUBLISH:
            aws_mqtt5_packet_publish_view_log(packet_view, AWS_LL_DEBUG);
            break;

        case AWS_MQTT5_PT_PUBACK:
            aws_mqtt5_packet_puback_view_log(packet_view, AWS_LL_DEBUG);
            break;

        case AWS_MQTT5_PT_SUBACK:
            aws_mqtt5_packet_suback_view_log(packet_view, AWS_LL_DEBUG);
            break;

        case AWS_MQTT5_PT_UNSUBACK:
            aws_mqtt5_packet_unsuback_view_log(packet_view, AWS_LL_DEBUG);
            break;

        case AWS_MQTT5_PT_PINGRESP:
            break; /* nothing to log */

        case AWS_MQTT5_PT_DISCONNECT:
            aws_mqtt5_packet_disconnect_view_log(packet_view, AWS_LL_DEBUG);
            break;

        default:
            break;
    }
}

static void s_aws_mqtt5_client_mqtt_connect_on_packet_received(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_packet_type type,
    void *packet_view) {
    if (type == AWS_MQTT5_PT_CONNACK) {
        s_aws_mqtt5_client_on_connack(client, (struct aws_mqtt5_packet_connack_view *)packet_view);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT, "id=%p: Invalid packet type received while in MQTT_CONNECT state", (void *)client);

        s_aws_mqtt5_client_shutdown_channel_clean(
            client, AWS_ERROR_MQTT5_DECODE_PROTOCOL_ERROR, AWS_MQTT5_DRC_PROTOCOL_ERROR);
    }
}

typedef bool(aws_linked_list_node_predicate_fn)(struct aws_linked_list_node *);

/*
 * This predicate finds the first (if any) operation in the queue that is not a PUBACK or a PINGREQ.
 */
static bool s_is_ping_or_puback(struct aws_linked_list_node *operation_node) {
    struct aws_mqtt5_operation *operation = AWS_CONTAINER_OF(operation_node, struct aws_mqtt5_operation, node);

    return operation->packet_type == AWS_MQTT5_PT_PUBACK || operation->packet_type == AWS_MQTT5_PT_PINGREQ;
}

/*
 * Helper function to insert a node (operation) into a list (operation queue) in the correct spot.  Currently, this
 * is only used to enqueue PUBACKs after existing PUBACKs and PINGREQs.  This ensure that PUBACKs go out in the order
 * the corresponding PUBLISH was received, regardless of whether or not there was an intervening service call.
 */
static void s_insert_node_before_predicate_failure(
    struct aws_linked_list *list,
    struct aws_linked_list_node *node,
    aws_linked_list_node_predicate_fn predicate) {
    struct aws_linked_list_node *current_node = NULL;
    for (current_node = aws_linked_list_begin(list); current_node != aws_linked_list_end(list);
         current_node = aws_linked_list_next(current_node)) {
        if (!predicate(current_node)) {
            break;
        }
    }

    AWS_FATAL_ASSERT(current_node != NULL);

    aws_linked_list_insert_before(current_node, node);
}

static int s_aws_mqtt5_client_queue_puback(struct aws_mqtt5_client *client, uint16_t packet_id) {
    AWS_PRECONDITION(client != NULL);

    const struct aws_mqtt5_packet_puback_view puback_view = {
        .packet_id = packet_id,
        .reason_code = AWS_MQTT5_PARC_SUCCESS,
    };

    struct aws_mqtt5_operation_puback *puback_op = aws_mqtt5_operation_puback_new(client->allocator, &puback_view);

    if (puback_op == NULL) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT,
        "id=%p: enqueuing PUBACK operation to first position in queue that is not a PUBACK or PINGREQ",
        (void *)client);

    /*
     * Put the PUBACK ahead of all user-submitted operations (PUBLISH, SUBSCRIBE, UNSUBSCRIBE, DISCONNECT), but behind
     * all pre-existing "internal" operations (PINGREQ, PUBACK).
     *
     * Qos 2 support will need to extend the predicate to include Qos 2 publish packets.
     */
    s_insert_node_before_predicate_failure(
        &client->operational_state.queued_operations, &puback_op->base.node, s_is_ping_or_puback);

    s_reevaluate_service_task(client);

    return AWS_OP_SUCCESS;
}

static void s_aws_mqtt5_client_connected_on_packet_received(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_packet_type type,
    void *packet_view) {

    switch (type) {
        case AWS_MQTT5_PT_PINGRESP:
            AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "id=%p: resetting PINGREQ timer", (void *)client);
            client->next_ping_timeout_time = 0;
            break;

        case AWS_MQTT5_PT_DISCONNECT:
            s_aws_mqtt5_client_emit_final_lifecycle_event(
                client, AWS_ERROR_MQTT5_DISCONNECT_RECEIVED, NULL, packet_view);

            AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "id=%p: shutting down channel due to DISCONNECT", (void *)client);

            s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT5_DISCONNECT_RECEIVED);
            break;

        case AWS_MQTT5_PT_SUBACK: {
            uint16_t packet_id = ((const struct aws_mqtt5_packet_suback_view *)packet_view)->packet_id;
            aws_mqtt5_client_operational_state_handle_ack(
                &client->operational_state, packet_id, AWS_MQTT5_PT_SUBACK, packet_view, AWS_ERROR_SUCCESS);
            break;
        }

        case AWS_MQTT5_PT_UNSUBACK: {
            uint16_t packet_id = ((const struct aws_mqtt5_packet_unsuback_view *)packet_view)->packet_id;
            aws_mqtt5_client_operational_state_handle_ack(
                &client->operational_state, packet_id, AWS_MQTT5_PT_UNSUBACK, packet_view, AWS_ERROR_SUCCESS);
            break;
        }

        case AWS_MQTT5_PT_PUBLISH: {
            const struct aws_mqtt5_packet_publish_view *publish_view = packet_view;

            aws_mqtt5_callback_set_manager_on_publish_received(&client->callback_manager, publish_view);

            /* Send a puback if QoS 1+ */
            if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {

                int result = s_aws_mqtt5_client_queue_puback(client, publish_view->packet_id);
                if (result != AWS_OP_SUCCESS) {
                    int error_code = aws_last_error();
                    AWS_LOGF_ERROR(
                        AWS_LS_MQTT5_CLIENT,
                        "id=%p: decode failure with error %d(%s)",
                        (void *)client,
                        error_code,
                        aws_error_debug_str(error_code));

                    s_aws_mqtt5_client_shutdown_channel(client, error_code);
                }
            }
            break;
        }

        case AWS_MQTT5_PT_PUBACK: {
            uint16_t packet_id = ((const struct aws_mqtt5_packet_puback_view *)packet_view)->packet_id;
            aws_mqtt5_client_operational_state_handle_ack(
                &client->operational_state, packet_id, AWS_MQTT5_PT_PUBACK, packet_view, AWS_ERROR_SUCCESS);
            break;
        }

        default:
            break;
    }
}

static int s_aws_mqtt5_client_on_packet_received(
    enum aws_mqtt5_packet_type type,
    void *packet_view,
    void *decoder_callback_user_data) {

    struct aws_mqtt5_client *client = decoder_callback_user_data;

    s_aws_mqtt5_client_log_received_packet(client, type, packet_view);

    switch (client->current_state) {
        case AWS_MCS_MQTT_CONNECT:
            s_aws_mqtt5_client_mqtt_connect_on_packet_received(client, type, packet_view);
            break;

        case AWS_MCS_CONNECTED:
        case AWS_MCS_CLEAN_DISCONNECT:
            s_aws_mqtt5_client_connected_on_packet_received(client, type, packet_view);
            break;

        default:
            break;
    }

    s_reevaluate_service_task(client);

    return AWS_OP_SUCCESS;
}

static uint64_t s_aws_high_res_clock_get_ticks_proxy(void) {
    uint64_t current_time = 0;
    AWS_FATAL_ASSERT(aws_high_res_clock_get_ticks(&current_time) == AWS_OP_SUCCESS);

    return current_time;
}

struct aws_io_message *s_aws_channel_acquire_message_from_pool_default(
    struct aws_channel *channel,
    enum aws_io_message_type message_type,
    size_t size_hint,
    void *user_data) {
    (void)user_data;

    return aws_channel_acquire_message_from_pool(channel, message_type, size_hint);
}

static int s_aws_channel_slot_send_message_default(
    struct aws_channel_slot *slot,
    struct aws_io_message *message,
    enum aws_channel_direction dir,
    void *user_data) {
    (void)user_data;

    return aws_channel_slot_send_message(slot, message, dir);
}

static struct aws_mqtt5_client_vtable s_default_client_vtable = {
    .get_current_time_fn = s_aws_high_res_clock_get_ticks_proxy,
    .channel_shutdown_fn = aws_channel_shutdown,
    .websocket_connect_fn = aws_websocket_client_connect,
    .client_bootstrap_new_socket_channel_fn = aws_client_bootstrap_new_socket_channel,
    .http_proxy_new_socket_channel_fn = aws_http_proxy_new_socket_channel,
    .on_client_state_change_callback_fn = NULL,
    .aws_channel_acquire_message_from_pool_fn = s_aws_channel_acquire_message_from_pool_default,
    .aws_channel_slot_send_message_fn = s_aws_channel_slot_send_message_default,

    .vtable_user_data = NULL,
};

void aws_mqtt5_client_set_vtable(struct aws_mqtt5_client *client, const struct aws_mqtt5_client_vtable *vtable) {
    client->vtable = vtable;
}

const struct aws_mqtt5_client_vtable *aws_mqtt5_client_get_default_vtable(void) {
    return &s_default_client_vtable;
}

struct aws_mqtt5_client *aws_mqtt5_client_new(
    struct aws_allocator *allocator,
    const struct aws_mqtt5_client_options *options) {
    AWS_FATAL_ASSERT(allocator != NULL);
    AWS_FATAL_ASSERT(options != NULL);

    struct aws_mqtt5_client *client = aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt5_client));
    if (client == NULL) {
        return NULL;
    }

    aws_task_init(&client->service_task, s_mqtt5_service_task_fn, client, "Mqtt5Service");

    client->allocator = allocator;
    client->vtable = &s_default_client_vtable;

    aws_ref_count_init(&client->ref_count, client, s_on_mqtt5_client_zero_ref_count);

    aws_mqtt5_callback_set_manager_init(&client->callback_manager, client);

    if (aws_mqtt5_client_operational_state_init(&client->operational_state, allocator, client)) {
        goto on_error;
    }

    client->config = aws_mqtt5_client_options_storage_new(allocator, options);
    if (client->config == NULL) {
        goto on_error;
    }

    aws_mqtt5_client_flow_control_state_init(client);

    /* all client activity will take place on this event loop, serializing things like reconnect, ping, etc... */
    client->loop = aws_event_loop_group_get_next_loop(client->config->bootstrap->event_loop_group);
    if (client->loop == NULL) {
        goto on_error;
    }

    client->desired_state = AWS_MCS_STOPPED;
    client->current_state = AWS_MCS_STOPPED;
    client->lifecycle_state = AWS_MQTT5_LS_NONE;

    struct aws_mqtt5_decoder_options decoder_options = {
        .callback_user_data = client,
        .on_packet_received = s_aws_mqtt5_client_on_packet_received,
    };

    if (aws_mqtt5_decoder_init(&client->decoder, allocator, &decoder_options)) {
        goto on_error;
    }

    struct aws_mqtt5_encoder_options encoder_options = {
        .client = client,
    };

    if (aws_mqtt5_encoder_init(&client->encoder, allocator, &encoder_options)) {
        goto on_error;
    }

    if (aws_mqtt5_inbound_topic_alias_resolver_init(&client->inbound_topic_alias_resolver, allocator)) {
        goto on_error;
    }

    client->outbound_topic_alias_resolver = aws_mqtt5_outbound_topic_alias_resolver_new(
        allocator, client->config->topic_aliasing_options.outbound_topic_alias_behavior);
    if (client->outbound_topic_alias_resolver == NULL) {
        goto on_error;
    }

    if (aws_mqtt5_negotiated_settings_init(
            allocator, &client->negotiated_settings, &options->connect_options->client_id)) {
        goto on_error;
    }

    client->current_reconnect_delay_ms = 0;

    client->handler.alloc = client->allocator;
    client->handler.vtable = &s_mqtt5_channel_handler_vtable;
    client->handler.impl = client;

    aws_mqtt5_client_options_storage_log(client->config, AWS_LL_DEBUG);

    s_init_statistics(&client->operation_statistics_impl);

    return client;

on_error:

    /* release isn't usable here since we may not even have an event loop */
    s_mqtt5_client_final_destroy(client);

    return NULL;
}

struct aws_mqtt5_client *aws_mqtt5_client_acquire(struct aws_mqtt5_client *client) {
    if (client != NULL) {
        aws_ref_count_acquire(&client->ref_count);
    }

    return client;
}

struct aws_mqtt5_client *aws_mqtt5_client_release(struct aws_mqtt5_client *client) {
    if (client != NULL) {
        aws_ref_count_release(&client->ref_count);
    }

    return NULL;
}

struct aws_mqtt_change_desired_state_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt5_client *client;
    enum aws_mqtt5_client_state desired_state;
    struct aws_mqtt5_operation_disconnect *disconnect_operation;
};

void aws_mqtt5_client_change_desired_state(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_client_state desired_state,
    struct aws_mqtt5_operation_disconnect *disconnect_op) {
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(client->loop));

    if (client->desired_state != desired_state) {
        AWS_LOGF_INFO(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: changing desired client state from %s to %s",
            (void *)client,
            aws_mqtt5_client_state_to_c_string(client->desired_state),
            aws_mqtt5_client_state_to_c_string(desired_state));

        client->desired_state = desired_state;

        if (desired_state == AWS_MCS_STOPPED && disconnect_op != NULL) {
            s_aws_mqtt5_client_shutdown_channel_with_disconnect(
                client, AWS_ERROR_MQTT5_USER_REQUESTED_STOP, disconnect_op);
        }

        s_reevaluate_service_task(client);
    }
}

static void s_change_state_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt_change_desired_state_task *change_state_task = arg;
    struct aws_mqtt5_client *client = change_state_task->client;
    enum aws_mqtt5_client_state desired_state = change_state_task->desired_state;
    if (status != AWS_TASK_STATUS_RUN_READY) {
        goto done;
    }

    aws_mqtt5_client_change_desired_state(client, desired_state, change_state_task->disconnect_operation);

done:

    aws_mqtt5_operation_disconnect_release(change_state_task->disconnect_operation);
    if (desired_state != AWS_MCS_TERMINATED) {
        aws_mqtt5_client_release(client);
    }

    aws_mem_release(change_state_task->allocator, change_state_task);
}

static struct aws_mqtt_change_desired_state_task *s_aws_mqtt_change_desired_state_task_new(
    struct aws_allocator *allocator,
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_client_state desired_state,
    struct aws_mqtt5_operation_disconnect *disconnect_operation) {

    struct aws_mqtt_change_desired_state_task *change_state_task =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_mqtt_change_desired_state_task));
    if (change_state_task == NULL) {
        return NULL;
    }

    aws_task_init(&change_state_task->task, s_change_state_task_fn, (void *)change_state_task, "ChangeStateTask");
    change_state_task->allocator = client->allocator;
    change_state_task->client = (desired_state == AWS_MCS_TERMINATED) ? client : aws_mqtt5_client_acquire(client);
    change_state_task->desired_state = desired_state;
    change_state_task->disconnect_operation = aws_mqtt5_operation_disconnect_acquire(disconnect_operation);

    return change_state_task;
}

static bool s_is_valid_desired_state(enum aws_mqtt5_client_state desired_state) {
    switch (desired_state) {
        case AWS_MCS_STOPPED:
        case AWS_MCS_CONNECTED:
        case AWS_MCS_TERMINATED:
            return true;

        default:
            return false;
    }
}

static int s_aws_mqtt5_client_change_desired_state(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_client_state desired_state,
    struct aws_mqtt5_operation_disconnect *disconnect_operation) {
    AWS_FATAL_ASSERT(client != NULL);
    AWS_FATAL_ASSERT(client->loop != NULL);
    AWS_FATAL_ASSERT(disconnect_operation == NULL || desired_state == AWS_MCS_STOPPED);

    if (!s_is_valid_desired_state(desired_state)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: invalid desired state argument %d(%s)",
            (void *)client,
            (int)desired_state,
            aws_mqtt5_client_state_to_c_string(desired_state));

        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_mqtt_change_desired_state_task *task =
        s_aws_mqtt_change_desired_state_task_new(client->allocator, client, desired_state, disconnect_operation);
    if (task == NULL) {
        AWS_LOGF_ERROR(AWS_LS_MQTT5_CLIENT, "id=%p: failed to create change desired state task", (void *)client);
        return AWS_OP_ERR;
    }

    aws_event_loop_schedule_task_now(client->loop, &task->task);

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_client_start(struct aws_mqtt5_client *client) {
    return s_aws_mqtt5_client_change_desired_state(client, AWS_MCS_CONNECTED, NULL);
}

int aws_mqtt5_client_stop(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_disconnect_view *options,
    const struct aws_mqtt5_disconnect_completion_options *completion_options) {
    AWS_FATAL_ASSERT(client != NULL);
    struct aws_mqtt5_operation_disconnect *disconnect_op = NULL;
    if (options != NULL) {
        struct aws_mqtt5_disconnect_completion_options internal_completion_options = {
            .completion_callback = s_on_disconnect_operation_complete,
            .completion_user_data = client,
        };

        disconnect_op = aws_mqtt5_operation_disconnect_new(
            client->allocator, options, completion_options, &internal_completion_options);
        if (disconnect_op == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT5_CLIENT, "id=%p: failed to create requested DISCONNECT operation", (void *)client);
            return AWS_OP_ERR;
        }

        AWS_LOGF_DEBUG(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: Stopping client via DISCONNECT operation (%p)",
            (void *)client,
            (void *)disconnect_op);
        aws_mqtt5_packet_disconnect_view_log(disconnect_op->base.packet_view, AWS_LL_DEBUG);
    } else {
        AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "id=%p: Stopping client immediately", (void *)client);
    }

    int result = s_aws_mqtt5_client_change_desired_state(client, AWS_MCS_STOPPED, disconnect_op);

    aws_mqtt5_operation_disconnect_release(disconnect_op);

    return result;
}

struct aws_mqtt5_submit_operation_task {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_mqtt5_client *client;
    struct aws_mqtt5_operation *operation;
};

void aws_mqtt5_client_submit_operation_internal(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    bool is_terminated) {

    /*
     * Take a ref to the operation that represents the client taking ownership
     * If we subsequently reject it (task cancel or offline queue policy), then the operation completion
     * will undo this ref acquisition.
     */
    aws_mqtt5_operation_acquire(operation);

    if (is_terminated) {
        s_complete_operation(NULL, operation, AWS_ERROR_MQTT5_CLIENT_TERMINATED, AWS_MQTT5_PT_NONE, NULL);
        return;
    }

    /*
     * If we're offline and this operation doesn't meet the requirements of the offline queue retention policy,
     * fail it immediately.
     */
    if (client->current_state != AWS_MCS_CONNECTED) {
        if (!s_aws_mqtt5_operation_satisfies_offline_queue_retention_policy(
                operation, client->config->offline_queue_behavior)) {
            s_complete_operation(
                NULL, operation, AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY, AWS_MQTT5_PT_NONE, NULL);
            return;
        }
    }

    /* newly-submitted operations must have a 0 packet id */
    aws_mqtt5_operation_set_packet_id(operation, 0);

    s_enqueue_operation_back(client, operation);
    aws_mqtt5_client_statistics_change_operation_statistic_state(client, operation, AWS_MQTT5_OSS_INCOMPLETE);
}

static void s_mqtt5_submit_operation_task_fn(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct aws_mqtt5_submit_operation_task *submit_operation_task = arg;
    struct aws_mqtt5_client *client = submit_operation_task->client;
    struct aws_mqtt5_operation *operation = submit_operation_task->operation;

    aws_mqtt5_client_submit_operation_internal(client, operation, status != AWS_TASK_STATUS_RUN_READY);

    aws_mqtt5_operation_release(submit_operation_task->operation);
    aws_mqtt5_client_release(submit_operation_task->client);

    aws_mem_release(submit_operation_task->allocator, submit_operation_task);
}

static int s_submit_operation(struct aws_mqtt5_client *client, struct aws_mqtt5_operation *operation) {
    struct aws_mqtt5_submit_operation_task *submit_task =
        aws_mem_calloc(client->allocator, 1, sizeof(struct aws_mqtt5_submit_operation_task));
    if (submit_task == NULL) {
        return AWS_OP_ERR;
    }

    aws_task_init(&submit_task->task, s_mqtt5_submit_operation_task_fn, submit_task, "Mqtt5SubmitOperation");
    submit_task->allocator = client->allocator;
    submit_task->client = aws_mqtt5_client_acquire(client);
    submit_task->operation = operation;

    aws_event_loop_schedule_task_now(client->loop, &submit_task->task);

    return AWS_OP_SUCCESS;
}

int aws_mqtt5_client_publish(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_publish_view *publish_options,
    const struct aws_mqtt5_publish_completion_options *completion_options) {

    AWS_PRECONDITION(client != NULL);
    AWS_PRECONDITION(publish_options != NULL);

    struct aws_mqtt5_operation_publish *publish_op =
        aws_mqtt5_operation_publish_new(client->allocator, client, publish_options, completion_options);

    if (publish_op == NULL) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "id=%p: Submitting PUBLISH operation (%p)", (void *)client, (void *)publish_op);
    aws_mqtt5_packet_publish_view_log(publish_op->base.packet_view, AWS_LL_DEBUG);

    if (s_submit_operation(client, &publish_op->base)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    aws_mqtt5_operation_release(&publish_op->base);

    return AWS_OP_ERR;
}

int aws_mqtt5_client_subscribe(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_subscribe_view *subscribe_options,
    const struct aws_mqtt5_subscribe_completion_options *completion_options) {

    AWS_PRECONDITION(client != NULL);
    AWS_PRECONDITION(subscribe_options != NULL);

    struct aws_mqtt5_operation_subscribe *subscribe_op =
        aws_mqtt5_operation_subscribe_new(client->allocator, client, subscribe_options, completion_options);

    if (subscribe_op == NULL) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: Submitting SUBSCRIBE operation (%p)", (void *)client, (void *)subscribe_op);
    aws_mqtt5_packet_subscribe_view_log(subscribe_op->base.packet_view, AWS_LL_DEBUG);

    if (s_submit_operation(client, &subscribe_op->base)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    aws_mqtt5_operation_release(&subscribe_op->base);

    return AWS_OP_ERR;
}

int aws_mqtt5_client_unsubscribe(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_unsubscribe_view *unsubscribe_options,
    const struct aws_mqtt5_unsubscribe_completion_options *completion_options) {

    AWS_PRECONDITION(client != NULL);
    AWS_PRECONDITION(unsubscribe_options != NULL);

    struct aws_mqtt5_operation_unsubscribe *unsubscribe_op =
        aws_mqtt5_operation_unsubscribe_new(client->allocator, client, unsubscribe_options, completion_options);

    if (unsubscribe_op == NULL) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: Submitting UNSUBSCRIBE operation (%p)", (void *)client, (void *)unsubscribe_op);
    aws_mqtt5_packet_unsubscribe_view_log(unsubscribe_op->base.packet_view, AWS_LL_DEBUG);

    if (s_submit_operation(client, &unsubscribe_op->base)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:

    aws_mqtt5_operation_release(&unsubscribe_op->base);

    return AWS_OP_ERR;
}

static bool s_needs_packet_id(const struct aws_mqtt5_operation *operation) {
    switch (operation->packet_type) {
        case AWS_MQTT5_PT_SUBSCRIBE:
        case AWS_MQTT5_PT_UNSUBSCRIBE:
            return aws_mqtt5_operation_get_packet_id(operation) == 0;

        case AWS_MQTT5_PT_PUBLISH: {
            const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
            if (publish_view->qos == AWS_MQTT5_QOS_AT_MOST_ONCE) {
                return false;
            }

            return aws_mqtt5_operation_get_packet_id(operation) == 0;
        }

        default:
            return false;
    }
}

static uint16_t s_next_packet_id(uint16_t current_id) {
    if (++current_id == 0) {
        current_id = 1;
    }

    return current_id;
}

int aws_mqtt5_operation_bind_packet_id(
    struct aws_mqtt5_operation *operation,
    struct aws_mqtt5_client_operational_state *client_operational_state) {
    if (!s_needs_packet_id(operation)) {
        return AWS_OP_SUCCESS;
    }

    uint16_t current_id = client_operational_state->next_mqtt_packet_id;
    struct aws_hash_element *elem = NULL;
    for (uint16_t i = 0; i < UINT16_MAX; ++i) {
        aws_hash_table_find(&client_operational_state->unacked_operations_table, &current_id, &elem);

        if (elem == NULL) {
            aws_mqtt5_operation_set_packet_id(operation, current_id);
            client_operational_state->next_mqtt_packet_id = s_next_packet_id(current_id);

            return AWS_OP_SUCCESS;
        }

        current_id = s_next_packet_id(current_id);
    }

    aws_raise_error(AWS_ERROR_INVALID_STATE);
    return AWS_OP_ERR;
}

/*
 * Priority queue comparison function for ack timeout processing
 */
static int s_compare_operation_timeouts(const void *a, const void *b) {
    const struct aws_mqtt5_operation **operation_a_ptr = (void *)a;
    const struct aws_mqtt5_operation *operation_a = *operation_a_ptr;

    const struct aws_mqtt5_operation **operation_b_ptr = (void *)b;
    const struct aws_mqtt5_operation *operation_b = *operation_b_ptr;

    if (operation_a->ack_timeout_timepoint_ns < operation_b->ack_timeout_timepoint_ns) {
        return -1;
    } else if (operation_a->ack_timeout_timepoint_ns > operation_b->ack_timeout_timepoint_ns) {
        return 1;
    } else {
        return 0;
    }
}

int aws_mqtt5_client_operational_state_init(
    struct aws_mqtt5_client_operational_state *client_operational_state,
    struct aws_allocator *allocator,
    struct aws_mqtt5_client *client) {

    aws_linked_list_init(&client_operational_state->queued_operations);
    aws_linked_list_init(&client_operational_state->write_completion_operations);
    aws_linked_list_init(&client_operational_state->unacked_operations);

    if (aws_hash_table_init(
            &client_operational_state->unacked_operations_table,
            allocator,
            DEFAULT_MQTT5_OPERATION_TABLE_SIZE,
            aws_mqtt_hash_uint16_t,
            aws_mqtt_compare_uint16_t_eq,
            NULL,
            NULL)) {
        return AWS_OP_ERR;
    }

    if (aws_priority_queue_init_dynamic(
            &client_operational_state->operations_by_ack_timeout,
            allocator,
            100,
            sizeof(struct aws_mqtt5_operation *),
            s_compare_operation_timeouts)) {
        return AWS_OP_ERR;
    }

    client_operational_state->next_mqtt_packet_id = 1;
    client_operational_state->current_operation = NULL;
    client_operational_state->client = client;

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_client_operational_state_clean_up(struct aws_mqtt5_client_operational_state *client_operational_state) {
    AWS_ASSERT(client_operational_state->current_operation == NULL);

    s_aws_mqtt5_client_operational_state_reset(client_operational_state, AWS_ERROR_MQTT5_CLIENT_TERMINATED, true);
}

static bool s_filter_queued_operations_for_offline(struct aws_mqtt5_operation *operation, void *context) {
    struct aws_mqtt5_client *client = context;
    enum aws_mqtt5_client_operation_queue_behavior_type queue_behavior = client->config->offline_queue_behavior;

    return !s_aws_mqtt5_operation_satisfies_offline_queue_retention_policy(operation, queue_behavior);
}

static void s_process_unacked_operations_for_disconnect(struct aws_mqtt5_operation *operation, void *context) {
    (void)context;

    if (operation->packet_type == AWS_MQTT5_PT_PUBLISH) {
        struct aws_mqtt5_packet_publish_view *publish_view =
            (struct aws_mqtt5_packet_publish_view *)operation->packet_view;
        if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {
            publish_view->duplicate = true;
            return;
        }
    }

    aws_mqtt5_operation_set_packet_id(operation, 0);
}

static bool s_filter_unacked_operations_for_offline(struct aws_mqtt5_operation *operation, void *context) {
    struct aws_mqtt5_client *client = context;
    enum aws_mqtt5_client_operation_queue_behavior_type queue_behavior = client->config->offline_queue_behavior;

    if (operation->packet_type == AWS_MQTT5_PT_PUBLISH) {
        const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
        if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {
            return false;
        }
    }

    return !s_aws_mqtt5_operation_satisfies_offline_queue_retention_policy(operation, queue_behavior);
}

/*
 * Resets the client's operational state based on a disconnection (from above comment):
 *
 *      If current_operation
 *         move current_operation to head of queued_operations
 *      Fail all operations in the pending write completion list
 *      Fail, remove, and release operations in queued_operations where they fail the offline queue policy
 *      Iterate unacked_operations:
 *         If qos1+ publish
 *            set dup flag
 *         else
 *            unset/release packet id
 *      Fail, remove, and release unacked_operations if:
 *         (1) They fail the offline queue policy AND
 *         (2) the operation is not Qos 1+ publish
 *
 *      Clears the unacked_operations table
 */
void aws_mqtt5_client_on_disconnection_update_operational_state(struct aws_mqtt5_client *client) {
    struct aws_mqtt5_client_operational_state *client_operational_state = &client->operational_state;

    /* move current operation to the head of the queue */
    if (client_operational_state->current_operation != NULL) {
        aws_linked_list_push_front(
            &client_operational_state->queued_operations, &client_operational_state->current_operation->node);
        client_operational_state->current_operation = NULL;
    }

    /* fail everything in pending write completion */
    s_complete_operation_list(
        client,
        &client_operational_state->write_completion_operations,
        AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY);

    struct aws_linked_list operations_to_fail;
    AWS_ZERO_STRUCT(operations_to_fail);
    aws_linked_list_init(&operations_to_fail);

    /* fail everything in the pending queue that doesn't meet the offline queue behavior retention requirements */
    s_filter_operation_list(
        &client_operational_state->queued_operations,
        s_filter_queued_operations_for_offline,
        &operations_to_fail,
        client);
    s_complete_operation_list(
        client, &operations_to_fail, AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY);

    /* Mark unacked qos1+ publishes as duplicate and release packet ids for non qos1+ publish */
    s_apply_to_operation_list(
        &client_operational_state->unacked_operations, s_process_unacked_operations_for_disconnect, NULL);

    /*
     * fail everything in the pending queue that
     *   (1) isn't a qos1+ publish AND
     *   (2) doesn't meet the offline queue behavior retention requirements
     */
    s_filter_operation_list(
        &client_operational_state->unacked_operations,
        s_filter_unacked_operations_for_offline,
        &operations_to_fail,
        client);
    s_complete_operation_list(
        client, &operations_to_fail, AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY);

    aws_hash_table_clear(&client->operational_state.unacked_operations_table);
    aws_priority_queue_clear(&client->operational_state.operations_by_ack_timeout);

    /*
     * Prevents inbound resolution on the highly unlikely, illegal server behavior of sending a PUBLISH before
     * a CONNACK on next connection establishment.
     */
    aws_mqtt5_decoder_set_inbound_topic_alias_resolver(&client->decoder, NULL);
}

static void s_set_operation_list_statistic_state(
    struct aws_mqtt5_client *client,
    struct aws_linked_list *operation_list,
    enum aws_mqtt5_operation_statistic_state_flags new_state_flags) {
    struct aws_linked_list_node *node = aws_linked_list_begin(operation_list);
    while (node != aws_linked_list_end(operation_list)) {
        struct aws_mqtt5_operation *operation = AWS_CONTAINER_OF(node, struct aws_mqtt5_operation, node);
        node = aws_linked_list_next(node);

        aws_mqtt5_client_statistics_change_operation_statistic_state(client, operation, new_state_flags);
    }
}

static bool s_filter_unacked_operations_for_session_rejoin(struct aws_mqtt5_operation *operation, void *context) {
    (void)context;

    if (operation->packet_type == AWS_MQTT5_PT_PUBLISH) {
        const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
        if (publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE) {
            return false;
        }
    }

    return true;
}

/*
 * Updates the client's operational state based on a successfully established connection event:
 *
 *      if rejoined_session:
 *          Move-and-append all non-qos1+-publishes in unacked_operations to the front of queued_operations
 *          Move-and-append remaining operations (qos1+ publishes) to the front of queued_operations
 *      else:
 *          Fail, remove, and release unacked_operations that fail the offline queue policy
 *          Move and append unacked operations to front of queued_operations
 */
void aws_mqtt5_client_on_connection_update_operational_state(struct aws_mqtt5_client *client) {

    struct aws_mqtt5_client_operational_state *client_operational_state = &client->operational_state;

    if (client->negotiated_settings.rejoined_session) {
        struct aws_linked_list requeued_operations;
        AWS_ZERO_STRUCT(requeued_operations);
        aws_linked_list_init(&requeued_operations);

        /*
         * qos1+ publishes must go out first, so split the unacked operation list into two sets: qos1+ publishes and
         * everything else.
         */
        s_filter_operation_list(
            &client_operational_state->unacked_operations,
            s_filter_unacked_operations_for_session_rejoin,
            &requeued_operations,
            client);

        /*
         * Put non-qos1+ publishes on the front of the pending queue
         */
        aws_linked_list_move_all_front(&client->operational_state.queued_operations, &requeued_operations);

        /*
         * Put qos1+ publishes on the front of the pending queue
         */
        aws_linked_list_move_all_front(
            &client->operational_state.queued_operations, &client_operational_state->unacked_operations);
    } else {
        struct aws_linked_list failed_operations;
        AWS_ZERO_STRUCT(failed_operations);
        aws_linked_list_init(&failed_operations);

        s_filter_operation_list(
            &client_operational_state->unacked_operations,
            s_filter_queued_operations_for_offline,
            &failed_operations,
            client);

        /*
         * fail operations that we aren't going to requeue.  In this particular case it's only qos1+ publishes
         * that we didn't fail because we didn't know if we were going to rejoin a sesison or not.
         */
        s_complete_operation_list(
            client, &failed_operations, AWS_ERROR_MQTT5_OPERATION_FAILED_DUE_TO_OFFLINE_QUEUE_POLICY);

        /* requeue operations that we are going to perform again */
        aws_linked_list_move_all_front(
            &client->operational_state.queued_operations, &client->operational_state.unacked_operations);
    }

    /* set everything remaining to incomplete */
    s_set_operation_list_statistic_state(
        client, &client->operational_state.queued_operations, AWS_MQTT5_OSS_INCOMPLETE);

    aws_mqtt5_client_flow_control_state_reset(client);

    uint16_t inbound_alias_maximum = client->negotiated_settings.topic_alias_maximum_to_client;

    if (aws_mqtt5_inbound_topic_alias_resolver_reset(&client->inbound_topic_alias_resolver, inbound_alias_maximum)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: client unable to reset inbound alias resolver",
            (void *)client_operational_state->client);
        goto on_error;
    }

    if (inbound_alias_maximum > 0) {
        aws_mqtt5_decoder_set_inbound_topic_alias_resolver(&client->decoder, &client->inbound_topic_alias_resolver);
    } else {
        aws_mqtt5_decoder_set_inbound_topic_alias_resolver(&client->decoder, NULL);
    }

    uint16_t outbound_alias_maximum = client->negotiated_settings.topic_alias_maximum_to_server;
    if (aws_mqtt5_outbound_topic_alias_resolver_reset(client->outbound_topic_alias_resolver, outbound_alias_maximum)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: client unable to reset outbound alias resolver",
            (void *)client_operational_state->client);
        goto on_error;
    }

    aws_mqtt5_encoder_set_outbound_topic_alias_resolver(&client->encoder, client->outbound_topic_alias_resolver);

    return;

on_error:

    s_aws_mqtt5_client_shutdown_channel(client, aws_last_error());
}

static bool s_aws_mqtt5_client_has_pending_operational_work(
    const struct aws_mqtt5_client_operational_state *client_operational_state,
    enum aws_mqtt5_client_state client_state) {
    if (aws_linked_list_empty(&client_operational_state->queued_operations)) {
        return false;
    }

    struct aws_linked_list_node *next_operation_node =
        aws_linked_list_front(&client_operational_state->queued_operations);
    struct aws_mqtt5_operation *next_operation =
        AWS_CONTAINER_OF(next_operation_node, struct aws_mqtt5_operation, node);

    switch (client_state) {
        case AWS_MCS_MQTT_CONNECT:
            /* Only allowed to send a CONNECT packet in this state */
            return next_operation->packet_type == AWS_MQTT5_PT_CONNECT;

        case AWS_MCS_CLEAN_DISCONNECT:
            /* Except for finishing the current operation, only allowed to send a DISCONNECT packet in this state */
            return next_operation->packet_type == AWS_MQTT5_PT_DISCONNECT;

        case AWS_MCS_CONNECTED:
            return true;

        default:
            return false;
    }
}

static uint64_t s_aws_mqtt5_client_compute_next_operation_flow_control_service_time(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    uint64_t now) {
    (void)operation;

    switch (client->current_state) {
        case AWS_MCS_MQTT_CONNECT:
        case AWS_MCS_CLEAN_DISCONNECT:
            return now;

        case AWS_MCS_CONNECTED:
            return aws_mqtt5_client_flow_control_state_get_next_operation_service_time(client, operation, now);

        default:
            /* no outbound traffic is allowed outside of the above states */
            return 0;
    }
}

/*
 * We don't presently know if IoT Core's throughput limit is on the plaintext or encrypted data stream.  Assume
 * it's on the encrypted stream for now and make a reasonable guess at the additional cost TLS imposes on data size:
 *
 * This calculation is intended to be a reasonable default but will not be accurate in all cases
 *
 * Estimate the # of ethernet frames (max 1444 bytes) and add in potential TLS framing and padding values per.
 *
 * TODO: query IoT Core to determine if this calculation is needed after all
 * TODO: may eventually want to expose the ethernet frame size here as a configurable option for networks that have a
 * lower MTU
 *
 * References:
 *  https://tools.ietf.org/id/draft-mattsson-uta-tls-overhead-01.xml#rfc.section.3
 *
 */

#define ETHERNET_FRAME_MAX_PAYLOAD_SIZE 1500
#define TCP_SIZE_OVERESTIMATE 72
#define TLS_FRAMING_AND_PADDING_OVERESTIMATE 64
#define AVAILABLE_ETHERNET_FRAME_SIZE                                                                                  \
    (ETHERNET_FRAME_MAX_PAYLOAD_SIZE - (TCP_SIZE_OVERESTIMATE + TLS_FRAMING_AND_PADDING_OVERESTIMATE))
#define ETHERNET_FRAMES_PER_IO_MESSAGE_ESTIMATE                                                                        \
    ((AWS_MQTT5_IO_MESSAGE_DEFAULT_LENGTH + AVAILABLE_ETHERNET_FRAME_SIZE - 1) / AVAILABLE_ETHERNET_FRAME_SIZE)
#define THROUGHPUT_TOKENS_PER_IO_MESSAGE_OVERESTIMATE                                                                  \
    (AWS_MQTT5_IO_MESSAGE_DEFAULT_LENGTH +                                                                             \
     ETHERNET_FRAMES_PER_IO_MESSAGE_ESTIMATE * TLS_FRAMING_AND_PADDING_OVERESTIMATE)

static uint64_t s_compute_throughput_throttle_wait(const struct aws_mqtt5_client *client, uint64_t now) {

    /* flow control only applies during CONNECTED/CLEAN_DISCONNECT */
    if (!aws_mqtt5_client_are_negotiated_settings_valid(client)) {
        return now;
    }

    uint64_t throughput_wait = 0;
    if (client->config->extended_validation_and_flow_control_options != AWS_MQTT5_EVAFCO_NONE) {
        throughput_wait = aws_rate_limiter_token_bucket_compute_wait_for_tokens(
            (struct aws_rate_limiter_token_bucket *)&client->flow_control_state.throughput_throttle,
            THROUGHPUT_TOKENS_PER_IO_MESSAGE_OVERESTIMATE);
    }

    return aws_add_u64_saturating(now, throughput_wait);
}

static uint64_t s_aws_mqtt5_client_compute_operational_state_service_time(
    const struct aws_mqtt5_client_operational_state *client_operational_state,
    uint64_t now) {
    /* If an io message is in transit down the channel, then wait for it to complete */
    if (client_operational_state->pending_write_completion) {
        return 0;
    }

    /* Throughput flow control check */
    uint64_t next_throttled_time = s_compute_throughput_throttle_wait(client_operational_state->client, now);
    if (next_throttled_time > now) {
        return next_throttled_time;
    }

    /* If we're in the middle of something, keep going */
    if (client_operational_state->current_operation != NULL) {
        return now;
    }

    /* If nothing is queued, there's nothing to do */
    enum aws_mqtt5_client_state client_state = client_operational_state->client->current_state;
    if (!s_aws_mqtt5_client_has_pending_operational_work(client_operational_state, client_state)) {
        return 0;
    }

    AWS_FATAL_ASSERT(!aws_linked_list_empty(&client_operational_state->queued_operations));

    struct aws_linked_list_node *next_operation_node =
        aws_linked_list_front(&client_operational_state->queued_operations);
    struct aws_mqtt5_operation *next_operation =
        AWS_CONTAINER_OF(next_operation_node, struct aws_mqtt5_operation, node);

    AWS_FATAL_ASSERT(next_operation != NULL);

    /*
     * Check the head of the pending operation queue against flow control and client state restrictions
     */
    return s_aws_mqtt5_client_compute_next_operation_flow_control_service_time(
        client_operational_state->client, next_operation, now);
}

static bool s_aws_mqtt5_client_should_service_operational_state(
    const struct aws_mqtt5_client_operational_state *client_operational_state,
    uint64_t now) {

    return now == s_aws_mqtt5_client_compute_operational_state_service_time(client_operational_state, now);
}

static bool s_operation_requires_ack(const struct aws_mqtt5_operation *operation) {
    switch (operation->packet_type) {
        case AWS_MQTT5_PT_SUBSCRIBE:
        case AWS_MQTT5_PT_UNSUBSCRIBE:
            return true;

        case AWS_MQTT5_PT_PUBLISH: {
            const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
            return publish_view->qos != AWS_MQTT5_QOS_AT_MOST_ONCE;
        }

        default:
            return false;
    }
}

static void s_on_pingreq_send(struct aws_mqtt5_client *client) {
    uint64_t now = client->vtable->get_current_time_fn();
    uint64_t ping_timeout_nanos =
        aws_timestamp_convert(client->config->ping_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
    uint64_t half_keep_alive_nanos =
        aws_timestamp_convert(
            client->negotiated_settings.server_keep_alive, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL) /
        2;

    uint64_t connection_ping_timeout = ping_timeout_nanos;
    if (connection_ping_timeout == 0 || connection_ping_timeout > half_keep_alive_nanos) {
        connection_ping_timeout = half_keep_alive_nanos;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT5_CLIENT, "id=%p: dynamic ping timeout: %" PRIu64 " ns", (void *)client, connection_ping_timeout);

    client->next_ping_timeout_time = aws_add_u64_saturating(now, connection_ping_timeout);
}

static int s_apply_throughput_flow_control(struct aws_mqtt5_client *client) {
    /* flow control only applies during CONNECTED/CLEAN_DISCONNECT */
    if (!aws_mqtt5_client_are_negotiated_settings_valid(client)) {
        return AWS_OP_SUCCESS;
    }

    if (client->config->extended_validation_and_flow_control_options == AWS_MQTT5_EVAFCO_NONE) {
        return AWS_OP_SUCCESS;
    }

    return aws_rate_limiter_token_bucket_take_tokens(
        (struct aws_rate_limiter_token_bucket *)&client->flow_control_state.throughput_throttle,
        THROUGHPUT_TOKENS_PER_IO_MESSAGE_OVERESTIMATE);
}

static int s_apply_publish_tps_flow_control(struct aws_mqtt5_client *client, struct aws_mqtt5_operation *operation) {
    if (client->config->extended_validation_and_flow_control_options == AWS_MQTT5_EVAFCO_NONE) {
        return AWS_OP_SUCCESS;
    }

    if (operation->packet_type != AWS_MQTT5_PT_PUBLISH) {
        return AWS_OP_SUCCESS;
    }

    return aws_rate_limiter_token_bucket_take_tokens(
        (struct aws_rate_limiter_token_bucket *)&client->flow_control_state.publish_throttle, 1);
}

int aws_mqtt5_client_service_operational_state(struct aws_mqtt5_client_operational_state *client_operational_state) {
    struct aws_mqtt5_client *client = client_operational_state->client;
    struct aws_channel_slot *slot = client->slot;
    const struct aws_mqtt5_client_vtable *vtable = client->vtable;
    uint64_t now = (*vtable->get_current_time_fn)();

    /* Should we write data? */
    bool should_service = s_aws_mqtt5_client_should_service_operational_state(client_operational_state, now);
    if (!should_service) {
        return AWS_OP_SUCCESS;
    }

    if (s_apply_throughput_flow_control(client)) {
        return AWS_OP_SUCCESS;
    }

    /* If we're going to write data, we need something to write to */
    struct aws_io_message *io_message = (*vtable->aws_channel_acquire_message_from_pool_fn)(
        slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, AWS_MQTT5_IO_MESSAGE_DEFAULT_LENGTH, vtable->vtable_user_data);
    if (io_message == NULL) {
        return AWS_OP_ERR;
    }

    int operational_error_code = AWS_ERROR_SUCCESS;

    do {
        /* if no current operation, pull one in and setup encode */
        if (client_operational_state->current_operation == NULL) {

            /*
             * Loop through queued operations, discarding ones that fail validation, until we run out or find
             * a good one.  Failing validation against negotiated settings is expected to be a rare event.
             */
            struct aws_mqtt5_operation *next_operation = NULL;
            while (!aws_linked_list_empty(&client_operational_state->queued_operations)) {
                struct aws_linked_list_node *next_operation_node =
                    aws_linked_list_front(&client_operational_state->queued_operations);
                struct aws_mqtt5_operation *operation =
                    AWS_CONTAINER_OF(next_operation_node, struct aws_mqtt5_operation, node);

                /* If this is a publish and we're throttled, just quit out of the loop. */
                if (s_apply_publish_tps_flow_control(client, operation)) {
                    break;
                }

                /* Wait until flow control has passed before actually dequeuing the operation. */
                aws_linked_list_pop_front(&client_operational_state->queued_operations);

                if (!aws_mqtt5_operation_validate_vs_connection_settings(operation, client)) {
                    next_operation = operation;
                    break;
                }

                enum aws_mqtt5_packet_type packet_type = operation->packet_type;
                int validation_error_code = aws_last_error();
                s_complete_operation(client, operation, validation_error_code, AWS_MQTT5_PT_NONE, NULL);

                /* A DISCONNECT packet failing dynamic validation should shut down the whole channel */
                if (packet_type == AWS_MQTT5_PT_DISCONNECT) {
                    operational_error_code = AWS_ERROR_MQTT5_OPERATION_PROCESSING_FAILURE;
                    break;
                }
            }

            if (next_operation != NULL && s_aws_mqtt5_client_set_current_operation(client, next_operation)) {
                operational_error_code = AWS_ERROR_MQTT5_OPERATION_PROCESSING_FAILURE;
                break;
            }
        }

        struct aws_mqtt5_operation *current_operation = client_operational_state->current_operation;
        if (current_operation == NULL) {
            break;
        }

        /* write current operation to message, handle errors */
        enum aws_mqtt5_encoding_result encoding_result =
            aws_mqtt5_encoder_encode_to_buffer(&client->encoder, &io_message->message_data);
        if (encoding_result == AWS_MQTT5_ER_ERROR) {
            operational_error_code = AWS_ERROR_MQTT5_ENCODE_FAILURE;
            break;
        }

        /* if encoding finished:
         *    push to write completion or unacked
         *    clear current
         * else (message full)
         *    break
         */
        if (encoding_result == AWS_MQTT5_ER_FINISHED) {
            aws_mqtt5_client_flow_control_state_on_outbound_operation(client, current_operation);

            if (s_operation_requires_ack(current_operation)) {
                /* track the operation in the unacked data structures by packet id */
                AWS_FATAL_ASSERT(aws_mqtt5_operation_get_packet_id(current_operation) != 0);

                if (aws_hash_table_put(
                        &client_operational_state->unacked_operations_table,
                        aws_mqtt5_operation_get_packet_id_address(current_operation),
                        current_operation,
                        NULL)) {
                    operational_error_code = aws_last_error();
                    break;
                }

                uint32_t ack_timeout_seconds = aws_mqtt5_operation_get_ack_timeout_override(current_operation);
                if (ack_timeout_seconds == 0) {
                    ack_timeout_seconds = client->config->ack_timeout_seconds;
                }

                if (ack_timeout_seconds > 0) {
                    current_operation->ack_timeout_timepoint_ns =
                        now + aws_timestamp_convert(ack_timeout_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
                } else {
                    current_operation->ack_timeout_timepoint_ns = UINT64_MAX;
                }

                if (aws_priority_queue_push_ref(
                        &client_operational_state->operations_by_ack_timeout,
                        (void *)&current_operation,
                        &current_operation->priority_queue_node)) {
                    operational_error_code = aws_last_error();
                    break;
                }

                aws_linked_list_push_back(&client_operational_state->unacked_operations, &current_operation->node);
                aws_mqtt5_client_statistics_change_operation_statistic_state(
                    client, current_operation, AWS_MQTT5_OSS_INCOMPLETE | AWS_MQTT5_OSS_UNACKED);
            } else {
                /* no ack is necessary, just add to socket write completion list */
                aws_linked_list_push_back(
                    &client_operational_state->write_completion_operations, &current_operation->node);

                /*
                 * We special-case setting the ping timeout here.  Other possible places are not appropriate:
                 *
                 *  (1) Socket write completion - this leads to a race condition where our domain socket tests can
                 *  sporadically fail because the PINGRESP is processed before the write completion callback is
                 *  invoked.
                 *
                 *  (2) Enqueue the ping - if the current operation is a large payload over a poor connection, it may
                 *  be an arbitrarily long time before the current operation completes and the ping even has a chance
                 *  to go out, meaning we will trigger a ping time out before it's even sent.
                 *
                 *  Given a reasonable io message size, this is the best place to set the timeout.
                 */
                if (current_operation->packet_type == AWS_MQTT5_PT_PINGREQ) {
                    s_on_pingreq_send(client);
                }
            }

            client->operational_state.current_operation = NULL;
        } else {
            AWS_FATAL_ASSERT(encoding_result == AWS_MQTT5_ER_OUT_OF_ROOM);
            break;
        }

        now = (*vtable->get_current_time_fn)();
        should_service = s_aws_mqtt5_client_should_service_operational_state(client_operational_state, now);
    } while (should_service);

    if (operational_error_code != AWS_ERROR_SUCCESS) {
        aws_mem_release(io_message->allocator, io_message);
        return aws_raise_error(operational_error_code);
    }

    /* It's possible for there to be no data if we serviced operations that failed validation */
    if (io_message->message_data.len == 0) {
        aws_mem_release(io_message->allocator, io_message);
        return AWS_OP_SUCCESS;
    }

    /* send io_message down channel in write direction, handle errors */
    io_message->on_completion = s_aws_mqtt5_on_socket_write_completion;
    io_message->user_data = client_operational_state->client;
    client_operational_state->pending_write_completion = true;

    if ((*vtable->aws_channel_slot_send_message_fn)(
            slot, io_message, AWS_CHANNEL_DIR_WRITE, vtable->vtable_user_data)) {
        client_operational_state->pending_write_completion = false;
        aws_mem_release(io_message->allocator, io_message);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_mqtt5_client_operational_state_handle_ack(
    struct aws_mqtt5_client_operational_state *client_operational_state,
    aws_mqtt5_packet_id_t packet_id,
    enum aws_mqtt5_packet_type packet_type,
    const void *packet_view,
    int error_code) {

    if (packet_type == AWS_MQTT5_PT_PUBACK) {
        aws_mqtt5_client_flow_control_state_on_puback(client_operational_state->client);
    }

    struct aws_hash_element *elem = NULL;
    aws_hash_table_find(&client_operational_state->unacked_operations_table, &packet_id, &elem);

    if (elem == NULL || elem->value == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: received an ACK for an unknown operation with id %d",
            (void *)client_operational_state->client,
            (int)packet_id);
        return;
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_MQTT5_CLIENT,
            "id=%p: Processing ACK with id %d",
            (void *)client_operational_state->client,
            (int)packet_id);
    }

    struct aws_mqtt5_operation *operation = elem->value;

    aws_linked_list_remove(&operation->node);
    aws_hash_table_remove(&client_operational_state->unacked_operations_table, &packet_id, NULL, NULL);

    s_complete_operation(client_operational_state->client, operation, error_code, packet_type, packet_view);
}

bool aws_mqtt5_client_are_negotiated_settings_valid(const struct aws_mqtt5_client *client) {
    return client->current_state == AWS_MCS_CONNECTED || client->current_state == AWS_MCS_CLEAN_DISCONNECT;
}

void aws_mqtt5_client_flow_control_state_init(struct aws_mqtt5_client *client) {
    struct aws_mqtt5_client_flow_control_state *flow_control = &client->flow_control_state;

    struct aws_rate_limiter_token_bucket_options publish_throttle_config = {
        .tokens_per_second = AWS_IOT_CORE_PUBLISH_PER_SECOND_LIMIT,
        .maximum_token_count = AWS_IOT_CORE_PUBLISH_PER_SECOND_LIMIT,
        .initial_token_count = 0,
    };
    aws_rate_limiter_token_bucket_init(&flow_control->publish_throttle, &publish_throttle_config);

    struct aws_rate_limiter_token_bucket_options throughput_throttle_config = {
        .tokens_per_second = AWS_IOT_CORE_THROUGHPUT_LIMIT,
        .maximum_token_count = AWS_IOT_CORE_THROUGHPUT_LIMIT,
        .initial_token_count = 0,
    };
    aws_rate_limiter_token_bucket_init(&flow_control->throughput_throttle, &throughput_throttle_config);
}

void aws_mqtt5_client_flow_control_state_reset(struct aws_mqtt5_client *client) {
    struct aws_mqtt5_client_flow_control_state *flow_control = &client->flow_control_state;

    AWS_FATAL_ASSERT(aws_mqtt5_client_are_negotiated_settings_valid(client));

    flow_control->unacked_publish_token_count = client->negotiated_settings.receive_maximum_from_server;

    aws_rate_limiter_token_bucket_reset(&client->flow_control_state.publish_throttle);
    aws_rate_limiter_token_bucket_reset(&client->flow_control_state.throughput_throttle);
}

void aws_mqtt5_client_flow_control_state_on_puback(struct aws_mqtt5_client *client) {
    struct aws_mqtt5_client_flow_control_state *flow_control = &client->flow_control_state;

    bool was_zero = flow_control->unacked_publish_token_count == 0;
    flow_control->unacked_publish_token_count = aws_min_u32(
        client->negotiated_settings.receive_maximum_from_server, flow_control->unacked_publish_token_count + 1);

    if (was_zero) {
        s_reevaluate_service_task(client);
    }
}

void aws_mqtt5_client_flow_control_state_on_outbound_operation(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation) {
    if (operation->packet_type != AWS_MQTT5_PT_PUBLISH) {
        return;
    }

    const struct aws_mqtt5_packet_publish_view *publish_view = operation->packet_view;
    if (publish_view->qos == AWS_MQTT5_QOS_AT_MOST_ONCE) {
        return;
    }

    struct aws_mqtt5_client_flow_control_state *flow_control = &client->flow_control_state;

    AWS_FATAL_ASSERT(flow_control->unacked_publish_token_count > 0);
    --flow_control->unacked_publish_token_count;
}

uint64_t aws_mqtt5_client_flow_control_state_get_next_operation_service_time(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *next_operation,
    uint64_t now) {

    if (next_operation->packet_type != AWS_MQTT5_PT_PUBLISH) {
        return now;
    }

    /* publish tps check */
    if (client->config->extended_validation_and_flow_control_options != AWS_MQTT5_EVAFCO_NONE) {
        uint64_t publish_wait =
            aws_rate_limiter_token_bucket_compute_wait_for_tokens(&client->flow_control_state.publish_throttle, 1);
        if (publish_wait > 0) {
            return now + publish_wait;
        }
    }

    /* receive maximum check */
    const struct aws_mqtt5_packet_publish_view *publish_view = next_operation->packet_view;
    if (publish_view->qos == AWS_MQTT5_QOS_AT_MOST_ONCE) {
        return now;
    }

    if (client->flow_control_state.unacked_publish_token_count > 0) {
        return now;
    }

    return 0;
}

void aws_mqtt5_client_statistics_change_operation_statistic_state(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    enum aws_mqtt5_operation_statistic_state_flags new_state_flags) {
    enum aws_mqtt5_packet_type packet_type = operation->packet_type;
    if (packet_type != AWS_MQTT5_PT_PUBLISH && packet_type != AWS_MQTT5_PT_SUBSCRIBE &&
        packet_type != AWS_MQTT5_PT_UNSUBSCRIBE) {
        return;
    }

    if (operation->packet_size == 0) {
        if (aws_mqtt5_packet_view_get_encoded_size(packet_type, operation->packet_view, &operation->packet_size)) {
            return;
        }
    }

    AWS_FATAL_ASSERT(operation->packet_size > 0);
    uint64_t packet_size = (uint64_t)operation->packet_size;

    enum aws_mqtt5_operation_statistic_state_flags old_state_flags = operation->statistic_state_flags;
    if (new_state_flags == old_state_flags) {
        return;
    }

    struct aws_mqtt5_client_operation_statistics_impl *stats = &client->operation_statistics_impl;

    if ((old_state_flags & AWS_MQTT5_OSS_INCOMPLETE) != (new_state_flags & AWS_MQTT5_OSS_INCOMPLETE)) {
        if ((new_state_flags & AWS_MQTT5_OSS_INCOMPLETE) != 0) {
            aws_atomic_fetch_add(&stats->incomplete_operation_count_atomic, 1);
            aws_atomic_fetch_add(&stats->incomplete_operation_size_atomic, (size_t)packet_size);
        } else {
            aws_atomic_fetch_sub(&stats->incomplete_operation_count_atomic, 1);
            aws_atomic_fetch_sub(&stats->incomplete_operation_size_atomic, (size_t)packet_size);
        }
    }

    if ((old_state_flags & AWS_MQTT5_OSS_UNACKED) != (new_state_flags & AWS_MQTT5_OSS_UNACKED)) {
        if ((new_state_flags & AWS_MQTT5_OSS_UNACKED) != 0) {
            aws_atomic_fetch_add(&stats->unacked_operation_count_atomic, 1);
            aws_atomic_fetch_add(&stats->unacked_operation_size_atomic, (size_t)packet_size);
        } else {
            aws_atomic_fetch_sub(&stats->unacked_operation_count_atomic, 1);
            aws_atomic_fetch_sub(&stats->unacked_operation_size_atomic, (size_t)packet_size);
        }
    }

    operation->statistic_state_flags = new_state_flags;

    if (client->vtable != NULL && client->vtable->on_client_statistics_changed_callback_fn != NULL) {
        (*client->vtable->on_client_statistics_changed_callback_fn)(
            client, operation, client->vtable->vtable_user_data);
    }
}

void aws_mqtt5_client_get_stats(struct aws_mqtt5_client *client, struct aws_mqtt5_client_operation_statistics *stats) {
    stats->incomplete_operation_count =
        (uint64_t)aws_atomic_load_int(&client->operation_statistics_impl.incomplete_operation_count_atomic);
    stats->incomplete_operation_size =
        (uint64_t)aws_atomic_load_int(&client->operation_statistics_impl.incomplete_operation_size_atomic);
    stats->unacked_operation_count =
        (uint64_t)aws_atomic_load_int(&client->operation_statistics_impl.unacked_operation_count_atomic);
    stats->unacked_operation_size =
        (uint64_t)aws_atomic_load_int(&client->operation_statistics_impl.unacked_operation_size_atomic);
}

bool aws_mqtt5_client_reset_connection(struct aws_mqtt5_client *client) {
    AWS_FATAL_ASSERT(aws_event_loop_thread_is_callers_thread(client->loop));

    client->current_reconnect_delay_ms = client->config->min_reconnect_delay_ms;

    switch (client->current_state) {
        case AWS_MCS_MQTT_CONNECT:
        case AWS_MCS_CONNECTED:
            s_aws_mqtt5_client_shutdown_channel(client, AWS_ERROR_MQTT_CONNECTION_RESET_FOR_ADAPTER_CONNECT);
            return true;

        case AWS_MCS_CONNECTING:
            client->should_reset_connection = true;
            return true;

        default:
            break;
    }

    return false;
}
