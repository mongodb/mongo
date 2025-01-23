#ifndef AWS_MQTT_PRIVATE_REQUEST_RESPONSE_SUBSCRIPTION_MANAGER_H
#define AWS_MQTT_PRIVATE_REQUEST_RESPONSE_SUBSCRIPTION_MANAGER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/common/hash_table.h>

struct aws_mqtt_protocol_adapter;
struct aws_protocol_adapter_connection_event;
struct aws_protocol_adapter_subscription_event;

/*
 * Describes a change to the state of a request operation subscription
 */
enum aws_rr_subscription_event_type {

    /*
     * A request subscription subscribe succeeded
     */
    ARRSET_REQUEST_SUBSCRIBE_SUCCESS,

    /*
     * A request subscription subscribe failed
     */
    ARRSET_REQUEST_SUBSCRIBE_FAILURE,

    /*
     * A previously successful request subscription has ended.
     *
     * Under normal circumstances this can happen when
     *
     * (1) failure to rejoin a session
     */
    ARRSET_REQUEST_SUBSCRIPTION_ENDED,

    /*
     * A streaming subscription subscribe succeeded
     */
    ARRSET_STREAMING_SUBSCRIPTION_ESTABLISHED,

    /*
     * The protocol client failed to rejoin a session containing a previously-established streaming subscription
     */
    ARRSET_STREAMING_SUBSCRIPTION_LOST,

    /*
     * A streaming subscription subscribe attempt resulted in an error or reason code that the client has determined
     * will result in indefinite failures to subscribe.  In this case, we stop attempting to resubscribe.
     *
     * Situations that can lead to this:
     * (1) Permission failures
     * (2) Invalid topic filter
     */
    ARRSET_STREAMING_SUBSCRIPTION_HALTED,

    /*
     * A subscription has lost its last listener and can be purged
     *
     * This event is global; operation_id will always be zero.
     */
    ARRSET_SUBSCRIPTION_EMPTY,

    /*
     * A subscription has been unsubscribed from
     *
     * This event is global; operation_id will always be zero.
     */
    ARRSET_UNSUBSCRIBE_COMPLETE,
};

struct aws_rr_subscription_status_event {
    enum aws_rr_subscription_event_type type;
    struct aws_byte_cursor topic_filter;
    uint64_t operation_id;
};

/*
 * Invariant: despite being on the same thread, these callbacks must be queued as cross-thread tasks on the native
 * request-response client.  This allows us to iterate internal collections without worrying about external
 * callers disrupting things by invoking APIs back on us.
 */
typedef void(
    aws_rr_subscription_status_event_callback_fn)(const struct aws_rr_subscription_status_event *event, void *userdata);

struct aws_rr_subscription_manager_options {

    /*
     * Maximum number of request-response subscriptions allowed.  Must be at least two.
     */
    size_t max_request_response_subscriptions;

    /*
     * Maximum number of streaming subscriptions allowed.
     */
    size_t max_streaming_subscriptions;

    /*
     * Ack timeout to use for all subscribe and unsubscribe operations
     */
    uint32_t operation_timeout_seconds;

    aws_rr_subscription_status_event_callback_fn *subscription_status_callback;
    void *userdata;
};

/*
 * The subscription manager works with the request-response client to handle subscriptions in an eager manner.
 * Subscription purges are checked with every client service call.  Unsubscribe failures don't trigger anything special,
 * we'll just try again next time we look for subscription space.  Subscribes are attempted on idle subscriptions
 * that still need them, either in response to a new operation listener or a connection resumption event.
 *
 * We only allow one subscribe or unsubscribe to be outstanding at once for a given topic.  If an operation requires a
 * subscription while an unsubscribe is in progress the operation is blocked until the unsubscribe resolves.
 *
 * These invariants are dropped during shutdown.  In that case, we immediately send unsubscribes for everything
 * that is not already unsubscribing.
 */
struct aws_rr_subscription_manager {
    struct aws_allocator *allocator;

    struct aws_rr_subscription_manager_options config;

    /* non-owning reference; the client is responsible for destroying this asynchronously (listener detachment) */
    struct aws_mqtt_protocol_adapter *protocol_adapter;

    /* &aws_rr_subscription_record.topic_filter_cursor -> aws_rr_subscription_record * */
    struct aws_hash_table subscriptions;

    bool is_protocol_client_connected;
};

enum aws_rr_subscription_type {
    ARRST_EVENT_STREAM,
    ARRST_REQUEST_RESPONSE,
};

struct aws_rr_acquire_subscription_options {
    struct aws_byte_cursor *topic_filters;
    size_t topic_filter_count;

    uint64_t operation_id;
    enum aws_rr_subscription_type type;
};

struct aws_rr_release_subscription_options {
    struct aws_byte_cursor *topic_filters;
    size_t topic_filter_count;

    uint64_t operation_id;
};

enum aws_acquire_subscription_result_type {

    /*
     * All requested subscriptions already exist and are active.  The operation can proceed to the next stage.
     */
    AASRT_SUBSCRIBED,

    /*
     * The requested subscriptions now exist but at least one is not yet active.  The operation must wait for subscribes
     * to complete as success or failure.
     */
    AASRT_SUBSCRIBING,

    /*
     * At least one subscription does not exist and there is no room for it currently.  Room may open up in the future,
     * so the operation should wait.
     */
    AASRT_BLOCKED,

    /*
     * At least one subscription does not exist and there is no room for it.  Unless an event stream subscription gets
     * closed, no room will be available in the future.  The operation should be failed.
     */
    AASRT_NO_CAPACITY,

    /*
     * An internal failure occurred while trying to establish subscriptions.  The operation should be failed.
     */
    AASRT_FAILURE
};

AWS_EXTERN_C_BEGIN

/*
 * Initializes a subscription manager.  Every native request-response client owns a single subscription manager.
 */
AWS_MQTT_API void aws_rr_subscription_manager_init(
    struct aws_rr_subscription_manager *manager,
    struct aws_allocator *allocator,
    struct aws_mqtt_protocol_adapter *protocol_adapter,
    const struct aws_rr_subscription_manager_options *options);

/*
 * Cleans up a subscription manager.  This is done early in the native request-response client shutdown process.
 * After this API is called, no other subscription manager APIs will be called by the request-response client (during
 * the rest of the asynchronous shutdown process).
 */
AWS_MQTT_API void aws_rr_subscription_manager_clean_up(struct aws_rr_subscription_manager *manager);

/*
 * Requests the the subscription manager unsubscribe from all currently-unused subscriptions
 */
AWS_MQTT_API void aws_rr_subscription_manager_purge_unused(struct aws_rr_subscription_manager *manager);

/*
 * Signals to the subscription manager that the native request-response client is processing an operation that
 * needs a subscription to a particular topic.  Return value indicates to the request-response client how it should
 * proceed with processing the operation.
 */
AWS_MQTT_API enum aws_acquire_subscription_result_type aws_rr_subscription_manager_acquire_subscription(
    struct aws_rr_subscription_manager *manager,
    const struct aws_rr_acquire_subscription_options *options);

/*
 * Signals to the subscription manager that the native request-response client operation no longer
 * needs a subscription to a particular topic.
 */
AWS_MQTT_API void aws_rr_subscription_manager_release_subscription(
    struct aws_rr_subscription_manager *manager,
    const struct aws_rr_release_subscription_options *options);

/*
 * Notifies the subscription manager of a subscription status event.  Invoked by the native request-response client
 * that owns the subscription manager.  The native request-response client also owns the protocol adapter that
 * the subscription event originates from, so the control flow looks like:
 *
 * [Subscribe]
 * subscription manager -> protocol adapter Subscribe -> protocol client Subscribe -> network...
 *
 * [Result]
 * protocol client Suback/Timeout/Error -> protocol adapter -> native request-response client ->
 *      subscription manager (this API)
 */
AWS_MQTT_API void aws_rr_subscription_manager_on_protocol_adapter_subscription_event(
    struct aws_rr_subscription_manager *manager,
    const struct aws_protocol_adapter_subscription_event *event);

/*
 * Notifies the subscription manager of a connection status event.  Invoked by the native request-response client
 * that owns the subscription manager.  The native request-response client also owns the protocol adapter that
 * the connection event originates from. The control flow looks like:
 *
 * protocol client connect/disconnect -> protocol adapter -> native request-response client ->
 *     Subscription manager (this API)
 */
AWS_MQTT_API void aws_rr_subscription_manager_on_protocol_adapter_connection_event(
    struct aws_rr_subscription_manager *manager,
    const struct aws_protocol_adapter_connection_event *event);

/*
 * Checks subscription manager options for validity.
 */
AWS_MQTT_API bool aws_rr_subscription_manager_are_options_valid(
    const struct aws_rr_subscription_manager_options *options);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_PRIVATE_REQUEST_RESPONSE_SUBSCRIPTION_MANAGER_H */
