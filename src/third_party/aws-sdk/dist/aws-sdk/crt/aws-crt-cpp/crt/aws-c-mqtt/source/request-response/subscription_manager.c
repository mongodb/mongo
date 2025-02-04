/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/private/request-response/subscription_manager.h>

#include <aws/common/logging.h>
#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/request-response/protocol_adapter.h>

#include <inttypes.h>

enum aws_rr_subscription_status_type {
    ARRSST_SUBSCRIBED,
    ARRSST_NOT_SUBSCRIBED,
};

/*
 * Invariant: subscriptions can only transition from nothing -> {subscribing, unsubscribing}
 *
 * In particular, the logic blocks subscribing while unsubscribing and unsubscribing while subscribing (unless
 * shutting down).
 */
enum aws_rr_subscription_pending_action_type {
    ARRSPAT_NOTHING,
    ARRSPAT_SUBSCRIBING,
    ARRSPAT_UNSUBSCRIBING,
};

struct aws_rr_subscription_listener {
    struct aws_allocator *allocator;
    uint64_t operation_id;
};

static uint64_t s_aws_hash_subscription_listener(const void *item) {
    const struct aws_rr_subscription_listener *listener = item;

    return listener->operation_id;
}

static bool s_aws_subscription_listener_hash_equality(const void *a, const void *b) {
    const struct aws_rr_subscription_listener *a_listener = a;
    const struct aws_rr_subscription_listener *b_listener = b;

    return a_listener->operation_id == b_listener->operation_id;
}

static void s_aws_subscription_listener_destroy(void *element) {
    struct aws_rr_subscription_listener *listener = element;

    aws_mem_release(listener->allocator, listener);
}

struct aws_rr_subscription_record {
    struct aws_allocator *allocator;

    struct aws_byte_buf topic_filter;
    struct aws_byte_cursor topic_filter_cursor;

    struct aws_hash_table listeners;

    enum aws_rr_subscription_status_type status;
    enum aws_rr_subscription_pending_action_type pending_action;

    enum aws_rr_subscription_type type;

    /*
     * A poisoned record represents a subscription that we will never try to subscribe to because a previous
     * attempt resulted in a failure that we judge to be "terminal."  Terminal failures include permission failures
     * and validation failures.  To remove a poisoned record, all listeners must be removed.  For request-response
     * operations this will happen naturally.  For streaming operations, the operation must be closed by the user (in
     * response to the user-facing event we emit on the streaming operation when the failure that poisons the
     * record occurs).
     */
    bool poisoned;
};

static void s_aws_rr_subscription_record_destroy(void *element) {
    struct aws_rr_subscription_record *record = element;

    aws_byte_buf_clean_up(&record->topic_filter);
    aws_hash_table_clean_up(&record->listeners);

    aws_mem_release(record->allocator, record);
}

static struct aws_rr_subscription_record *s_aws_rr_subscription_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor topic_filter,
    enum aws_rr_subscription_type type) {
    struct aws_rr_subscription_record *record = aws_mem_calloc(allocator, 1, sizeof(struct aws_rr_subscription_record));
    record->allocator = allocator;

    aws_byte_buf_init_copy_from_cursor(&record->topic_filter, allocator, topic_filter);
    record->topic_filter_cursor = aws_byte_cursor_from_buf(&record->topic_filter);

    aws_hash_table_init(
        &record->listeners,
        allocator,
        4,
        s_aws_hash_subscription_listener,
        s_aws_subscription_listener_hash_equality,
        NULL,
        s_aws_subscription_listener_destroy);

    record->status = ARRSST_NOT_SUBSCRIBED;
    record->pending_action = ARRSPAT_NOTHING;

    record->type = type;

    return record;
}

static void s_subscription_record_unsubscribe(
    struct aws_rr_subscription_manager *manager,
    struct aws_rr_subscription_record *record,
    bool shutdown) {

    bool currently_subscribed = record->status == ARRSST_SUBSCRIBED;
    bool currently_subscribing = record->pending_action == ARRSPAT_SUBSCRIBING;
    bool currently_unsubscribing = record->pending_action == ARRSPAT_UNSUBSCRIBING;

    /*
     * The difference between a shutdown unsubscribe and a normal unsubscribe is that on a shutdown we will "chase"
     * a pending subscribe with an unsubscribe (breaking the invariant of never having multiple MQTT operations
     * pending on a subscription).
     */
    bool should_unsubscribe = currently_subscribed && !currently_unsubscribing;
    if (shutdown) {
        should_unsubscribe = should_unsubscribe || currently_subscribing;
    }

    if (!should_unsubscribe) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - subscription ('" PRInSTR
            "') has no listeners but is not in a state that allows unsubscribe yet",
            AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor));
        return;
    }

    struct aws_protocol_adapter_unsubscribe_options unsubscribe_options = {
        .topic_filter = record->topic_filter_cursor,
        .ack_timeout_seconds = manager->config.operation_timeout_seconds,
    };

    if (aws_mqtt_protocol_adapter_unsubscribe(manager->protocol_adapter, &unsubscribe_options)) {
        int error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - sync unsubscribe failure for ('" PRInSTR "'), ec %d(%s)",
            AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor),
            error_code,
            aws_error_debug_str(error_code));
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager - unsubscribe submitted for ('" PRInSTR "')",
        AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor));

    record->pending_action = ARRSPAT_UNSUBSCRIBING;
}

/* Only called by the request-response client when shutting down */
static int s_rr_subscription_clean_up_foreach_wrap(void *context, struct aws_hash_element *elem) {
    struct aws_rr_subscription_manager *manager = context;
    struct aws_rr_subscription_record *subscription = elem->value;

    s_subscription_record_unsubscribe(manager, subscription, true);
    s_aws_rr_subscription_record_destroy(subscription);

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE | AWS_COMMON_HASH_TABLE_ITER_DELETE;
}

static struct aws_rr_subscription_record *s_get_subscription_record(
    struct aws_rr_subscription_manager *manager,
    struct aws_byte_cursor topic_filter) {
    struct aws_rr_subscription_record *subscription = NULL;
    struct aws_hash_element *element = NULL;
    if (aws_hash_table_find(&manager->subscriptions, &topic_filter, &element)) {
        return NULL;
    }

    if (element != NULL) {
        subscription = element->value;
    }

    return subscription;
}

struct aws_subscription_stats {
    size_t request_response_subscriptions;
    size_t event_stream_subscriptions;
    size_t unsubscribing_event_stream_subscriptions;
};

static int s_rr_subscription_count_foreach_wrap(void *context, struct aws_hash_element *elem) {
    const struct aws_rr_subscription_record *subscription = elem->value;
    struct aws_subscription_stats *stats = context;

    if (subscription->type == ARRST_EVENT_STREAM) {
        ++stats->event_stream_subscriptions;
        if (subscription->pending_action == ARRSPAT_UNSUBSCRIBING) {
            ++stats->unsubscribing_event_stream_subscriptions;
        }
    } else {
        ++stats->request_response_subscriptions;
    }

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static void s_get_subscription_stats(
    struct aws_rr_subscription_manager *manager,
    struct aws_subscription_stats *stats) {
    AWS_ZERO_STRUCT(*stats);

    aws_hash_table_foreach(&manager->subscriptions, s_rr_subscription_count_foreach_wrap, stats);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager current stats: %d event stream sub records, %d request-response sub "
        "records, %d unsubscribing event stream subscriptions",
        (int)stats->event_stream_subscriptions,
        (int)stats->request_response_subscriptions,
        (int)stats->unsubscribing_event_stream_subscriptions);
}

static void s_remove_listener_from_subscription_record(
    struct aws_rr_subscription_manager *manager,
    struct aws_byte_cursor topic_filter,
    uint64_t operation_id) {
    struct aws_rr_subscription_record *record = s_get_subscription_record(manager, topic_filter);
    if (record == NULL) {
        return;
    }

    struct aws_rr_subscription_listener listener = {
        .operation_id = operation_id,
    };

    aws_hash_table_remove(&record->listeners, &listener, NULL, NULL);

    size_t listener_count = aws_hash_table_get_entry_count(&record->listeners);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager - removed listener %" PRIu64 " from subscription ('" PRInSTR
        "'), %zu listeners left",
        operation_id,
        AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor),
        listener_count);

    if (listener_count == 0) {
        struct aws_rr_subscription_status_event event = {
            .type = ARRSET_SUBSCRIPTION_EMPTY,
            .topic_filter = record->topic_filter_cursor,
            .operation_id = 0,
        };

        (*manager->config.subscription_status_callback)(&event, manager->config.userdata);
    }
}

static void s_add_listener_to_subscription_record(struct aws_rr_subscription_record *record, uint64_t operation_id) {
    struct aws_rr_subscription_listener *listener =
        aws_mem_calloc(record->allocator, 1, sizeof(struct aws_rr_subscription_listener));
    listener->allocator = record->allocator;
    listener->operation_id = operation_id;

    aws_hash_table_put(&record->listeners, listener, listener, NULL);

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager - added listener %" PRIu64 " to subscription ('" PRInSTR
        "'), %zu listeners total",
        operation_id,
        AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor),
        aws_hash_table_get_entry_count(&record->listeners));
}

static int s_rr_subscription_purge_unused_subscriptions_wrapper(void *context, struct aws_hash_element *elem) {
    struct aws_rr_subscription_record *record = elem->value;
    struct aws_rr_subscription_manager *manager = context;

    if (aws_hash_table_get_entry_count(&record->listeners) == 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - checking subscription ('" PRInSTR "') for removal",
            AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor));

        if (manager->is_protocol_client_connected) {
            s_subscription_record_unsubscribe(manager, record, false);
        }

        if (record->status == ARRSST_NOT_SUBSCRIBED && record->pending_action == ARRSPAT_NOTHING) {
            AWS_LOGF_DEBUG(
                AWS_LS_MQTT_REQUEST_RESPONSE,
                "request-response subscription manager - deleting subscription ('" PRInSTR "')",
                AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor));

            s_aws_rr_subscription_record_destroy(record);
            return AWS_COMMON_HASH_TABLE_ITER_CONTINUE | AWS_COMMON_HASH_TABLE_ITER_DELETE;
        }
    }

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

void aws_rr_subscription_manager_purge_unused(struct aws_rr_subscription_manager *manager) {
    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE, "request-response subscription manager - purging unused subscriptions");
    aws_hash_table_foreach(&manager->subscriptions, s_rr_subscription_purge_unused_subscriptions_wrapper, manager);
}

static const char *s_rr_subscription_event_type_to_c_str(enum aws_rr_subscription_event_type type) {
    switch (type) {
        case ARRSET_REQUEST_SUBSCRIBE_SUCCESS:
            return "RequestSubscribeSuccess";

        case ARRSET_REQUEST_SUBSCRIBE_FAILURE:
            return "RequestSubscribeFailure";

        case ARRSET_REQUEST_SUBSCRIPTION_ENDED:
            return "RequestSubscriptionEnded";

        case ARRSET_STREAMING_SUBSCRIPTION_ESTABLISHED:
            return "StreamingSubscriptionEstablished";

        case ARRSET_STREAMING_SUBSCRIPTION_LOST:
            return "StreamingSubscriptionLost";

        case ARRSET_STREAMING_SUBSCRIPTION_HALTED:
            return "StreamingSubscriptionHalted";

        case ARRSET_UNSUBSCRIBE_COMPLETE:
            return "UnsubscribeComplete";

        case ARRSET_SUBSCRIPTION_EMPTY:
            return "SubscriptionEmpty";
    }

    return "Unknown";
}

static bool s_subscription_type_matches_event_type(
    enum aws_rr_subscription_type subscription_type,
    enum aws_rr_subscription_event_type event_type) {
    switch (event_type) {
        case ARRSET_REQUEST_SUBSCRIBE_SUCCESS:
        case ARRSET_REQUEST_SUBSCRIBE_FAILURE:
        case ARRSET_REQUEST_SUBSCRIPTION_ENDED:
            return subscription_type == ARRST_REQUEST_RESPONSE;

        case ARRSET_STREAMING_SUBSCRIPTION_ESTABLISHED:
        case ARRSET_STREAMING_SUBSCRIPTION_LOST:
        case ARRSET_STREAMING_SUBSCRIPTION_HALTED:
            return subscription_type == ARRST_EVENT_STREAM;

        default:
            return true;
    }
}

static void s_emit_subscription_event(
    const struct aws_rr_subscription_manager *manager,
    const struct aws_rr_subscription_record *record,
    enum aws_rr_subscription_event_type type) {

    AWS_FATAL_ASSERT(s_subscription_type_matches_event_type(record->type, type));

    for (struct aws_hash_iter iter = aws_hash_iter_begin(&record->listeners); !aws_hash_iter_done(&iter);
         aws_hash_iter_next(&iter)) {

        struct aws_rr_subscription_listener *listener = iter.element.value;
        struct aws_rr_subscription_status_event event = {
            .type = type,
            .topic_filter = record->topic_filter_cursor,
            .operation_id = listener->operation_id,
        };

        (*manager->config.subscription_status_callback)(&event, manager->config.userdata);

        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - subscription event for ('" PRInSTR
            "'), type: %s, operation: %" PRIu64 "",
            AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor),
            s_rr_subscription_event_type_to_c_str(type),
            listener->operation_id);
    }
}

static int s_rr_activate_idle_subscription(
    struct aws_rr_subscription_manager *manager,
    struct aws_rr_subscription_record *record) {
    int result = AWS_OP_SUCCESS;

    if (record->poisoned) {
        /*
         * Don't try and establish poisoned subscriptions.  This is not an error or a loggable event, it just means
         * we hit a "try and make subscriptions" event when a poisoned subscription still hadn't been fully released.
         */
        return AWS_OP_SUCCESS;
    }

    if (manager->is_protocol_client_connected && aws_hash_table_get_entry_count(&record->listeners) > 0) {
        if (record->status == ARRSST_NOT_SUBSCRIBED && record->pending_action == ARRSPAT_NOTHING) {
            struct aws_protocol_adapter_subscribe_options subscribe_options = {
                .topic_filter = record->topic_filter_cursor,
                .ack_timeout_seconds = manager->config.operation_timeout_seconds,
            };

            result = aws_mqtt_protocol_adapter_subscribe(manager->protocol_adapter, &subscribe_options);
            if (result == AWS_OP_SUCCESS) {
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_REQUEST_RESPONSE,
                    "request-response subscription manager - initiating subscribe operation for ('" PRInSTR "')",
                    AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor));
                record->pending_action = ARRSPAT_SUBSCRIBING;
            } else {
                int error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_MQTT_REQUEST_RESPONSE,
                    "request-response subscription manager - synchronous failure subscribing to ('" PRInSTR
                    "'), ec %d(%s)",
                    AWS_BYTE_CURSOR_PRI(record->topic_filter_cursor),
                    error_code,
                    aws_error_debug_str(error_code));

                if (record->type == ARRST_REQUEST_RESPONSE) {
                    s_emit_subscription_event(manager, record, ARRSET_REQUEST_SUBSCRIBE_FAILURE);
                } else {
                    record->poisoned = true;
                    s_emit_subscription_event(manager, record, ARRSET_STREAMING_SUBSCRIPTION_HALTED);
                }
            }
        }
    }

    return result;
}

enum aws_acquire_subscription_result_type aws_rr_subscription_manager_acquire_subscription(
    struct aws_rr_subscription_manager *manager,
    const struct aws_rr_acquire_subscription_options *options) {

    if (options->topic_filter_count == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - acquire_subscription for operation %" PRIu64
            " with no topic filters",
            options->operation_id);
        return AASRT_FAILURE;
    }

    /*
     * Check for poisoned or mismatched records.  This has precedence over the following unsubscribing check,
     * and so we put them in separate loops
     */
    for (size_t i = 0; i < options->topic_filter_count; ++i) {
        struct aws_byte_cursor topic_filter = options->topic_filters[i];
        struct aws_rr_subscription_record *existing_record = s_get_subscription_record(manager, topic_filter);
        if (existing_record == NULL) {
            continue;
        }

        if (existing_record->poisoned) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_REQUEST_RESPONSE,
                "request-response subscription manager - acquire subscription for ('" PRInSTR "'), operation %" PRIu64
                " failed - existing subscription is poisoned and has not been released",
                AWS_BYTE_CURSOR_PRI(topic_filter),
                options->operation_id);
            return AASRT_FAILURE;
        }

        if (existing_record->type != options->type) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_REQUEST_RESPONSE,
                "request-response subscription manager - acquire subscription for ('" PRInSTR "'), operation %" PRIu64
                " failed - conflicts with subscription type of existing subscription",
                AWS_BYTE_CURSOR_PRI(topic_filter),
                options->operation_id);
            return AASRT_FAILURE;
        }
    }

    /* blocked if an existing record is unsubscribing; also compute how many subscriptions are needed */
    size_t subscriptions_needed = 0;
    for (size_t i = 0; i < options->topic_filter_count; ++i) {
        struct aws_byte_cursor topic_filter = options->topic_filters[i];
        struct aws_rr_subscription_record *existing_record = s_get_subscription_record(manager, topic_filter);
        if (existing_record != NULL) {
            if (existing_record->pending_action == ARRSPAT_UNSUBSCRIBING) {
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_REQUEST_RESPONSE,
                    "request-response subscription manager - acquire subscription for ('" PRInSTR
                    "'), operation %" PRIu64 " blocked - existing subscription is unsubscribing",
                    AWS_BYTE_CURSOR_PRI(topic_filter),
                    options->operation_id);
                return AASRT_BLOCKED;
            }
        } else {
            ++subscriptions_needed;
        }
    }

    /* Check for space and fail or block as appropriate */
    if (subscriptions_needed > 0) {
        /* how much of the budget are we using? */
        struct aws_subscription_stats stats;
        s_get_subscription_stats(manager, &stats);

        if (options->type == ARRST_REQUEST_RESPONSE) {
            if (subscriptions_needed >
                manager->config.max_request_response_subscriptions - stats.request_response_subscriptions) {
                AWS_LOGF_DEBUG(
                    AWS_LS_MQTT_REQUEST_RESPONSE,
                    "request-response subscription manager - acquire subscription for request operation %" PRIu64
                    " blocked - no room currently",
                    options->operation_id);
                return AASRT_BLOCKED;
            }
        } else {
            /*
             * Streaming subscriptions have more complicated space-checking logic.  Under certain conditions, we may
             * block rather than failing
             */
            if (subscriptions_needed + stats.event_stream_subscriptions > manager->config.max_streaming_subscriptions) {
                if (subscriptions_needed + stats.event_stream_subscriptions <=
                    manager->config.max_streaming_subscriptions + stats.unsubscribing_event_stream_subscriptions) {
                    /* If enough subscriptions are in the process of going away then wait in the blocked state */
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT_REQUEST_RESPONSE,
                        "request-response subscription manager - acquire subscription for streaming operation %" PRIu64
                        " blocked - no room currently",
                        options->operation_id);
                    return AASRT_BLOCKED;
                } else {
                    /* Otherwise, there's no hope, fail */
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT_REQUEST_RESPONSE,
                        "request-response subscription manager - acquire subscription for operation %" PRIu64
                        " failed - no room",
                        options->operation_id);
                    return AASRT_NO_CAPACITY;
                }
            }
        }
    }

    bool is_fully_subscribed = true;
    for (size_t i = 0; i < options->topic_filter_count; ++i) {
        struct aws_byte_cursor topic_filter = options->topic_filters[i];
        struct aws_rr_subscription_record *existing_record = s_get_subscription_record(manager, topic_filter);

        if (existing_record == NULL) {
            existing_record = s_aws_rr_subscription_new(manager->allocator, topic_filter, options->type);
            aws_hash_table_put(&manager->subscriptions, &existing_record->topic_filter_cursor, existing_record, NULL);
        }

        s_add_listener_to_subscription_record(existing_record, options->operation_id);
        if (existing_record->status != ARRSST_SUBSCRIBED) {
            is_fully_subscribed = false;
        }
    }

    if (is_fully_subscribed) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - acquire subscription for operation %" PRIu64
            " fully subscribed - all required subscriptions are active",
            options->operation_id);
        return AASRT_SUBSCRIBED;
    }

    for (size_t i = 0; i < options->topic_filter_count; ++i) {
        struct aws_byte_cursor topic_filter = options->topic_filters[i];
        struct aws_rr_subscription_record *existing_record = s_get_subscription_record(manager, topic_filter);

        if (s_rr_activate_idle_subscription(manager, existing_record)) {
            AWS_LOGF_ERROR(
                AWS_LS_MQTT_REQUEST_RESPONSE,
                "request-response subscription manager - acquire subscription for operation %" PRIu64
                " failed - synchronous subscribe failure",
                options->operation_id);
            return AASRT_FAILURE;
        }
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager - acquire subscription for operation %" PRIu64
        " subscribing - waiting on one or more subscribes to complete",
        options->operation_id);

    return AASRT_SUBSCRIBING;
}

void aws_rr_subscription_manager_release_subscription(
    struct aws_rr_subscription_manager *manager,
    const struct aws_rr_release_subscription_options *options) {
    for (size_t i = 0; i < options->topic_filter_count; ++i) {
        struct aws_byte_cursor topic_filter = options->topic_filters[i];
        s_remove_listener_from_subscription_record(manager, topic_filter, options->operation_id);
    }
}

static void s_handle_protocol_adapter_request_subscription_event(
    struct aws_rr_subscription_manager *manager,
    struct aws_rr_subscription_record *record,
    const struct aws_protocol_adapter_subscription_event *event) {
    if (event->event_type == AWS_PASET_SUBSCRIBE) {
        AWS_FATAL_ASSERT(record->pending_action == ARRSPAT_SUBSCRIBING);
        record->pending_action = ARRSPAT_NOTHING;

        if (event->error_code == AWS_ERROR_SUCCESS) {
            record->status = ARRSST_SUBSCRIBED;
            s_emit_subscription_event(manager, record, ARRSET_REQUEST_SUBSCRIBE_SUCCESS);
        } else {
            s_emit_subscription_event(manager, record, ARRSET_REQUEST_SUBSCRIBE_FAILURE);
        }
    } else {
        AWS_FATAL_ASSERT(event->event_type == AWS_PASET_UNSUBSCRIBE);
        AWS_FATAL_ASSERT(record->pending_action == ARRSPAT_UNSUBSCRIBING);
        record->pending_action = ARRSPAT_NOTHING;

        if (event->error_code == AWS_ERROR_SUCCESS) {
            record->status = ARRSST_NOT_SUBSCRIBED;

            struct aws_rr_subscription_status_event unsubscribe_event = {
                .type = ARRSET_UNSUBSCRIBE_COMPLETE,
                .topic_filter = record->topic_filter_cursor,
                .operation_id = 0,
            };

            (*manager->config.subscription_status_callback)(&unsubscribe_event, manager->config.userdata);
        }
    }
}

static void s_handle_protocol_adapter_streaming_subscription_event(
    struct aws_rr_subscription_manager *manager,
    struct aws_rr_subscription_record *record,
    const struct aws_protocol_adapter_subscription_event *event) {
    if (event->event_type == AWS_PASET_SUBSCRIBE) {
        AWS_FATAL_ASSERT(record->pending_action == ARRSPAT_SUBSCRIBING);
        record->pending_action = ARRSPAT_NOTHING;

        if (event->error_code == AWS_ERROR_SUCCESS) {
            record->status = ARRSST_SUBSCRIBED;
            s_emit_subscription_event(manager, record, ARRSET_STREAMING_SUBSCRIPTION_ESTABLISHED);
        } else {
            if (event->retryable) {
                s_rr_activate_idle_subscription(manager, record);
            } else {
                record->poisoned = true;
                s_emit_subscription_event(manager, record, ARRSET_STREAMING_SUBSCRIPTION_HALTED);
            }
        }
    } else {
        AWS_FATAL_ASSERT(event->event_type == AWS_PASET_UNSUBSCRIBE);
        AWS_FATAL_ASSERT(record->pending_action == ARRSPAT_UNSUBSCRIBING);
        record->pending_action = ARRSPAT_NOTHING;

        if (event->error_code == AWS_ERROR_SUCCESS) {
            record->status = ARRSST_NOT_SUBSCRIBED;

            struct aws_rr_subscription_status_event unsubscribe_event = {
                .type = ARRSET_UNSUBSCRIBE_COMPLETE,
                .topic_filter = record->topic_filter_cursor,
                .operation_id = 0,
            };

            (*manager->config.subscription_status_callback)(&unsubscribe_event, manager->config.userdata);
        }
    }
}

void aws_rr_subscription_manager_on_protocol_adapter_subscription_event(
    struct aws_rr_subscription_manager *manager,
    const struct aws_protocol_adapter_subscription_event *event) {
    struct aws_rr_subscription_record *record = s_get_subscription_record(manager, event->topic_filter);
    if (record == NULL) {
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_MQTT_REQUEST_RESPONSE,
        "request-response subscription manager - received a protocol adapter subscription event for ('" PRInSTR
        "'), type %s, error_code %d(%s)",
        AWS_BYTE_CURSOR_PRI(event->topic_filter),
        aws_protocol_adapter_subscription_event_type_to_c_str(event->event_type),
        event->error_code,
        aws_error_debug_str(event->error_code));

    if (record->type == ARRST_REQUEST_RESPONSE) {
        s_handle_protocol_adapter_request_subscription_event(manager, record, event);
    } else {
        s_handle_protocol_adapter_streaming_subscription_event(manager, record, event);
    }
}

static int s_rr_activate_idle_subscriptions_wrapper(void *context, struct aws_hash_element *elem) {
    struct aws_rr_subscription_record *record = elem->value;
    struct aws_rr_subscription_manager *manager = context;

    s_rr_activate_idle_subscription(manager, record);

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static void s_activate_idle_subscriptions(struct aws_rr_subscription_manager *manager) {
    aws_hash_table_foreach(&manager->subscriptions, s_rr_activate_idle_subscriptions_wrapper, manager);
}

static int s_apply_session_lost_wrapper(void *context, struct aws_hash_element *elem) {
    struct aws_rr_subscription_record *record = elem->value;
    struct aws_rr_subscription_manager *manager = context;

    if (record->status == ARRSST_SUBSCRIBED) {
        record->status = ARRSST_NOT_SUBSCRIBED;

        if (record->type == ARRST_REQUEST_RESPONSE) {
            s_emit_subscription_event(manager, record, ARRSET_REQUEST_SUBSCRIPTION_ENDED);

            if (record->pending_action != ARRSPAT_UNSUBSCRIBING) {
                s_aws_rr_subscription_record_destroy(record);
                return AWS_COMMON_HASH_TABLE_ITER_CONTINUE | AWS_COMMON_HASH_TABLE_ITER_DELETE;
            }
        } else {
            s_emit_subscription_event(manager, record, ARRSET_STREAMING_SUBSCRIPTION_LOST);
        }
    }

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static int s_apply_streaming_resubscribe_wrapper(void *context, struct aws_hash_element *elem) {
    struct aws_rr_subscription_record *record = elem->value;
    struct aws_rr_subscription_manager *manager = context;

    if (record->type == ARRST_EVENT_STREAM) {
        s_rr_activate_idle_subscription(manager, record);
    }

    return AWS_COMMON_HASH_TABLE_ITER_CONTINUE;
}

static void s_apply_session_lost(struct aws_rr_subscription_manager *manager) {
    aws_hash_table_foreach(&manager->subscriptions, s_apply_session_lost_wrapper, manager);
    aws_hash_table_foreach(&manager->subscriptions, s_apply_streaming_resubscribe_wrapper, manager);
}

void aws_rr_subscription_manager_on_protocol_adapter_connection_event(
    struct aws_rr_subscription_manager *manager,
    const struct aws_protocol_adapter_connection_event *event) {

    if (event->event_type == AWS_PACET_CONNECTED) {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - received a protocol adapter connection event, joined_session: "
            "%d",
            (int)(event->joined_session ? 1 : 0));

        manager->is_protocol_client_connected = true;
        if (!event->joined_session) {
            s_apply_session_lost(manager);
        }

        aws_rr_subscription_manager_purge_unused(manager);
        s_activate_idle_subscriptions(manager);
    } else {
        AWS_LOGF_DEBUG(
            AWS_LS_MQTT_REQUEST_RESPONSE,
            "request-response subscription manager - received a protocol adapter disconnection event");

        manager->is_protocol_client_connected = false;
    }
}

bool aws_rr_subscription_manager_are_options_valid(const struct aws_rr_subscription_manager_options *options) {
    if (options == NULL || options->max_request_response_subscriptions < 2 || options->operation_timeout_seconds == 0) {
        return false;
    }

    return true;
}

void aws_rr_subscription_manager_init(
    struct aws_rr_subscription_manager *manager,
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter *protocol_adapter,
    const struct aws_rr_subscription_manager_options *options) {
    AWS_ZERO_STRUCT(*manager);

    AWS_FATAL_ASSERT(aws_rr_subscription_manager_are_options_valid(options));

    manager->allocator = allocator;
    manager->config = *options;
    manager->protocol_adapter = protocol_adapter;

    aws_hash_table_init(
        &manager->subscriptions,
        allocator,
        options->max_request_response_subscriptions + options->max_streaming_subscriptions,
        aws_hash_byte_cursor_ptr,
        aws_mqtt_byte_cursor_hash_equality,
        NULL,
        s_aws_rr_subscription_record_destroy);

    manager->is_protocol_client_connected = aws_mqtt_protocol_adapter_is_connected(protocol_adapter);
}

void aws_rr_subscription_manager_clean_up(struct aws_rr_subscription_manager *manager) {
    aws_hash_table_foreach(&manager->subscriptions, s_rr_subscription_clean_up_foreach_wrap, manager);
    aws_hash_table_clean_up(&manager->subscriptions);
}
