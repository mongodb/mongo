/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/client_impl.h>

#include <aws/mqtt/private/packets.h>
#include <aws/mqtt/private/topic_tree.h>

#include <aws/io/logging.h>

#include <aws/common/clock.h>
#include <aws/common/math.h>
#include <aws/common/task_scheduler.h>

#include <inttypes.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204)
#endif

/*******************************************************************************
 * Static Helper functions
 ******************************************************************************/

/* Caches the socket write time for ping scheduling purposes */
static void s_update_next_ping_time(struct aws_mqtt_client_connection_311_impl *connection) {
    if (connection->slot != NULL && connection->slot->channel != NULL) {
        aws_channel_current_clock_time(connection->slot->channel, &connection->next_ping_time);
        aws_add_u64_checked(connection->next_ping_time, connection->keep_alive_time_ns, &connection->next_ping_time);
    }
}

/* Caches the request send time. The `request_send_timestamp` will be used to push off ping request on request complete.
 */
static void s_update_request_send_time(struct aws_mqtt_request *request) {
    if (request->connection != NULL && request->connection->slot != NULL &&
        request->connection->slot->channel != NULL) {
        aws_channel_current_clock_time(request->connection->slot->channel, &request->request_send_timestamp);
    }
}

/* push off next ping time on ack received to last_request_send_timestamp_ns + keep_alive_time_ns
 * The function must be called in critical section. */
static void s_pushoff_next_ping_time(
    struct aws_mqtt_client_connection_311_impl *connection,
    uint64_t last_request_send_timestamp_ns) {
    ASSERT_SYNCED_DATA_LOCK_HELD(connection);
    aws_add_u64_checked(
        last_request_send_timestamp_ns, connection->keep_alive_time_ns, &last_request_send_timestamp_ns);
    if (last_request_send_timestamp_ns > connection->next_ping_time) {
        connection->next_ping_time = last_request_send_timestamp_ns;
    }
}

/*******************************************************************************
 * Packet State Machine
 ******************************************************************************/

static int s_packet_handler_default(struct aws_byte_cursor message_cursor, void *user_data) {
    (void)message_cursor;

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_ERROR(AWS_LS_MQTT_CLIENT, "id=%p: Unhandled packet type received", (void *)connection);
    return aws_raise_error(AWS_ERROR_MQTT_INVALID_PACKET_TYPE);
}

static void s_on_time_to_ping(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status);
static void s_schedule_ping(struct aws_mqtt_client_connection_311_impl *connection) {
    aws_channel_task_init(&connection->ping_task, s_on_time_to_ping, connection, "mqtt_ping");

    uint64_t now = 0;
    aws_channel_current_clock_time(connection->slot->channel, &now);

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT, "id=%p: Scheduling PING task. current timestamp is %" PRIu64, (void *)connection, now);

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: The next PING task will be run at timestamp %" PRIu64,
        (void *)connection,
        connection->next_ping_time);

    aws_channel_schedule_task_future(connection->slot->channel, &connection->ping_task, connection->next_ping_time);
}

static void s_on_time_to_ping(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    (void)channel_task;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_mqtt_client_connection_311_impl *connection = arg;

        uint64_t now = 0;
        aws_channel_current_clock_time(connection->slot->channel, &now);
        if (now >= connection->next_ping_time) {
            s_update_next_ping_time(connection);
            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Sending PING", (void *)connection);
            aws_mqtt_client_connection_ping(connection);
        } else {

            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Skipped sending PING because scheduled ping time %" PRIu64
                " has not elapsed yet. Current time is %" PRIu64
                ". Rescheduling ping to run at the scheduled ping time...",
                (void *)connection,
                connection->next_ping_time,
                now);
        }
        s_schedule_ping(connection);
    }
}

static int s_validate_received_packet_type(
    struct aws_mqtt_client_connection_311_impl *connection,
    enum aws_mqtt_packet_type packet_type) {
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        /* [MQTT-3.2.0-1] The first packet sent from the Server to the Client MUST be a CONNACK Packet */
        if (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_CONNECTING &&
            packet_type != AWS_MQTT_PACKET_CONNACK) {
            mqtt_connection_unlock_synced_data(connection);
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: First message received from the server was not a CONNACK. Terminating connection.",
                (void *)connection);
            return aws_raise_error(AWS_ERROR_MQTT_PROTOCOL_ERROR);
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (AWS_UNLIKELY(packet_type > AWS_MQTT_PACKET_DISCONNECT || packet_type < AWS_MQTT_PACKET_CONNECT)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Invalid packet type received %d. Terminating connection.",
            (void *)connection,
            packet_type);
        return aws_raise_error(AWS_ERROR_MQTT_INVALID_PACKET_TYPE);
    }

    /* Handle the packet */
    return AWS_OP_SUCCESS;
}

static int s_packet_handler_connack(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: CONNACK received", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_CONNACK)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_connack connack;
    if (aws_mqtt_packet_connack_decode(&message_cursor, &connack)) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT, "id=%p: error %d parsing CONNACK packet", (void *)connection, aws_last_error());

        return AWS_OP_ERR;
    }
    bool was_reconnecting;
    struct aws_linked_list requests;
    aws_linked_list_init(&requests);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        /* User requested disconnect, don't do anything */
        if (connection->synced_data.state >= AWS_MQTT_CLIENT_STATE_DISCONNECTING) {
            mqtt_connection_unlock_synced_data(connection);
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT, "id=%p: User has requested disconnect, dropping connection", (void *)connection);
            return AWS_OP_SUCCESS;
        }

        was_reconnecting = connection->synced_data.state == AWS_MQTT_CLIENT_STATE_RECONNECTING;
        if (connack.connect_return_code == AWS_MQTT_CONNECT_ACCEPTED) {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_CLIENT,
                "id=%p: connection was accepted, switch state from %d to CONNECTED.",
                (void *)connection,
                (int)connection->synced_data.state);
            /* Don't change the state if it's not ACCEPTED by broker */
            mqtt_connection_set_state(connection, AWS_MQTT_CLIENT_STATE_CONNECTED);
            aws_linked_list_swap_contents(&connection->synced_data.pending_requests_list, &requests);
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */
    connection->connection_count++;

    uint64_t now = 0;
    aws_high_res_clock_get_ticks(&now);

    if (connack.connect_return_code == AWS_MQTT_CONNECT_ACCEPTED) {

        /*
         * This was a successful MQTT connection establishment, record the time so that channel shutdown
         * can make a good decision about reconnect backoff reset.
         */
        connection->reconnect_timeouts.channel_successful_connack_timestamp_ns = now;

        /* If successfully connected, schedule all pending tasks */
        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT, "id=%p: connection was accepted processing offline requests.", (void *)connection);

        if (!aws_linked_list_empty(&requests)) {

            struct aws_linked_list_node *current = aws_linked_list_front(&requests);
            const struct aws_linked_list_node *end = aws_linked_list_end(&requests);

            do {
                struct aws_mqtt_request *request = AWS_CONTAINER_OF(current, struct aws_mqtt_request, list_node);
                AWS_LOGF_TRACE(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: processing offline request %" PRIu16,
                    (void *)connection,
                    request->packet_id);
                aws_channel_schedule_task_now(connection->slot->channel, &request->outgoing_task);
                current = current->next;
            } while (current != end);
        }
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_CLIENT,
            "id=%p: invalid connect return code %d, disconnecting",
            (void *)connection,
            connack.connect_return_code);
        /* If error code returned, disconnect, on_completed will be invoked from shutdown process */
        aws_channel_shutdown(connection->slot->channel, AWS_ERROR_MQTT_PROTOCOL_ERROR);

        return AWS_OP_SUCCESS;
    }

    /* It is possible for a connection to complete, and a hangup to occur before the
     * CONNECT/CONNACK cycle completes. In that case, we must deliver on_connection_complete
     * on the first successful CONNACK or user code will never think it's connected */
    if (was_reconnecting && connection->connection_count > 1) {

        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT,
            "id=%p: connection is a resumed connection, invoking on_resumed callback",
            (void *)connection);

        MQTT_CLIENT_CALL_CALLBACK_ARGS(connection, on_resumed, connack.connect_return_code, connack.session_present);
    } else {

        aws_create_reconnect_task(connection);

        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT,
            "id=%p: connection is a new connection, invoking on_connection_complete callback",
            (void *)connection);
        MQTT_CLIENT_CALL_CALLBACK_ARGS(
            connection, on_connection_complete, AWS_OP_SUCCESS, connack.connect_return_code, connack.session_present);
    }

    /*
     * The on_connection_success would get triggered on the successful CONNACK. It invoked with both the first connect
     * attempt and reconnection attempt as Mqtt5 does not have on_resume callback for reconnection.
     */
    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: received a successful CONNACK, invoking on_connection_success callback",
        (void *)connection);
    MQTT_CLIENT_CALL_CALLBACK_ARGS(
        connection, on_connection_success, connack.connect_return_code, connack.session_present);

    aws_mqtt311_callback_set_manager_on_connection_success(
        &connection->callback_manager, connack.connect_return_code, connack.session_present);

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: connection callback completed", (void *)connection);

    s_update_next_ping_time(connection);
    s_schedule_ping(connection);
    return AWS_OP_SUCCESS;
}

static int s_packet_handler_publish(struct aws_byte_cursor message_cursor, void *user_data) {
    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: PUBLISH received", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_PUBLISH)) {
        return AWS_OP_ERR;
    }

    /* TODO: need to handle the QoS 2 message to avoid processing the message a second time */
    struct aws_mqtt_packet_publish publish;
    if (aws_mqtt_packet_publish_decode(&message_cursor, &publish)) {
        return AWS_OP_ERR;
    }

    aws_mqtt_topic_tree_publish(&connection->thread_data.subscriptions, &publish);

    bool dup = aws_mqtt_packet_publish_get_dup(&publish);
    enum aws_mqtt_qos qos = aws_mqtt_packet_publish_get_qos(&publish);
    bool retain = aws_mqtt_packet_publish_get_retain(&publish);

    MQTT_CLIENT_CALL_CALLBACK_ARGS(connection, on_any_publish, &publish.topic_name, &publish.payload, dup, qos, retain);

    aws_mqtt311_callback_set_manager_on_publish_received(
        &connection->callback_manager, &publish.topic_name, &publish.payload, dup, qos, retain);

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: publish received with msg id=%" PRIu16 " dup=%d qos=%d retain=%d payload-size=%zu topic=" PRInSTR,
        (void *)connection,
        publish.packet_identifier,
        dup,
        qos,
        retain,
        publish.payload.len,
        AWS_BYTE_CURSOR_PRI(publish.topic_name));
    struct aws_mqtt_packet_ack puback;
    AWS_ZERO_STRUCT(puback);

    /* Switch on QoS flags (bits 1 & 2) */
    switch (qos) {
        case AWS_MQTT_QOS_AT_MOST_ONCE:
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT, "id=%p: received publish QOS is 0, not sending puback", (void *)connection);
            /* No more communication necessary */
            break;
        case AWS_MQTT_QOS_AT_LEAST_ONCE:
            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: received publish QOS is 1, sending puback", (void *)connection);
            aws_mqtt_packet_puback_init(&puback, publish.packet_identifier);
            break;
        case AWS_MQTT_QOS_EXACTLY_ONCE:
            AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: received publish QOS is 2, sending pubrec", (void *)connection);
            aws_mqtt_packet_pubrec_init(&puback, publish.packet_identifier);
            break;
        default:
            /* Impossible to hit this branch. QoS value is checked when decoding */
            AWS_FATAL_ASSERT(0);
            break;
    }

    if (puback.packet_identifier) {
        struct aws_io_message *message = mqtt_get_message_for_packet(connection, &puback.fixed_header);
        if (!message) {
            return AWS_OP_ERR;
        }

        if (aws_mqtt_packet_ack_encode(&message->message_data, &puback)) {
            aws_mem_release(message->allocator, message);
            return AWS_OP_ERR;
        }

        if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
            aws_mem_release(message->allocator, message);
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_packet_handler_puback(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a PUBACK", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_PUBACK)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_ack ack;
    if (aws_mqtt_packet_ack_decode(&message_cursor, &ack)) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT, "id=%p: received ack for message id %" PRIu16, (void *)connection, ack.packet_identifier);

    mqtt_request_complete(connection, AWS_ERROR_SUCCESS, ack.packet_identifier);

    return AWS_OP_SUCCESS;
}

static int s_packet_handler_suback(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a SUBACK", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_SUBACK)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_suback suback;
    if (aws_mqtt_packet_suback_init(&suback, connection->allocator, 0 /* fake packet_id */)) {
        return AWS_OP_ERR;
    }

    if (aws_mqtt_packet_suback_decode(&message_cursor, &suback)) {
        goto error;
    }

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: received suback for message id %" PRIu16,
        (void *)connection,
        suback.packet_identifier);

    struct aws_mqtt_request *request = NULL;

    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        struct aws_hash_element *elem = NULL;
        aws_hash_table_find(&connection->synced_data.outstanding_requests_table, &suback.packet_identifier, &elem);
        if (elem != NULL) {
            request = elem->value;
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (request == NULL) {
        /* no corresponding request found */
        goto done;
    }

    struct subscribe_task_arg *task_arg = request->on_complete_ud;
    size_t request_topics_len = aws_array_list_length(&task_arg->topics);
    size_t suback_return_code_len = aws_array_list_length(&suback.return_codes);
    if (request_topics_len != suback_return_code_len) {
        goto error;
    }
    size_t num_filters = aws_array_list_length(&suback.return_codes);
    for (size_t i = 0; i < num_filters; ++i) {

        uint8_t return_code = 0;
        struct subscribe_task_topic *topic = NULL;
        aws_array_list_get_at(&suback.return_codes, (void *)&return_code, i);
        aws_array_list_get_at(&task_arg->topics, &topic, i);
        topic->request.qos = return_code;
    }

done:
    mqtt_request_complete(connection, AWS_ERROR_SUCCESS, suback.packet_identifier);
    aws_mqtt_packet_suback_clean_up(&suback);
    return AWS_OP_SUCCESS;
error:
    aws_mqtt_packet_suback_clean_up(&suback);
    return AWS_OP_ERR;
}

static int s_packet_handler_unsuback(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a UNSUBACK", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_UNSUBACK)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_ack ack;
    if (aws_mqtt_packet_ack_decode(&message_cursor, &ack)) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT, "id=%p: received ack for message id %" PRIu16, (void *)connection, ack.packet_identifier);

    mqtt_request_complete(connection, AWS_ERROR_SUCCESS, ack.packet_identifier);
    return AWS_OP_SUCCESS;
}

static int s_packet_handler_pubrec(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a PUBREC", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_PUBREC)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_ack ack;
    if (aws_mqtt_packet_ack_decode(&message_cursor, &ack)) {
        return AWS_OP_ERR;
    }

    /* TODO: When sending PUBLISH with QoS 2, we should be storing the data until this packet is received, at which
     * point we may discard it. */

    /* Send PUBREL */
    aws_mqtt_packet_pubrel_init(&ack, ack.packet_identifier);
    struct aws_io_message *message = mqtt_get_message_for_packet(connection, &ack.fixed_header);
    if (!message) {
        return AWS_OP_ERR;
    }

    if (aws_mqtt_packet_ack_encode(&message->message_data, &ack)) {
        goto on_error;
    }

    if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    if (message) {
        aws_mem_release(message->allocator, message);
    }

    return AWS_OP_ERR;
}

static int s_packet_handler_pubrel(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a PUBREL", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_PUBREL)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_ack ack;
    if (aws_mqtt_packet_ack_decode(&message_cursor, &ack)) {
        return AWS_OP_ERR;
    }

    /* Send PUBCOMP */
    aws_mqtt_packet_pubcomp_init(&ack, ack.packet_identifier);
    struct aws_io_message *message = mqtt_get_message_for_packet(connection, &ack.fixed_header);
    if (!message) {
        return AWS_OP_ERR;
    }

    if (aws_mqtt_packet_ack_encode(&message->message_data, &ack)) {
        goto on_error;
    }

    if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    if (message) {
        aws_mem_release(message->allocator, message);
    }

    return AWS_OP_ERR;
}

static int s_packet_handler_pubcomp(struct aws_byte_cursor message_cursor, void *user_data) {

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: received a PUBCOMP", (void *)connection);
    if (s_validate_received_packet_type(connection, AWS_MQTT_PACKET_PUBCOMP)) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_packet_ack ack;
    if (aws_mqtt_packet_ack_decode(&message_cursor, &ack)) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_CLIENT, "id=%p: received ack for message id %" PRIu16, (void *)connection, ack.packet_identifier);

    mqtt_request_complete(connection, AWS_ERROR_SUCCESS, ack.packet_identifier);
    return AWS_OP_SUCCESS;
}

static int s_packet_handler_pingresp(struct aws_byte_cursor message_cursor, void *user_data) {

    (void)message_cursor;

    struct aws_mqtt_client_connection_311_impl *connection = user_data;
    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "id=%p: PINGRESP received", (void *)connection);

    connection->thread_data.waiting_on_ping_response = false;

    return AWS_OP_SUCCESS;
}

/* Bake up a big ol' function table just like Gramma used to make */
static struct aws_mqtt_client_connection_packet_handlers s_default_packet_handlers = {
    .handlers_by_packet_type = {
        [AWS_MQTT_PACKET_CONNECT] = &s_packet_handler_default,
        [AWS_MQTT_PACKET_CONNACK] = &s_packet_handler_connack,
        [AWS_MQTT_PACKET_PUBLISH] = &s_packet_handler_publish,
        [AWS_MQTT_PACKET_PUBACK] = &s_packet_handler_puback,
        [AWS_MQTT_PACKET_PUBREC] = &s_packet_handler_pubrec,
        [AWS_MQTT_PACKET_PUBREL] = &s_packet_handler_pubrel,
        [AWS_MQTT_PACKET_PUBCOMP] = &s_packet_handler_pubcomp,
        [AWS_MQTT_PACKET_SUBSCRIBE] = &s_packet_handler_default,
        [AWS_MQTT_PACKET_SUBACK] = &s_packet_handler_suback,
        [AWS_MQTT_PACKET_UNSUBSCRIBE] = &s_packet_handler_default,
        [AWS_MQTT_PACKET_UNSUBACK] = &s_packet_handler_unsuback,
        [AWS_MQTT_PACKET_PINGREQ] = &s_packet_handler_default,
        [AWS_MQTT_PACKET_PINGRESP] = &s_packet_handler_pingresp,
        [AWS_MQTT_PACKET_DISCONNECT] = &s_packet_handler_default,
    }};

const struct aws_mqtt_client_connection_packet_handlers *aws_mqtt311_get_default_packet_handlers(void) {
    return &s_default_packet_handlers;
}

/*******************************************************************************
 * Channel Handler
 ******************************************************************************/

/**
 * Handles incoming messages from the server.
 */
static int s_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    struct aws_mqtt_client_connection_311_impl *connection = handler->impl;

    if (message->message_type != AWS_IO_MESSAGE_APPLICATION_DATA || message->message_data.len < 1) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: precessing read message of size %zu",
        (void *)connection,
        message->message_data.len);

    /* This cursor will be updated as we read through the message. */
    struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&message->message_data);

    int result = aws_mqtt311_decoder_on_bytes_received(&connection->thread_data.decoder, message_cursor);

    if (result == AWS_OP_SUCCESS) {
        /* Do cleanup */
        size_t message_data_length = message->message_data.len;
        aws_mem_release(message->allocator, message);
        aws_channel_slot_increment_read_window(slot, message_data_length);
    } else {
        aws_channel_shutdown(connection->slot->channel, aws_last_error());
    }

    return result;
}

static int s_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {

    struct aws_mqtt_client_connection_311_impl *connection = handler->impl;

    if (dir == AWS_CHANNEL_DIR_WRITE) {
        /* On closing write direction, send out disconnect packet before closing connection. */

        if (!free_scarce_resources_immediately) {

            if (error_code == AWS_OP_SUCCESS) {
                AWS_LOGF_INFO(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: sending disconnect message as part of graceful shutdown.",
                    (void *)connection);
                /* On clean shutdown, send the disconnect message */
                struct aws_mqtt_packet_connection disconnect;
                aws_mqtt_packet_disconnect_init(&disconnect);

                struct aws_io_message *message = mqtt_get_message_for_packet(connection, &disconnect.fixed_header);
                if (!message) {
                    goto done;
                }

                if (aws_mqtt_packet_connection_encode(&message->message_data, &disconnect)) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT_CLIENT,
                        "id=%p: failed to encode courteous disconnect io message",
                        (void *)connection);
                    aws_mem_release(message->allocator, message);
                    goto done;
                }

                if (aws_channel_slot_send_message(slot, message, AWS_CHANNEL_DIR_WRITE)) {
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT_CLIENT,
                        "id=%p: failed to send courteous disconnect io message",
                        (void *)connection);
                    aws_mem_release(message->allocator, message);
                    goto done;
                }
            }
        }
    }

done:
    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resources_immediately);
}

static size_t s_initial_window_size(struct aws_channel_handler *handler) {

    (void)handler;

    return SIZE_MAX;
}

static void s_destroy(struct aws_channel_handler *handler) {

    struct aws_mqtt_client_connection_311_impl *connection = handler->impl;
    (void)connection;
}

static size_t s_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return 0;
}

struct aws_channel_handler_vtable *aws_mqtt_get_client_channel_vtable(void) {

    static struct aws_channel_handler_vtable s_vtable = {
        .process_read_message = &s_process_read_message,
        .process_write_message = NULL,
        .increment_read_window = NULL,
        .shutdown = &s_shutdown,
        .initial_window_size = &s_initial_window_size,
        .message_overhead = &s_message_overhead,
        .destroy = &s_destroy,
    };

    return &s_vtable;
}

/*******************************************************************************
 * Helpers
 ******************************************************************************/

struct aws_io_message *mqtt_get_message_for_packet(
    struct aws_mqtt_client_connection_311_impl *connection,
    struct aws_mqtt_fixed_header *header) {

    const size_t required_length = 3 + header->remaining_length;

    struct aws_io_message *message = aws_channel_acquire_message_from_pool(
        connection->slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, required_length);

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: Acquiring memory from pool of required_length %zu",
        (void *)connection,
        required_length);

    return message;
}

/*******************************************************************************
 * Requests
 ******************************************************************************/

/* Send the request */
static void s_request_outgoing_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {

    struct aws_mqtt_request *request = arg;
    struct aws_mqtt_client_connection_311_impl *connection = request->connection;

    if (status == AWS_TASK_STATUS_CANCELED) {
        /* Connection lost before the request ever get send, check the request needs to be retried or not */
        if (request->retryable) {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_CLIENT,
                "static: task id %p, was canceled due to the channel shutting down. Request for packet id "
                "%" PRIu16 ". will be retried",
                (void *)task,
                request->packet_id);

            /* put it into the offline queue. */
            { /* BEGIN CRITICAL SECTION */
                mqtt_connection_lock_synced_data(connection);

                /* Set the status as incomplete */
                aws_mqtt_connection_statistics_change_operation_statistic_state(
                    connection, request, AWS_MQTT_OSS_INCOMPLETE);

                aws_linked_list_push_back(&connection->synced_data.pending_requests_list, &request->list_node);

                mqtt_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */
        } else {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_CLIENT,
                "static: task id %p, was canceled due to the channel shutting down. Request for packet id "
                "%" PRIu16 ". will NOT be retried, will be cancelled",
                (void *)task,
                request->packet_id);

            /* Fire the callback and clean up the memory, as the connection get destroyed. */
            if (request->on_complete) {
                request->on_complete(
                    &connection->base, request->packet_id, AWS_ERROR_MQTT_NOT_CONNECTED, request->on_complete_ud);
            }

            { /* BEGIN CRITICAL SECTION */
                mqtt_connection_lock_synced_data(connection);

                /* Cancel the request in the operation statistics */
                aws_mqtt_connection_statistics_change_operation_statistic_state(connection, request, AWS_MQTT_OSS_NONE);

                aws_hash_table_remove(
                    &connection->synced_data.outstanding_requests_table, &request->packet_id, NULL, NULL);
                aws_memory_pool_release(&connection->synced_data.requests_pool, request);
                mqtt_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */
        }
        return;
    }

    /* Send the request */
    enum aws_mqtt_client_request_state state =
        request->send_request(request->packet_id, !request->initiated, request->send_request_ud);
    /* Update the request send time.*/
    s_update_request_send_time(request);
    request->initiated = true;
    int error_code = AWS_ERROR_SUCCESS;
    switch (state) {
        case AWS_MQTT_CLIENT_REQUEST_ERROR:
            error_code = aws_last_error();
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: sending request %" PRIu16 " failed with error %d.",
                (void *)request->connection,
                request->packet_id,
                error_code);
            /* fall-thru */

        case AWS_MQTT_CLIENT_REQUEST_COMPLETE:
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: sending request %" PRIu16 " complete, invoking on_complete callback.",
                (void *)request->connection,
                request->packet_id);

            /* If the send_request function reports the request is complete,
             * remove from the hash table and call the callback. */
            if (request->on_complete) {
                request->on_complete(&connection->base, request->packet_id, error_code, request->on_complete_ud);
            }

            { /* BEGIN CRITICAL SECTION */
                mqtt_connection_lock_synced_data(connection);

                /* Set the request as complete in the operation statistics */
                aws_mqtt_connection_statistics_change_operation_statistic_state(
                    request->connection, request, AWS_MQTT_OSS_NONE);

                aws_hash_table_remove(
                    &connection->synced_data.outstanding_requests_table, &request->packet_id, NULL, NULL);
                aws_memory_pool_release(&connection->synced_data.requests_pool, request);
                mqtt_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */
            break;

        case AWS_MQTT_CLIENT_REQUEST_ONGOING:
            AWS_LOGF_TRACE(
                AWS_LS_MQTT_CLIENT,
                "id=%p: request %" PRIu16 " sent, but waiting on an acknowledgement from peer.",
                (void *)request->connection,
                request->packet_id);

            { /* BEGIN CRITICAL SECTION */
                mqtt_connection_lock_synced_data(connection);

                /* Set the request as incomplete and un-acked in the operation statistics */
                aws_mqtt_connection_statistics_change_operation_statistic_state(
                    request->connection, request, AWS_MQTT_OSS_INCOMPLETE | AWS_MQTT_OSS_UNACKED);

                mqtt_connection_unlock_synced_data(connection);
            } /* END CRITICAL SECTION */

            /* Put the request into the ongoing list */
            aws_linked_list_push_back(&connection->thread_data.ongoing_requests_list, &request->list_node);
            break;
    }
}

uint16_t mqtt_create_request(
    struct aws_mqtt_client_connection_311_impl *connection,
    aws_mqtt_send_request_fn *send_request,
    void *send_request_ud,
    aws_mqtt_op_complete_fn *on_complete,
    void *on_complete_ud,
    bool noRetry,
    uint64_t packet_size) {

    AWS_ASSERT(connection);
    AWS_ASSERT(send_request);
    struct aws_mqtt_request *next_request = NULL;
    bool should_schedule_task = false;
    struct aws_channel *channel = NULL;
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        if (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_DISCONNECTING) {
            mqtt_connection_unlock_synced_data(connection);
            /* User requested disconnecting, ensure no new requests are made until the channel finished shutting
             * down. */
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Disconnect requested, stop creating any new request until disconnect process finishes.",
                (void *)connection);
            aws_raise_error(AWS_ERROR_MQTT_CONNECTION_DISCONNECTING);
            return 0;
        }

        if (noRetry && connection->synced_data.state != AWS_MQTT_CLIENT_STATE_CONNECTED) {
            mqtt_connection_unlock_synced_data(connection);
            /* Not offline queueing QoS 0 publish or PINGREQ. Fail the call. */
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_CLIENT,
                "id=%p: Not currently connected. No offline queueing for QoS 0 publish or pingreq.",
                (void *)connection);
            aws_raise_error(AWS_ERROR_MQTT_NOT_CONNECTED);
            return 0;
        }
        /**
         * Find a free packet ID.
         * QoS 0 PUBLISH packets don't actually need an ID on the wire,
         * but we assign them internally anyway just so everything has a unique ID.
         *
         * Yes, this is an O(N) search.
         * We remember the last ID we assigned, so it's O(1) in the common case.
         * But it's theoretically possible to reach O(N) where N is just above 64000
         * if the user is letting a ton of un-ack'd messages queue up
         */
        uint16_t search_start = connection->synced_data.packet_id;
        struct aws_hash_element *elem = NULL;
        while (true) {
            /* Increment ID, watch out for overflow, ID cannot be 0 */
            if (connection->synced_data.packet_id == UINT16_MAX) {
                connection->synced_data.packet_id = 1;
            } else {
                connection->synced_data.packet_id++;
            }

            /* Is there already an outstanding request using this ID? */
            aws_hash_table_find(
                &connection->synced_data.outstanding_requests_table, &connection->synced_data.packet_id, &elem);

            if (elem == NULL) {
                /* Found a free ID! Break out of loop */
                break;
            } else if (connection->synced_data.packet_id == search_start) {
                /* Every ID is taken */
                mqtt_connection_unlock_synced_data(connection);
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_CLIENT,
                    "id=%p: Queue is full. No more packet IDs are available at this time.",
                    (void *)connection);
                aws_raise_error(AWS_ERROR_MQTT_QUEUE_FULL);
                return 0;
            }
        }

        next_request = aws_memory_pool_acquire(&connection->synced_data.requests_pool);
        if (!next_request) {
            mqtt_connection_unlock_synced_data(connection);
            return 0;
        }
        memset(next_request, 0, sizeof(struct aws_mqtt_request));

        next_request->packet_id = connection->synced_data.packet_id;

        if (aws_hash_table_put(
                &connection->synced_data.outstanding_requests_table, &next_request->packet_id, next_request, NULL)) {
            /* failed to put the next request into the table */
            aws_memory_pool_release(&connection->synced_data.requests_pool, next_request);
            mqtt_connection_unlock_synced_data(connection);
            return 0;
        }
        /* Store the request by packet_id */
        next_request->allocator = connection->allocator;
        next_request->connection = connection;
        next_request->initiated = false;
        next_request->retryable = !noRetry;
        next_request->send_request = send_request;
        next_request->send_request_ud = send_request_ud;
        next_request->on_complete = on_complete;
        next_request->on_complete_ud = on_complete_ud;
        next_request->packet_size = packet_size;
        aws_channel_task_init(
            &next_request->outgoing_task, s_request_outgoing_task, next_request, "mqtt_outgoing_request_task");
        if (connection->synced_data.state != AWS_MQTT_CLIENT_STATE_CONNECTED) {
            aws_linked_list_push_back(&connection->synced_data.pending_requests_list, &next_request->list_node);
        } else {
            AWS_ASSERT(connection->slot);
            AWS_ASSERT(connection->slot->channel);
            should_schedule_task = true;
            channel = connection->slot->channel;
            /* keep the channel alive until the task is scheduled */
            aws_channel_acquire_hold(channel);
        }

        if (next_request && next_request->packet_size > 0) {
            /* Set the status as incomplete */
            aws_mqtt_connection_statistics_change_operation_statistic_state(
                next_request->connection, next_request, AWS_MQTT_OSS_INCOMPLETE);
        }

        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (should_schedule_task) {
        AWS_LOGF_TRACE(
            AWS_LS_MQTT_CLIENT,
            "id=%p: Currently not in the event-loop thread, scheduling a task to send message id %" PRIu16 ".",
            (void *)connection,
            next_request->packet_id);
        aws_channel_schedule_task_now(channel, &next_request->outgoing_task);
        /* release the refcount we hold with the protection of lock */
        aws_channel_release_hold(channel);
    }

    return next_request->packet_id;
}

void mqtt_request_complete(struct aws_mqtt_client_connection_311_impl *connection, int error_code, uint16_t packet_id) {

    AWS_LOGF_TRACE(
        AWS_LS_MQTT_CLIENT,
        "id=%p: message id %" PRIu16 " completed with error code %d, removing from outstanding requests list.",
        (void *)connection,
        packet_id,
        error_code);

    bool found_request = false;
    aws_mqtt_op_complete_fn *on_complete = NULL;
    void *on_complete_ud = NULL;

    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        struct aws_hash_element *elem = NULL;
        aws_hash_table_find(&connection->synced_data.outstanding_requests_table, &packet_id, &elem);
        if (elem != NULL) {
            found_request = true;

            struct aws_mqtt_request *request = elem->value;
            on_complete = request->on_complete;
            on_complete_ud = request->on_complete_ud;

            /* Set the status as complete */
            aws_mqtt_connection_statistics_change_operation_statistic_state(
                request->connection, request, AWS_MQTT_OSS_NONE);

            if (error_code == AWS_OP_SUCCESS) {
                s_pushoff_next_ping_time(connection, request->request_send_timestamp);
            }
            /* clean up request resources */
            aws_hash_table_remove_element(&connection->synced_data.outstanding_requests_table, elem);
            /* remove the request from the list, which is thread_data.ongoing_requests_list */
            aws_linked_list_remove(&request->list_node);
            aws_memory_pool_release(&connection->synced_data.requests_pool, request);
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (!found_request) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_CLIENT,
            "id=%p: received completion for message id %" PRIu16
            " but no outstanding request exists.  Assuming this is an ack of a resend when the first request has "
            "already completed.",
            (void *)connection,
            packet_id);
        return;
    }

    /* Invoke the complete callback. */
    if (on_complete) {
        on_complete(&connection->base, packet_id, error_code, on_complete_ud);
    }
}

struct mqtt_shutdown_task {
    int error_code;
    struct aws_channel_task task;
};

static void s_mqtt_disconnect_task(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {

    (void)status;

    struct mqtt_shutdown_task *task = AWS_CONTAINER_OF(channel_task, struct mqtt_shutdown_task, task);
    struct aws_mqtt_client_connection_311_impl *connection = arg;

    AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Doing disconnect", (void *)connection);
    { /* BEGIN CRITICAL SECTION */
        mqtt_connection_lock_synced_data(connection);
        /* If there is an outstanding reconnect task, cancel it */
        if (connection->synced_data.state == AWS_MQTT_CLIENT_STATE_DISCONNECTING && connection->reconnect_task) {
            aws_atomic_store_ptr(&connection->reconnect_task->connection_ptr, NULL);
            /* If the reconnect_task isn't scheduled, free it */
            if (connection->reconnect_task && !connection->reconnect_task->task.timestamp) {
                aws_mem_release(connection->reconnect_task->allocator, connection->reconnect_task);
            }
            connection->reconnect_task = NULL;
        }
        mqtt_connection_unlock_synced_data(connection);
    } /* END CRITICAL SECTION */

    if (connection->slot && connection->slot->channel) {
        aws_channel_shutdown(connection->slot->channel, task->error_code);
    }

    aws_mem_release(connection->allocator, task);
}

void mqtt_disconnect_impl(struct aws_mqtt_client_connection_311_impl *connection, int error_code) {
    if (connection->slot) {
        struct mqtt_shutdown_task *shutdown_task =
            aws_mem_calloc(connection->allocator, 1, sizeof(struct mqtt_shutdown_task));
        shutdown_task->error_code = error_code;
        aws_channel_task_init(&shutdown_task->task, s_mqtt_disconnect_task, connection, "mqtt_disconnect");
        aws_channel_schedule_task_now(connection->slot->channel, &shutdown_task->task);
    } else {
        AWS_LOGF_TRACE(AWS_LS_MQTT_CLIENT, "id=%p: Client currently has no slot to disconnect", (void *)connection);
    }
}
