#ifndef AWS_MQTT_MQTT5_CLIENT_IMPL_H
#define AWS_MQTT_MQTT5_CLIENT_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/io/channel.h>
#include <aws/mqtt/private/v5/mqtt5_callbacks.h>
#include <aws/mqtt/private/v5/mqtt5_decoder.h>
#include <aws/mqtt/private/v5/mqtt5_encoder.h>
#include <aws/mqtt/private/v5/mqtt5_topic_alias.h>
#include <aws/mqtt/private/v5/rate_limiters.h>
#include <aws/mqtt/v5/mqtt5_types.h>

struct aws_event_loop;
struct aws_http_message;
struct aws_http_proxy_options;
struct aws_mqtt5_client_options_storage;
struct aws_mqtt5_operation;
struct aws_websocket_client_connection_options;

/**
 * The various states that the client can be in.  A client has both a current state and a desired state.
 * Desired state is only allowed to be one of {STOPPED, CONNECTED, TERMINATED}.  The client transitions states
 * based on either
 *  (1) changes in desired state, or
 *  (2) external events.
 *
 * Most states are interruptible (in the sense of a change in desired state causing an immediate change in state) but
 * CONNECTING and CHANNEL_SHUTDOWN cannot be interrupted due to waiting for an asynchronous callback (that has no
 * cancel) to complete.
 */
enum aws_mqtt5_client_state {

    /*
     * The client is not connected and not waiting for anything to happen.
     *
     * Next States:
     *    CONNECTING - if the user invokes Start() on the client
     *    TERMINATED - if the user releases the last ref count on the client
     */
    AWS_MCS_STOPPED,

    /*
     * The client is attempting to connect to a remote endpoint, and is waiting for channel setup to complete. This
     * state is not interruptible by any means other than channel setup completion.
     *
     * Next States:
     *    MQTT_CONNECT - if the channel completes setup with no error and desired state is still CONNECTED
     *    CHANNEL_SHUTDOWN - if the channel completes setup with no error, but desired state is not CONNECTED
     *    PENDING_RECONNECT - if the channel fails to complete setup and desired state is still CONNECTED
     *    STOPPED - if the channel fails to complete setup and desired state is not CONNECTED
     */
    AWS_MCS_CONNECTING,

    /*
     * The client is sending a CONNECT packet and waiting on a CONNACK packet.
     *
     * Next States:
     *    CONNECTED - if a successful CONNACK is received and desired state is still CONNECTED
     *    CHANNEL_SHUTDOWN - On send/encode errors, read/decode errors, unsuccessful CONNACK, timeout to receive
     *       CONNACK, desired state is no longer CONNECTED
     *    PENDING_RECONNECT - unexpected channel shutdown completion and desired state still CONNECTED
     *    STOPPED - unexpected channel shutdown completion and desired state no longer CONNECTED
     */
    AWS_MCS_MQTT_CONNECT,

    /*
     * The client is ready to perform user-requested mqtt operations.
     *
     * Next States:
     *    CHANNEL_SHUTDOWN - On send/encode errors, read/decode errors, DISCONNECT packet received, desired state
     *       no longer CONNECTED, PINGRESP timeout
     *    PENDING_RECONNECT - unexpected channel shutdown completion and desired state still CONNECTED
     *    STOPPED - unexpected channel shutdown completion and desired state no longer CONNECTED
     */
    AWS_MCS_CONNECTED,

    /*
     * The client is attempt to shut down a connection cleanly by finishing the current operation and then
     * transmitting an outbound DISCONNECT.
     *
     * Next States:
     *    CHANNEL_SHUTDOWN - on successful (or unsuccessful) send of the DISCONNECT
     *    PENDING_RECONNECT - unexpected channel shutdown completion and desired state still CONNECTED
     *    STOPPED - unexpected channel shutdown completion and desired state no longer CONNECTED
     */
    AWS_MCS_CLEAN_DISCONNECT,

    /*
     * The client is waiting for the io channel to completely shut down.  This state is not interruptible.
     *
     * Next States:
     *    PENDING_RECONNECT - the io channel has shut down and desired state is still CONNECTED
     *    STOPPED - the io channel has shut down and desired state is not CONNECTED
     */
    AWS_MCS_CHANNEL_SHUTDOWN,

    /*
     * The client is waiting for the reconnect timer to expire before attempting to connect again.
     *
     * Next States:
     *    CONNECTING - the reconnect timer has expired and desired state is still CONNECTED
     *    STOPPED - desired state is no longer CONNECTED
     */
    AWS_MCS_PENDING_RECONNECT,

    /*
     * The client is performing final shutdown and release of all resources.  This state is only realized for
     * a non-observable instant of time (transition out of STOPPED).
     */
    AWS_MCS_TERMINATED,
};

/**
 * Table of overridable external functions to allow mocking and monitoring of the client.
 */
struct aws_mqtt5_client_vtable {
    /* aws_high_res_clock_get_ticks */
    uint64_t (*get_current_time_fn)(void);

    /* aws_channel_shutdown */
    int (*channel_shutdown_fn)(struct aws_channel *channel, int error_code);

    /* aws_websocket_client_connect */
    int (*websocket_connect_fn)(const struct aws_websocket_client_connection_options *options);

    /* aws_client_bootstrap_new_socket_channel */
    int (*client_bootstrap_new_socket_channel_fn)(struct aws_socket_channel_bootstrap_options *options);

    /* aws_http_proxy_new_socket_channel */
    int (*http_proxy_new_socket_channel_fn)(
        struct aws_socket_channel_bootstrap_options *channel_options,
        const struct aws_http_proxy_options *proxy_options);

    /* This doesn't replace anything, it's just for test verification of state changes */
    void (*on_client_state_change_callback_fn)(
        struct aws_mqtt5_client *client,
        enum aws_mqtt5_client_state old_state,
        enum aws_mqtt5_client_state new_state,
        void *vtable_user_data);

    /* This doesn't replace anything, it's just for test verification of statistic changes */
    void (*on_client_statistics_changed_callback_fn)(
        struct aws_mqtt5_client *client,
        struct aws_mqtt5_operation *operation,
        void *vtable_user_data);

    /* aws_channel_acquire_message_from_pool */
    struct aws_io_message *(*aws_channel_acquire_message_from_pool_fn)(
        struct aws_channel *channel,
        enum aws_io_message_type message_type,
        size_t size_hint,
        void *user_data);

    /* aws_channel_slot_send_message */
    int (*aws_channel_slot_send_message_fn)(
        struct aws_channel_slot *slot,
        struct aws_io_message *message,
        enum aws_channel_direction dir,
        void *user_data);

    void *vtable_user_data;
};

/*
 * In order to make it easier to guarantee the lifecycle events are properly paired and emitted, we track
 * a separate state (from aws_mqtt5_client_state) and emit lifecycle events based on it.
 *
 * For example, if our lifecycle event is state CONNECTING, than anything going wrong becomes a CONNECTION_FAILED event
 * whereas if we were in  CONNECTED, it must be a DISCONNECTED event.  By setting the state to NONE after emitting
 * a CONNECTION_FAILED or DISCONNECTED event, then emission spots further down the execution pipeline will not
 * accidentally emit an additional event.  This also allows us to emit immediately when an event happens, if
 * appropriate, without having to persist additional event data (like packet views) until some singular point.
 *
 * For example:
 *
 * If I'm in CONNECTING and the channel shuts down, I want to emit a CONNECTION_FAILED event with the error code.
 * If I'm in CONNECTING and I receive a failed CONNACK, I want to emit a CONNECTION_FAILED event immediately with
 *   the CONNACK view in it and then invoke channel shutdown (and channel shutdown completing later should not emit an
 *   event).
 * If I'm in CONNECTED and the channel shuts down, I want to emit a DISCONNECTED event with the error code.
 * If I'm in CONNECTED and get a DISCONNECT packet from the server, I want to emit a DISCONNECTED event with
 *  the DISCONNECT packet in it, invoke channel shutdown,  and then I *don't* want to emit a DISCONNECTED event
 *  when the channel finishes shutting down.
 */
enum aws_mqtt5_lifecycle_state {
    AWS_MQTT5_LS_NONE,
    AWS_MQTT5_LS_CONNECTING,
    AWS_MQTT5_LS_CONNECTED,
};

/*
 * Operation-related state notes
 *
 * operation flow:
 *   (qos 0 publish, disconnect, connect)
 *      user (via cross thread task) ->
 *      queued_operations -> (on front of queue)
 *      current_operation -> (on completely encoded and passed to next handler)
 *      write_completion_operations -> (on socket write complete)
 *      release
 *
 *   (qos 1+ publish, sub/unsub)
 *      user (via cross thread task) ->
 *      queued_operations -> (on front of queue)
 *      current_operation (allocate packet id if necessary) -> (on completely encoded and passed to next handler)
 *      unacked_operations && unacked_operations_table -> (on ack received)
 *      release
 *
 *      QoS 1+ requires both a table and a list holding the same operations in order to support fast lookups by
 *      mqtt packet id and in-order re-queueing in the case of a disconnection (required by spec)
 *
 *   On Qos 1 PUBLISH completely received (and final callback invoked):
 *      Add PUBACK at head of queued_operations
 *
 *   On disconnect (on transition to PENDING_RECONNECT or STOPPED):
 *      If current_operation, move current_operation to head of queued_operations
 *      Fail all operations in the pending write completion list
 *      Fail, remove, and release operations in queued_operations where
 *         (1) They fail the offline queue policy OR
 *         (2) They are a PUBACK, PINGREQ, or DISCONNECT
 *      Fail, remove, and release unacked_operations if:
 *         (1) They fail the offline queue policy AND
 *         (2) operation is not Qos 1+ publish
 *
 *   On reconnect (post CONNACK):
 *      if rejoined_session:
 *          Move-and-append all non-qos1+-publishes in unacked_operations to the front of queued_operations
 *          Move-and-append remaining operations (qos1+ publishes) to the front of queued_operations
 *      else:
 *          Fail, remove, and release unacked_operations that fail the offline queue policy
 *          Move and append unacked operations to front of queued_operations
 *
 *      Clear unacked_operations_table
 */
struct aws_mqtt5_client_operational_state {

    /* back pointer to the client */
    struct aws_mqtt5_client *client;

    /*
     * One more than the most recently used packet id.  This is the best starting point for a forward search through
     * the id space for a free id.
     */
    aws_mqtt5_packet_id_t next_mqtt_packet_id;

    struct aws_linked_list queued_operations;
    struct aws_mqtt5_operation *current_operation;
    struct aws_hash_table unacked_operations_table;
    struct aws_linked_list unacked_operations;
    struct aws_linked_list write_completion_operations;

    /*
     * heap of operation pointers where the timeout is the sort value.  Elements are added/removed from this
     * data structure in exact synchronization with unacked_operations_table.
     */
    struct aws_priority_queue operations_by_ack_timeout;

    /*
     * Is there an io message in transit (to the socket) that has not invoked its write completion callback yet?
     * The client implementation only allows one in-transit message at a time, and so if this is true, we don't
     * send additional ones/
     */
    bool pending_write_completion;
};

/*
 * State related to flow-control rules for the mqtt5 client
 *
 * Includes:
 *   (1) Mqtt5 ReceiveMaximum support
 *   (2) AWS IoT Core limit support:
 *       (a) Publish TPS rate limit
 *       (b) Total outbound throughput limit
 */
struct aws_mqtt5_client_flow_control_state {

    /*
     * Mechanically follows the mqtt5 suggested implementation:
     *
     * Starts at the server's receive maximum.
     *   1. Decrement every time we send a QoS1+ publish
     *   2. Increment every time we receive a PUBACK
     *
     * Qos1+ publishes (and all operations behind them in the queue) are blocked while this value is zero.
     *
     * Qos 2 support will require additional work here to match the spec.
     */
    uint32_t unacked_publish_token_count;

    /*
     * Optional throttle (extended validation) that prevents the client from exceeding Iot Core's default throughput
     * limit
     */
    struct aws_rate_limiter_token_bucket throughput_throttle;

    /*
     * Optional throttle (extended validation) that prevents the client from exceeding Iot Core's default publish
     * rate limit.
     */
    struct aws_rate_limiter_token_bucket publish_throttle;
};

/**
 * Contains some simple statistics about the current state of the client's queue of operations
 */
struct aws_mqtt5_client_operation_statistics_impl {
    /*
     * total number of operations submitted to the client that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    struct aws_atomic_var incomplete_operation_count_atomic;

    /*
     * total packet size of operations submitted to the client that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    struct aws_atomic_var incomplete_operation_size_atomic;

    /*
     * total number of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    struct aws_atomic_var unacked_operation_count_atomic;

    /*
     * total packet size of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    struct aws_atomic_var unacked_operation_size_atomic;
};

struct aws_mqtt5_client {

    struct aws_allocator *allocator;
    struct aws_ref_count ref_count;

    const struct aws_mqtt5_client_vtable *vtable;

    /*
     * Client configuration
     */
    struct aws_mqtt5_client_options_storage *config;

    /*
     * The recurrent task that runs all client logic outside of external event callbacks.  Bound to the client's
     * event loop.
     */
    struct aws_task service_task;

    /*
     * Tracks when the client's service task is next schedule to run.  Is zero if the task is not scheduled to run or
     * we are in the middle of a service (so technically not scheduled too).
     */
    uint64_t next_service_task_run_time;

    /*
     * True if the client's service task is running.  Used to skip service task reevaluation due to state changes
     * while running the service task.  Reevaluation will occur at the very end of the service.
     */
    bool in_service;

    /*
     * The final mqtt5 settings negotiated between defaults, CONNECT, and CONNACK.  Only valid while in
     * CONNECTED or CLEAN_DISCONNECT states.
     */
    struct aws_mqtt5_negotiated_settings negotiated_settings;

    /*
     * Event loop all the client's connections and any related tasks will be pinned to, ensuring serialization and
     * concurrency safety.
     */
    struct aws_event_loop *loop;

    /* Channel handler information */
    struct aws_channel_handler handler;
    struct aws_channel_slot *slot;

    /*
     * What state is the client working towards?
     */
    enum aws_mqtt5_client_state desired_state;

    /*
     * What is the client's current state?
     */
    enum aws_mqtt5_client_state current_state;

    /*
     * The client's lifecycle state.  Used to correctly emit lifecycle events in spite of the complicated
     * async execution pathways that are possible.
     */
    enum aws_mqtt5_lifecycle_state lifecycle_state;

    /*
     * The client's MQTT packet encoder
     */
    struct aws_mqtt5_encoder encoder;

    /*
     * The client's MQTT packet decoder
     */
    struct aws_mqtt5_decoder decoder;

    /*
     * Cache of inbound topic aliases
     */
    struct aws_mqtt5_inbound_topic_alias_resolver inbound_topic_alias_resolver;

    /*
     * Cache of outbound topic aliases
     */
    struct aws_mqtt5_outbound_topic_alias_resolver *outbound_topic_alias_resolver;

    /*
     * Temporary state-related data.
     *
     * clean_disconnect_error_code - the CLEAN_DISCONNECT state takes time to complete and we want to be able
     * to pass an error code from a prior event to the channel shutdown.  This holds the "override" error code
     * that we'd like to shut down the channel with while CLEAN_DISCONNECT is processed.
     *
     * handshake exists on websocket-configured clients between the transform completion timepoint and the
     * websocket setup callback.
     */
    int clean_disconnect_error_code;
    struct aws_http_message *handshake;

    /*
     * Wraps all state related to pending and in-progress MQTT operations within the client.
     */
    struct aws_mqtt5_client_operational_state operational_state;

    /* Statistics tracking operational state */
    struct aws_mqtt5_client_operation_statistics_impl operation_statistics_impl;

    /*
     * Wraps all state related to outbound flow control.
     */
    struct aws_mqtt5_client_flow_control_state flow_control_state;

    /*
     * Manages notification listener chains for lifecycle events and incoming publishes
     */
    struct aws_mqtt5_callback_set_manager callback_manager;

    /*
     * When should the next PINGREQ be sent?
     */
    uint64_t next_ping_time;

    /*
     * When should we shut down the channel due to failure to receive a PINGRESP?  Only non-zero when an outstanding
     * PINGREQ has not been answered.
     */
    uint64_t next_ping_timeout_time;

    /*
     * When should the client next attempt to reconnect?  Only used by PENDING_RECONNECT state.
     */
    uint64_t next_reconnect_time_ns;

    /*
     * How many consecutive reconnect failures have we experienced?
     */
    uint64_t reconnect_count;

    /*
     * How much should we wait before our next reconnect attempt?
     */
    uint64_t current_reconnect_delay_ms;

    /*
     * When should the client reset current_reconnect_delay_interval_ms to the minimum value?  Only relevant to the
     * CONNECTED state.
     */
    uint64_t next_reconnect_delay_reset_time_ns;

    /*
     * When should we shut down the channel due to failure to receive a CONNACK?  Only relevant during the MQTT_CONNECT
     * state.
     */
    uint64_t next_mqtt_connect_packet_timeout_time;

    /*
     * Starts false and set to true as soon as a successful connection is established.  If the session resumption
     * behavior is AWS_MQTT5_CSBT_REJOIN_POST_SUCCESS then this must be true before the client sends CONNECT packets
     * with clean start set to false.
     */
    bool has_connected_successfully;

    /*
     * A flag that allows in-thread observers (currently the mqtt3_to_5 adapter) to signal that the connection
     * should be torn down and re-established.  Only relevant to the CONNECTING state which is not interruptible:
     *
     * If the mqtt5 client is in the CONNECTING state (ie waiting for bootstrap to complete) and the 3-adapter
     * is asked to connect, then we *MUST* discard the in-progress connection attempt in order to guarantee the
     * connection we establish uses all of the configuration parameters that are passed during the mqtt3 API's connect
     * call (host, port, tls options, socket options, etc...).  Since we can't interrupt the CONNECTING state, we
     * instead set a flag that tells the mqtt5 client to tear down the connection as soon as the initial bootstrap
     * completes.  The reconnect will establish the requested connection using the parameters passed to
     * the mqtt3 API.
     *
     * Rather than try and catch every escape path from CONNECTING, we lazily reset this flag to false when we
     * enter the CONNECTING state.  On a similar note, we only check this flag as we transition to MQTT_CONNECT.
     *
     * This flag is ultimately only needed when the 3 adapter and 5 client are used out-of-sync.  If you use the
     * 3 adapter exclusively after 5 client creation, it never comes into play.
     *
     * Even the adapter shouldn't manipulate this directly.  Instead, use the aws_mqtt5_client_reset_connection private
     * API to tear down an in-progress or established connection in response to a connect() request on the adapter.
     */
    bool should_reset_connection;
};

AWS_EXTERN_C_BEGIN

/*
 * A number of private APIs which are either set up for mocking parts of the client or testing subsystems within it by
 * exposing what would normally be static functions internal to the implementation.
 */

/*
 * Override the vtable used by the client; useful for mocking certain scenarios.
 */
AWS_MQTT_API void aws_mqtt5_client_set_vtable(
    struct aws_mqtt5_client *client,
    const struct aws_mqtt5_client_vtable *vtable);

/*
 * Gets the default vtable used by the client.  In order to mock something, we start with the default and then
 * mutate it selectively to achieve the scenario we're interested in.
 */
AWS_MQTT_API const struct aws_mqtt5_client_vtable *aws_mqtt5_client_get_default_vtable(void);

/*
 * Sets the packet id, if necessary, on an operation based on the current pending acks table.  The caller is
 * responsible for adding the operation to the unacked table when the packet has been encoding in an io message.
 *
 * There is an argument that the operation should go into the table only on socket write completion, but that breaks
 * allocation unless an additional, independent table is added, which I'd prefer not to do presently.  Also, socket
 * write completion callbacks can be a bit delayed which could lead to a situation where the response from a local
 * server could arrive before the write completion runs which would be a disaster.
 */
AWS_MQTT_API int aws_mqtt5_operation_bind_packet_id(
    struct aws_mqtt5_operation *operation,
    struct aws_mqtt5_client_operational_state *client_operational_state);

/*
 * Initialize and clean up of the client operational state.  Exposed (privately) to enabled tests to reuse the
 * init/cleanup used by the client itself.
 */
AWS_MQTT_API int aws_mqtt5_client_operational_state_init(
    struct aws_mqtt5_client_operational_state *client_operational_state,
    struct aws_allocator *allocator,
    struct aws_mqtt5_client *client);

AWS_MQTT_API void aws_mqtt5_client_operational_state_clean_up(
    struct aws_mqtt5_client_operational_state *client_operational_state);

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
 */
AWS_MQTT_API void aws_mqtt5_client_on_disconnection_update_operational_state(struct aws_mqtt5_client *client);

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
AWS_MQTT_API void aws_mqtt5_client_on_connection_update_operational_state(struct aws_mqtt5_client *client);

/*
 * Processes the pending operation queue based on the current state of the associated client
 */
AWS_MQTT_API int aws_mqtt5_client_service_operational_state(
    struct aws_mqtt5_client_operational_state *client_operational_state);

/*
 * Updates the client's operational state based on the receipt of an ACK packet from the server.  In general this
 * means looking up the original operation in the pending ack table, completing it, removing it from both the
 * pending ack table and list, and then destroying it.
 */
AWS_MQTT_API void aws_mqtt5_client_operational_state_handle_ack(
    struct aws_mqtt5_client_operational_state *client_operational_state,
    aws_mqtt5_packet_id_t packet_id,
    enum aws_mqtt5_packet_type packet_type,
    const void *packet_view,
    int error_code);

/*
 * Helper function that returns whether or not the current value of the negotiated settings can be used.  Primarily
 * a client state check (received CONNACK, not yet disconnected)
 */
AWS_MQTT_API bool aws_mqtt5_client_are_negotiated_settings_valid(const struct aws_mqtt5_client *client);

/*
 * Initializes the client's flow control state.  This state governs the rates and delays between processing
 * operations and sending packets.
 */
AWS_MQTT_API void aws_mqtt5_client_flow_control_state_init(struct aws_mqtt5_client *client);

/*
 * Resets the client's flow control state to a known baseline.  Invoked right after entering the connected state.
 */
AWS_MQTT_API void aws_mqtt5_client_flow_control_state_reset(struct aws_mqtt5_client *client);

/*
 * Updates the client's flow control state based on the receipt of a PUBACK for a Qos1 publish.
 */
AWS_MQTT_API void aws_mqtt5_client_flow_control_state_on_puback(struct aws_mqtt5_client *client);

/*
 * Updates the client's flow control state based on successfully encoding an operation into a channel message.
 */
AWS_MQTT_API void aws_mqtt5_client_flow_control_state_on_outbound_operation(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation);

/*
 * Given the next operation in the queue, examines the flow control state to determine when is the earliest time
 * it should be processed.
 */
AWS_MQTT_API uint64_t aws_mqtt5_client_flow_control_state_get_next_operation_service_time(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    uint64_t now);

/*
 * Updates the client's operation statistics based on a change in the state of an operation.
 */
AWS_MQTT_API void aws_mqtt5_client_statistics_change_operation_statistic_state(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    enum aws_mqtt5_operation_statistic_state_flags new_state_flags);

/**
 * Converts a client state type to a readable description.
 *
 * @param state client state
 * @return short string describing the client state
 */
AWS_MQTT_API const char *aws_mqtt5_client_state_to_c_string(enum aws_mqtt5_client_state state);

/**
 * An internal API used by the MQTT3 adapter to force any existing-or-in-progress connection to
 * be torn down and re-established.  Necessary because the MQTT3 interface allows overrides on a large number
 * of configuration parameters through the connect() call.  We must honor those parameters and the safest thing
 * to do is to just throw away the current connection (if it exists) and make a new one.  In the case that an MQTT5
 * client is being driven entirely by the MQTT3 adapter, this case never actually happens.
 *
 * @param client client to reset an existing or in-progress connection for
 * @return true if a connection reset was triggered, false if there was nothing to do
 */
AWS_MQTT_API bool aws_mqtt5_client_reset_connection(struct aws_mqtt5_client *client);

/**
 * Event-loop-internal API used to switch the client's desired state.  Used by both start() and stop() cross-thread
 * tasks as well as by the 3-to-5 adapter to make changes synchronously (when in the event loop).
 *
 * @param client mqtt5 client to update desired state for
 * @param desired_state new desired state
 * @param disconnect_op optional description of a DISCONNECT packet to send as part of a stop command
 */
AWS_MQTT_API void aws_mqtt5_client_change_desired_state(
    struct aws_mqtt5_client *client,
    enum aws_mqtt5_client_state desired_state,
    struct aws_mqtt5_operation_disconnect *disconnect_op);

/**
 * Event-loop-internal API to add an operation to the client's queue.  Used by the 3-to-5 adapter to synchnrously
 * inject the MQTT5 operation once the adapter operation has reached the event loop.
 *
 * @param client MQTT5 client to submit an operation to
 * @param operation MQTT5 operation to submit
 * @param is_terminated flag that indicates whether the submitter is shutting down or not.  Needed to differentiate
 * between adapter submissions and MQTT5 client API submissions and correctly handle ref count adjustments.
 */
AWS_MQTT_API void aws_mqtt5_client_submit_operation_internal(
    struct aws_mqtt5_client *client,
    struct aws_mqtt5_operation *operation,
    bool is_terminated);

AWS_EXTERN_C_END

#endif /* AWS_MQTT_MQTT5_CLIENT_IMPL_H */
