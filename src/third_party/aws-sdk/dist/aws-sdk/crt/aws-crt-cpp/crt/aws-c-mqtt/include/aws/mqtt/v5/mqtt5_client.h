#ifndef AWS_MQTT_MQTT5_CLIENT_H
#define AWS_MQTT_MQTT5_CLIENT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/io/retry_strategy.h>
#include <aws/mqtt/v5/mqtt5_types.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_allocator;
struct aws_client_bootstrap;
struct aws_host_resolution_config;
struct aws_http_message;
struct aws_mqtt5_client;
struct aws_mqtt5_client_lifecycle_event;
struct aws_tls_connection_options;
struct aws_socket_options;

/* public client-related enums */

/**
 * Controls how the mqtt client should behave with respect to mqtt sessions.
 */
enum aws_mqtt5_client_session_behavior_type {
    /**
     * Maps to AWS_MQTT5_CSBT_CLEAN
     */
    AWS_MQTT5_CSBT_DEFAULT,

    /**
     * Always join a new, clean session
     */
    AWS_MQTT5_CSBT_CLEAN,

    /**
     * Always attempt to rejoin an existing session after an initial connection success.
     */
    AWS_MQTT5_CSBT_REJOIN_POST_SUCCESS,

    /**
     * Always attempt to rejoin an existing session.  Since the client does not support durable session persistence,
     * this option is not guaranteed to be spec compliant because any unacknowledged qos1 publishes (which are
     * part of the client session state) will not be present on the initial connection.  Until we support
     * durable session resumption, this option is technically spec-breaking, but useful.
     */
    AWS_MQTT5_CSBT_REJOIN_ALWAYS,
};

/**
 * Outbound topic aliasing behavior is controlled by this type.
 *
 * Topic alias behavior is described in https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113
 *
 * If the server allows topic aliasing, this setting controls how topic aliases are used on PUBLISH packets sent
 * from the client to the server.
 *
 * If topic aliasing is not supported by the server, this setting has no effect and any attempts to directly
 * manipulate the topic alias id in outbound publishes will be ignored.
 */
enum aws_mqtt5_client_outbound_topic_alias_behavior_type {
    /**
     * Maps to AWS_MQTT5_COTABT_DISABLED  This keeps the client from being broken (by default) if the broker
     * topic aliasing implementation has a problem.
     */
    AWS_MQTT5_COTABT_DEFAULT,

    /**
     * Outbound aliasing is the user's responsibility.  Client will cache and use
     * previously-established aliases if they fall within the negotiated limits of the connection.
     *
     * The user must still always submit a full topic in their publishes because disconnections disrupt
     * topic alias mappings unpredictably.  The client will properly use the alias when the current connection
     * has seen the alias binding already.
     */
    AWS_MQTT5_COTABT_MANUAL,

    /**
     * Client ignores any user-specified topic aliasing and acts on the outbound alias set as an LRU cache.
     */
    AWS_MQTT5_COTABT_LRU,

    /**
     * Completely disable outbound topic aliasing.
     */
    AWS_MQTT5_COTABT_DISABLED
};

/**
 * Inbound topic aliasing behavior is controlled by this type.
 *
 * Topic alias behavior is described in https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113
 *
 * This setting controls whether or not the client will send a positive topic alias maximum to the server
 * in its CONNECT packets.
 *
 * If topic aliasing is not supported by the server, this setting has no net effect.
 */
enum aws_mqtt5_client_inbound_topic_alias_behavior_type {
    /**
     * Maps to AWS_MQTT5_CITABT_DISABLED
     */
    AWS_MQTT5_CITABT_DEFAULT,

    /**
     * Allow the server to send PUBLISH packets to the client that use topic aliasing
     */
    AWS_MQTT5_CITABT_ENABLED,

    /**
     * Forbid the server from sending PUBLISH packets to the client that use topic aliasing
     */
    AWS_MQTT5_CITABT_DISABLED
};

/**
 * Configuration struct for all client topic aliasing behavior.  If this is left null, then all default options
 * (as it zeroed) will be used.
 */
struct aws_mqtt5_client_topic_alias_options {

    /**
     * Controls what kind of outbound topic aliasing behavior the client should attempt to use.
     */
    enum aws_mqtt5_client_outbound_topic_alias_behavior_type outbound_topic_alias_behavior;

    /**
     * If outbound topic aliasing is set to LRU, this controls the maximum size of the cache.  If outbound topic
     * aliasing is set to LRU and this is zero, a sensible default is used (25).  If outbound topic aliasing is not
     * set to LRU, then this setting has no effect.
     *
     * The final size of the cache is determined by the minimum of this setting and the value of the
     * topic_alias_maximum property of the received CONNACK.  If the received CONNACK does not have an explicit
     * positive value for that field, outbound topic aliasing is disabled for the duration of that connection.
     */
    uint16_t outbound_alias_cache_max_size;

    /**
     * Controls what kind of inbound topic aliasing behavior the client should use.
     *
     * Even if inbound topic aliasing is enabled, it is up to the server to choose whether or not to use it.
     */
    enum aws_mqtt5_client_inbound_topic_alias_behavior_type inbound_topic_alias_behavior;

    /**
     * If inbound topic aliasing is enabled, this will control the size of the inbound alias cache.  If inbound
     * aliases are enabled and this is zero, then a sensible default will be used (25).  If inbound aliases are
     * disabled, this setting has no effect.
     *
     * Behaviorally, this value overrides anything present in the topic_alias_maximum field of
     * the CONNECT packet options.
     */
    uint16_t inbound_alias_cache_size;
};

/**
 * Extended validation and flow control options
 *
 * Potentially a point of expansion in the future.  We could add custom controls letting people override
 * the Aws IOT Core limits based on their account properties.  We could, with IoT Core support, add dynamic
 * limit recognition via user properties as well.
 */
enum aws_mqtt5_extended_validation_and_flow_control_options {
    /**
     * Do not do any additional validation or flow control outside of the MQTT5 spec
     */
    AWS_MQTT5_EVAFCO_NONE,

    /**
     * Apply additional client-side operational flow control that respects the
     * default AWS IoT Core limits.
     *
     * Applies the following flow control:
     *  (1) Outbound throughput throttled to 512KB/s
     *  (2) Outbound publish TPS throttled to 100
     */
    AWS_MQTT5_EVAFCO_AWS_IOT_CORE_DEFAULTS,
};

/**
 * Controls how disconnects affect the queued and in-progress operations submitted to the client. Also controls
 * how operations are handled while the client is not connected.  In particular, if the client is not connected,
 * then any operation that would be failed on disconnect (according to these rules) will be rejected.
 */
enum aws_mqtt5_client_operation_queue_behavior_type {

    /*
     * Maps to AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT
     */
    AWS_MQTT5_COQBT_DEFAULT,

    /*
     * Requeues QoS 1+ publishes on disconnect; unacked publishes go to the front, unprocessed publishes stay
     * in place.  All other operations (QoS 0 publishes, subscribe, unsubscribe) are failed.
     */
    AWS_MQTT5_COQBT_FAIL_NON_QOS1_PUBLISH_ON_DISCONNECT,

    /*
     * Qos 0 publishes that are not complete at the time of disconnection are failed.  Unacked QoS 1+ publishes are
     * requeued at the head of the line for immediate retransmission on a session resumption.  All other operations
     * are requeued in original order behind any retransmissions.
     */
    AWS_MQTT5_COQBT_FAIL_QOS0_PUBLISH_ON_DISCONNECT,

    /*
     * All operations that are not complete at the time of disconnection are failed, except those operations that
     * the mqtt 5 spec requires to be retransmitted (unacked qos1+ publishes).
     */
    AWS_MQTT5_COQBT_FAIL_ALL_ON_DISCONNECT,
};

/**
 * Type of a client lifecycle event
 */
enum aws_mqtt5_client_lifecycle_event_type {
    /**
     * Emitted when the client begins an attempt to connect to the remote endpoint.
     *
     * Mandatory event fields: client, user_data
     */
    AWS_MQTT5_CLET_ATTEMPTING_CONNECT,

    /**
     * Emitted after the client connects to the remote endpoint and receives a successful CONNACK.
     * Every ATTEMPTING_CONNECT will be followed by exactly one CONNECTION_SUCCESS or one CONNECTION_FAILURE.
     *
     * Mandatory event fields: client, user_data, connack_data, settings
     */
    AWS_MQTT5_CLET_CONNECTION_SUCCESS,

    /**
     * Emitted at any point during the connection process when it has conclusively failed.
     * Every ATTEMPTING_CONNECT will be followed by exactly one CONNECTION_SUCCESS or one CONNECTION_FAILURE.
     *
     * Mandatory event fields: client, user_data, error_code
     * Conditional event fields: connack_data
     */
    AWS_MQTT5_CLET_CONNECTION_FAILURE,

    /**
     * Lifecycle event containing information about a disconnect.  Every CONNECTION_SUCCESS will eventually be
     * followed by one and only one DISCONNECTION.
     *
     * Mandatory event fields: client, user_data, error_code
     * Conditional event fields: disconnect_data
     */
    AWS_MQTT5_CLET_DISCONNECTION,

    /**
     * Lifecycle event notifying the user that the client has entered the STOPPED state.  Entering this state will
     * cause the client to wipe all MQTT session state.
     *
     * Mandatory event fields: client, user_data
     */
    AWS_MQTT5_CLET_STOPPED,
};

/* client-related callback function signatures */

/**
 * Signature of the continuation function to be called after user-code transforms a websocket handshake request
 */
typedef void(aws_mqtt5_transform_websocket_handshake_complete_fn)(
    struct aws_http_message *request,
    int error_code,
    void *complete_ctx);

/**
 * Signature of the websocket handshake request transformation function.  After transformation, the completion
 * function must be invoked to send the request.
 */
typedef void(aws_mqtt5_transform_websocket_handshake_fn)(
    struct aws_http_message *request,
    void *user_data,
    aws_mqtt5_transform_websocket_handshake_complete_fn *complete_fn,
    void *complete_ctx);

/**
 * Callback signature for mqtt5 client lifecycle events.
 */
typedef void(aws_mqtt5_client_connection_event_callback_fn)(const struct aws_mqtt5_client_lifecycle_event *event);

/**
 * Signature of callback to invoke on Publish success/failure.
 */
typedef void(aws_mqtt5_publish_completion_fn)(
    enum aws_mqtt5_packet_type packet_type,
    const void *packet,
    int error_code,
    void *complete_ctx);

/**
 * Signature of callback to invoke on Subscribe success/failure.
 */
typedef void(aws_mqtt5_subscribe_completion_fn)(
    const struct aws_mqtt5_packet_suback_view *suback,
    int error_code,
    void *complete_ctx);

/**
 * Signature of callback to invoke on Unsubscribe success/failure.
 */
typedef void(aws_mqtt5_unsubscribe_completion_fn)(
    const struct aws_mqtt5_packet_unsuback_view *unsuback,
    int error_code,
    void *complete_ctx);

/**
 * Signature of callback to invoke on Publish received
 */
typedef void(aws_mqtt5_publish_received_fn)(const struct aws_mqtt5_packet_publish_view *publish, void *user_data);

/**
 * Signature of a listener publish received callback that returns an indicator whether or not the publish
 * was handled by the listener.
 */
typedef bool(
    aws_mqtt5_listener_publish_received_fn)(const struct aws_mqtt5_packet_publish_view *publish, void *user_data);

/**
 * Signature of callback to invoke when a DISCONNECT is fully written to the socket (or fails to be)
 */
typedef void(aws_mqtt5_disconnect_completion_fn)(int error_code, void *complete_ctx);

/**
 * Signature of callback invoked when a client has completely destroyed itself
 */
typedef void(aws_mqtt5_client_termination_completion_fn)(void *complete_ctx);

/* operation completion options structures */

/**
 * Completion options for the Publish operation
 */
struct aws_mqtt5_publish_completion_options {
    aws_mqtt5_publish_completion_fn *completion_callback;
    void *completion_user_data;

    /** Overrides the client's ack timeout with this value, for this operation only */
    uint32_t ack_timeout_seconds_override;
};

/**
 * Completion options for the Subscribe operation
 */
struct aws_mqtt5_subscribe_completion_options {
    aws_mqtt5_subscribe_completion_fn *completion_callback;
    void *completion_user_data;

    /** Overrides the client's ack timeout with this value, for this operation only */
    uint32_t ack_timeout_seconds_override;
};

/**
 * Completion options for the Unsubscribe operation
 */
struct aws_mqtt5_unsubscribe_completion_options {
    aws_mqtt5_unsubscribe_completion_fn *completion_callback;
    void *completion_user_data;

    /** Overrides the client's ack timeout with this value, for this operation only */
    uint32_t ack_timeout_seconds_override;
};

/**
 * Completion options for the a DISCONNECT operation
 */
struct aws_mqtt5_disconnect_completion_options {
    aws_mqtt5_disconnect_completion_fn *completion_callback;
    void *completion_user_data;
};

/**
 * Mqtt behavior settings that are dynamically negotiated as part of the CONNECT/CONNACK exchange.
 */
struct aws_mqtt5_negotiated_settings {
    /**
     * The maximum QoS used between the server and client.
     */
    enum aws_mqtt5_qos maximum_qos;

    /**
     * the amount of time in seconds the server will retain the session after a disconnect.
     */
    uint32_t session_expiry_interval;

    /**
     * the number of QoS 1 and QoS2 publications the server is willing to process concurrently.
     */
    uint16_t receive_maximum_from_server;

    /**
     * the maximum packet size the server is willing to accept.
     */
    uint32_t maximum_packet_size_to_server;

    /**
     * the highest value that the server will accept as a Topic Alias sent by the client.
     */
    uint16_t topic_alias_maximum_to_server;

    /**
     * the highest value that the client will accept as a Topic Alias sent by the server.
     */
    uint16_t topic_alias_maximum_to_client;

    /**
     * the amount of time in seconds before the server will disconnect the client for inactivity.
     */
    uint16_t server_keep_alive;

    /**
     * whether the server supports retained messages.
     */
    bool retain_available;

    /**
     * whether the server supports wildcard subscriptions.
     */
    bool wildcard_subscriptions_available;

    /**
     * whether the server supports subscription identifiers
     */
    bool subscription_identifiers_available;

    /**
     * whether the server supports shared subscriptions
     */
    bool shared_subscriptions_available;

    /**
     * whether the client has rejoined an existing session.
     */
    bool rejoined_session;

    struct aws_byte_buf client_id_storage;
};

/**
 * Contains some simple statistics about the current state of the client's queue of operations
 */
struct aws_mqtt5_client_operation_statistics {
    /*
     * total number of operations submitted to the client that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    uint64_t incomplete_operation_count;

    /*
     * total packet size of operations submitted to the client that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    uint64_t incomplete_operation_size;

    /*
     * total number of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    uint64_t unacked_operation_count;

    /*
     * total packet size of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    uint64_t unacked_operation_size;
};

/**
 * Details about a client lifecycle event.
 */
struct aws_mqtt5_client_lifecycle_event {

    /**
     * Type of event this is.
     */
    enum aws_mqtt5_client_lifecycle_event_type event_type;

    /**
     * Client this event corresponds to.  Necessary (can't be replaced with user data) because the client
     * doesn't exist at the time the event callback user data is configured.
     */
    struct aws_mqtt5_client *client;

    /**
     * Aws-c-* error code associated with the event
     */
    int error_code;

    /**
     * User data associated with the client's lifecycle event handler.  Set with client configuration.
     */
    void *user_data;

    /**
     * If this event was caused by receiving a CONNACK, this will be a view of that packet.
     */
    const struct aws_mqtt5_packet_connack_view *connack_data;

    /**
     * If this is a successful connection establishment, this will contain the negotiated mqtt5 behavioral settings
     */
    const struct aws_mqtt5_negotiated_settings *settings;

    /**
     * If this event was caused by receiving a DISCONNECT, this will be a view of that packet.
     */
    const struct aws_mqtt5_packet_disconnect_view *disconnect_data;
};

/**
 * Basic mqtt5 client configuration struct.
 *
 * Contains desired connection properties
 * Configuration that represents properties of the mqtt5 CONNECT packet go in the connect view (connect_options)
 */
struct aws_mqtt5_client_options {

    /**
     * Host to establish mqtt connections to
     */
    struct aws_byte_cursor host_name;

    /**
     * Port to establish mqtt connections to
     */
    uint32_t port;

    /**
     * Client bootstrap to use whenever this client establishes a connection
     */
    struct aws_client_bootstrap *bootstrap;

    /**
     * Socket options to use whenever this client establishes a connection
     */
    const struct aws_socket_options *socket_options;

    /**
     * (Optional) Tls options to use whenever this client establishes a connection
     */
    const struct aws_tls_connection_options *tls_options;

    /**
     * (Optional) Http proxy options to use whenever this client establishes a connection
     */
    const struct aws_http_proxy_options *http_proxy_options;

    /**
     * (Optional) Websocket handshake transformation function and user data.  Websockets are used if the
     * transformation function is non-null.
     */
    aws_mqtt5_transform_websocket_handshake_fn *websocket_handshake_transform;
    void *websocket_handshake_transform_user_data;

    /**
     * All CONNECT-related options, includes the will configuration, if desired
     */
    const struct aws_mqtt5_packet_connect_view *connect_options;

    /**
     * Controls session rejoin behavior
     */
    enum aws_mqtt5_client_session_behavior_type session_behavior;

    /**
     * Controls if any additional AWS-specific validation or flow control should be performed by the client.
     */
    enum aws_mqtt5_extended_validation_and_flow_control_options extended_validation_and_flow_control_options;

    /**
     * Controls how the client treats queued/in-progress operations when the connection drops for any reason.
     */
    enum aws_mqtt5_client_operation_queue_behavior_type offline_queue_behavior;

    /**
     * Controls the exponential backoff behavior when the client is waiting to reconnect.
     *
     * See: https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/
     */
    enum aws_exponential_backoff_jitter_mode retry_jitter_mode;

    /**
     * Minimum amount of time in ms to wait before attempting to reconnect.  If this is zero, a default of 1000 ms will
     * be used.
     */
    uint64_t min_reconnect_delay_ms;

    /**
     * Maximum amount of time in ms to wait before attempting to reconnect.  If this is zero, a default of 120000 ms
     * will be used.
     */
    uint64_t max_reconnect_delay_ms;

    /**
     * Amount of time that must elapse with a good connection before the reconnect delay is reset to the minimum.  If
     * this zero, a default of 30000 ms will be used.
     */
    uint64_t min_connected_time_to_reset_reconnect_delay_ms;

    /**
     * Time interval to wait after sending a PINGREQ for a PINGRESP to arrive.  If one does not arrive, the connection
     * will be shut down.  If this is zero, a default of 30000 ms will be used.
     */
    uint32_t ping_timeout_ms;

    /**
     * Time interval to wait after sending a CONNECT request for a CONNACK to arrive.  If one does not arrive, the
     * connection will be shut down.  If this zero, a default of 20000 ms will be used.
     */
    uint32_t connack_timeout_ms;

    /**
     * Time interval to wait for an ack after sending a SUBSCRIBE, UNSUBSCRIBE, or PUBLISH with QoS 1+ before
     * failing the packet, notifying the client of failure, and removing it.  If this is zero, a default of 60 seconds
     * will be used.
     */
    uint32_t ack_timeout_seconds;

    /**
     * Controls how the client uses mqtt5 topic aliasing.  If NULL, zero-based defaults will be used.
     */
    const struct aws_mqtt5_client_topic_alias_options *topic_aliasing_options;

    /**
     * Callback for received publish packets
     */
    aws_mqtt5_publish_received_fn *publish_received_handler;
    void *publish_received_handler_user_data;

    /**
     * Callback and user data for all client lifecycle events.
     * Life cycle events include:
     *    ConnectionSuccess
     *    ConnectionFailure,
     *    Disconnect
     *    (client) Stopped
     *
     *  Disconnect lifecycle events are 1-1 with -- strictly after -- ConnectionSuccess events.
     */
    aws_mqtt5_client_connection_event_callback_fn *lifecycle_event_handler;
    void *lifecycle_event_handler_user_data;

    /**
     * Callback for when the client has completely destroyed itself
     */
    aws_mqtt5_client_termination_completion_fn *client_termination_handler;
    void *client_termination_handler_user_data;

    /**
     * Options to override aspects of DNS resolution.  If unspecified, use a default that matches the regular
     * configuration but changes the refresh frequency to a value that prevents DNS pinging.
     */
    struct aws_host_resolution_config *host_resolution_override;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a new mqtt5 client using the supplied configuration
 *
 * @param allocator allocator to use with all memory operations related to this client's creation and operation
 * @param options mqtt5 client configuration
 * @return a new mqtt5 client or NULL
 */
AWS_MQTT_API
struct aws_mqtt5_client *aws_mqtt5_client_new(
    struct aws_allocator *allocator,
    const struct aws_mqtt5_client_options *options);

/**
 * Acquires a reference to an mqtt5 client
 *
 * @param client client to acquire a reference to.  May be NULL.
 * @return what was passed in as the client (a client or NULL)
 */
AWS_MQTT_API
struct aws_mqtt5_client *aws_mqtt5_client_acquire(struct aws_mqtt5_client *client);

/**
 * Release a reference to an mqtt5 client.  When the client ref count drops to zero, the client will automatically
 * trigger a stop and once the stop completes, the client will delete itself.
 *
 * @param client client to release a reference to.  May be NULL.
 * @return NULL
 */
AWS_MQTT_API
struct aws_mqtt5_client *aws_mqtt5_client_release(struct aws_mqtt5_client *client);

/**
 * Asynchronous notify to the mqtt5 client that you want it to attempt to connect to the configured endpoint.
 * The client will attempt to stay connected using the properties of the reconnect-related parameters
 * in the mqtt5 client configuration.
 *
 * @param client mqtt5 client to start
 * @return success/failure in the synchronous logic that kicks off the start process
 */
AWS_MQTT_API
int aws_mqtt5_client_start(struct aws_mqtt5_client *client);

/**
 * Asynchronous notify to the mqtt5 client that you want it to transition to the stopped state.  When the client
 * reaches the stopped state, all session state is erased.
 *
 * @param client mqtt5 client to stop
 * @param disconnect_options (optional) properties of a DISCONNECT packet to send as part of the shutdown process
 * @return success/failure in the synchronous logic that kicks off the stop process
 */
AWS_MQTT_API
int aws_mqtt5_client_stop(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_disconnect_view *disconnect_options,
    const struct aws_mqtt5_disconnect_completion_options *completion_options);

/**
 * Queues a Publish operation in an mqtt5 client
 *
 * @param client mqtt5 client to queue a Publish for
 * @param publish_options configuration options for the Publish operation
 * @param completion_options completion callback configuration.  Successful QoS 0 publishes invoke the callback when
 * the data has been written to the socket.  Successful QoS1+ publishes invoke the callback when the corresponding ack
 * is received.  Unsuccessful publishes invoke the callback at the point in time a failure condition is reached.
 * @return success/failure in the synchronous logic that kicks off the publish operation
 */
AWS_MQTT_API
int aws_mqtt5_client_publish(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_publish_view *publish_options,
    const struct aws_mqtt5_publish_completion_options *completion_options);

/**
 * Queues a Subscribe operation in an mqtt5 client
 *
 * @param client mqtt5 client to queue a Subscribe for
 * @param subscribe_options configuration options for the Subscribe operation
 * @param completion_options Completion callback configuration.  Invoked when the corresponding SUBACK is received or
 * a failure condition is reached.  An error code implies complete failure of the subscribe, while a success code
 * implies the user must still check all of the SUBACK's reason codes for per-subscription feedback.
 * @return success/failure in the synchronous logic that kicks off the Subscribe operation
 */
AWS_MQTT_API
int aws_mqtt5_client_subscribe(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_subscribe_view *subscribe_options,
    const struct aws_mqtt5_subscribe_completion_options *completion_options);

/**
 * Queues an Unsubscribe operation in an mqtt5 client
 *
 * @param client mqtt5 client to queue an Unsubscribe for
 * @param unsubscribe_options configuration options for the Unsubscribe operation
 * @param completion_options Completion callback configuration.  Invoked when the corresponding UNSUBACK is received or
 * a failure condition is reached.  An error code implies complete failure of the unsubscribe, while a success code
 * implies the user must still check all of the UNSUBACK's reason codes for per-topic-filter feedback.
 * @return success/failure in the synchronous logic that kicks off the Unsubscribe operation
 */
AWS_MQTT_API
int aws_mqtt5_client_unsubscribe(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_packet_unsubscribe_view *unsubscribe_options,
    const struct aws_mqtt5_unsubscribe_completion_options *completion_options);

/**
 * Queries the client's internal statistics for incomplete operations.
 * @param client client to get statistics for
 * @param stats set of incomplete operation statistics
 */
AWS_MQTT_API
void aws_mqtt5_client_get_stats(struct aws_mqtt5_client *client, struct aws_mqtt5_client_operation_statistics *stats);

/* Misc related type APIs */

/**
 * Initializes the Client ID byte buf in negotiated settings
 *
 * @param allocator allocator to use for memory allocation
 * @param negotiated_settings settings to apply client id to
 * @param client_id client id to set
 */
AWS_MQTT_API int aws_mqtt5_negotiated_settings_init(
    struct aws_allocator *allocator,
    struct aws_mqtt5_negotiated_settings *negotiated_settings,
    const struct aws_byte_cursor *client_id);

/**
 * Makes an owning copy of a negotiated settings structure.
 *
 * @param source settings to copy from
 * @param dest settings to copy into.  Must be in a zeroed or initialized state because it gets clean up
 *  called on it as the first step of the copy process.
 * @return success/failure
 *
 * Used in downstream.
 */
AWS_MQTT_API int aws_mqtt5_negotiated_settings_copy(
    const struct aws_mqtt5_negotiated_settings *source,
    struct aws_mqtt5_negotiated_settings *dest);

/**
 * Clean up owned memory in negotiated_settings
 *
 * @param negotiated_settings settings to clean up
 */
AWS_MQTT_API void aws_mqtt5_negotiated_settings_clean_up(struct aws_mqtt5_negotiated_settings *negotiated_settings);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_MQTT_MQTT5_CLIENT_H */
