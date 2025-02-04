#ifndef AWS_MQTT_PRIVATE_CLIENT_IMPL_H
#define AWS_MQTT_PRIVATE_CLIENT_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/mqtt/client.h>

#include <aws/mqtt/private/client_impl_shared.h>
#include <aws/mqtt/private/fixed_header.h>
#include <aws/mqtt/private/mqtt311_decoder.h>
#include <aws/mqtt/private/mqtt311_listener.h>
#include <aws/mqtt/private/topic_tree.h>

#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/task_scheduler.h>

#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/message_pool.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>

struct aws_mqtt_client_connection_311_impl;

#define MQTT_CLIENT_CALL_CALLBACK(client_ptr, callback)                                                                \
    do {                                                                                                               \
        if ((client_ptr)->callback) {                                                                                  \
            (client_ptr)->callback((&client_ptr->base), (client_ptr)->callback##_ud);                                  \
        }                                                                                                              \
    } while (false)
#define MQTT_CLIENT_CALL_CALLBACK_ARGS(client_ptr, callback, ...)                                                      \
    do {                                                                                                               \
        if ((client_ptr)->callback) {                                                                                  \
            (client_ptr)->callback((&client_ptr->base), __VA_ARGS__, (client_ptr)->callback##_ud);                     \
        }                                                                                                              \
    } while (false)

#if ASSERT_LOCK_HELD
#    define ASSERT_SYNCED_DATA_LOCK_HELD(object)                                                                       \
        {                                                                                                              \
            int cached_error = aws_last_error();                                                                       \
            AWS_ASSERT(aws_mutex_try_lock(&(object)->synced_data.lock) == AWS_OP_ERR);                                 \
            aws_raise_error(cached_error);                                                                             \
        }
#else
#    define ASSERT_SYNCED_DATA_LOCK_HELD(object)
#endif

enum aws_mqtt_client_connection_state {
    AWS_MQTT_CLIENT_STATE_CONNECTING,
    AWS_MQTT_CLIENT_STATE_CONNECTED,
    AWS_MQTT_CLIENT_STATE_RECONNECTING,
    AWS_MQTT_CLIENT_STATE_DISCONNECTING,
    AWS_MQTT_CLIENT_STATE_DISCONNECTED,
};

enum aws_mqtt_client_request_state {
    AWS_MQTT_CLIENT_REQUEST_ONGOING,
    AWS_MQTT_CLIENT_REQUEST_COMPLETE,
    AWS_MQTT_CLIENT_REQUEST_ERROR,
};

/**
 * Contains some simple statistics about the current state of the connection's queue of operations
 */
struct aws_mqtt_connection_operation_statistics_impl {
    /**
     * total number of operations submitted to the connection that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    struct aws_atomic_var incomplete_operation_count_atomic;

    /**
     * total packet size of operations submitted to the connection that have not yet been completed.  Unacked operations
     * are a subset of this.
     */
    struct aws_atomic_var incomplete_operation_size_atomic;

    /**
     * total number of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    struct aws_atomic_var unacked_operation_count_atomic;

    /**
     * total packet size of operations that have been sent to the server and are waiting for a corresponding ACK before
     * they can be completed.
     */
    struct aws_atomic_var unacked_operation_size_atomic;
};

/**
 * Called after the timeout if a matching ack packet hasn't arrived, with is_first_attempt set as false.
 * Or called when the request packet attempt to send firstly, with is_first_attempt set as true.
 * Return AWS_MQTT_CLIENT_REQUEST_ONGOING to check on the task later.
 * Return AWS_MQTT_CLIENT_REQUEST_COMPLETE to consider request complete.
 * Return AWS_MQTT_CLIENT_REQUEST_ERROR cancel the task and report an error to the caller.
 */
typedef enum aws_mqtt_client_request_state(
    aws_mqtt_send_request_fn)(uint16_t packet_id, bool is_first_attempt, void *userdata);

/**
 * Called when the operation statistics change.
 */
typedef void(
    aws_mqtt_on_operation_statistics_fn)(struct aws_mqtt_client_connection_311_impl *connection, void *userdata);

/* Flags that indicate the way in which way an operation is currently affecting the statistics of the connection */
enum aws_mqtt_operation_statistic_state_flags {
    /* The operation is not affecting the connection's statistics at all */
    AWS_MQTT_OSS_NONE = 0,

    /* The operation is affecting the connection's "incomplete operation" statistics */
    AWS_MQTT_OSS_INCOMPLETE = 1 << 0,

    /* The operation is affecting the connection's "unacked operation" statistics */
    AWS_MQTT_OSS_UNACKED = 1 << 1,
};

struct aws_mqtt_request {
    struct aws_linked_list_node list_node;

    struct aws_allocator *allocator;
    struct aws_mqtt_client_connection_311_impl *connection;

    struct aws_channel_task outgoing_task;

    /*
     * The request send time. Currently used to push off keepalive packet.
     */
    uint64_t request_send_timestamp;

    /* How this operation is currently affecting the statistics of the connection */
    enum aws_mqtt_operation_statistic_state_flags statistic_state_flags;
    /* The encoded size of the packet - used for operation statistics tracking */
    uint64_t packet_size;

    uint16_t packet_id;
    bool retryable;
    bool initiated;
    aws_mqtt_send_request_fn *send_request;
    void *send_request_ud;
    aws_mqtt_op_complete_fn *on_complete;
    void *on_complete_ud;
};

struct aws_mqtt_reconnect_task {
    struct aws_task task;
    struct aws_atomic_var connection_ptr;
    struct aws_allocator *allocator;
};

struct request_timeout_wrapper;

/* used for timeout task */
struct request_timeout_task_arg {
    uint16_t packet_id;
    struct aws_mqtt_client_connection_311_impl *connection;
    struct request_timeout_wrapper *task_arg_wrapper;
};

/*
 * We want the timeout task to be able to destroy the forward reference from the operation's task arg structure
 * to the timeout task.  But the operation task arg structures don't have any data structure in common.  So to allow
 * the timeout to refer back to a zero-able forward pointer, we wrap a pointer to the timeout task and embed it
 * in every operation's task arg that needs to create a timeout.
 */
struct request_timeout_wrapper {
    struct request_timeout_task_arg *timeout_task_arg;
};

/* The lifetime of this struct is from subscribe -> suback */
struct subscribe_task_arg {

    struct aws_mqtt_client_connection_311_impl *connection;

    /* list of pointer of subscribe_task_topics */
    struct aws_array_list topics;

    /* Packet to populate */
    struct aws_mqtt_packet_subscribe subscribe;

    /* true if transaction was committed to the topic tree, false requires a retry */
    bool tree_updated;

    struct {
        aws_mqtt_suback_multi_fn *multi;
        aws_mqtt_suback_fn *single;
    } on_suback;
    void *on_suback_ud;

    struct request_timeout_wrapper timeout_wrapper;
    uint64_t timeout_duration_in_ns;
};

/* The lifetime of this struct is the same as the lifetime of the subscription */
struct subscribe_task_topic {
    struct aws_mqtt_client_connection_311_impl *connection;

    struct aws_mqtt_topic_subscription request;
    struct aws_string *filter;

    struct aws_ref_count ref_count;
};

struct aws_mqtt_client_connection_311_impl {
    struct aws_allocator *allocator;

    struct aws_mqtt_client_connection base;

    struct aws_ref_count ref_count;

    struct aws_mqtt_client *client;

    /* Channel handler information */
    struct aws_channel_handler handler;
    struct aws_channel_slot *slot;

    /* The host information, changed by user when state is AWS_MQTT_CLIENT_STATE_DISCONNECTED */
    struct aws_string *host_name;
    uint32_t port;
    struct aws_tls_connection_options tls_options;
    struct aws_socket_options socket_options;
    struct aws_http_proxy_config *http_proxy_config;
    struct aws_event_loop *loop;
    struct aws_host_resolution_config host_resolution_config;

    /* Connect parameters */
    struct aws_byte_buf client_id;
    bool clean_session;
    uint16_t keep_alive_time_secs;
    uint64_t keep_alive_time_ns;
    uint64_t ping_timeout_ns;
    uint64_t operation_timeout_ns;
    struct aws_string *username;
    struct aws_string *password;
    struct {
        struct aws_byte_buf topic;
        enum aws_mqtt_qos qos;
        bool retain;
        struct aws_byte_buf payload;
    } will;
    struct {
        uint64_t current_sec; /* seconds */
        uint64_t min_sec;     /* seconds */
        uint64_t max_sec;     /* seconds */

        /*
         * Invariant: this is always zero except when the current MQTT channel has received a successful connack
         * and is not yet shutdown.  During that interval, it is the timestamp the connack was received.
         */
        uint64_t channel_successful_connack_timestamp_ns;
    } reconnect_timeouts;

    /* User connection callbacks */
    aws_mqtt_client_on_connection_complete_fn *on_connection_complete;
    void *on_connection_complete_ud;
    aws_mqtt_client_on_connection_success_fn *on_connection_success;
    void *on_connection_success_ud;
    aws_mqtt_client_on_connection_failure_fn *on_connection_failure;
    void *on_connection_failure_ud;
    aws_mqtt_client_on_connection_interrupted_fn *on_interrupted;
    void *on_interrupted_ud;
    aws_mqtt_client_on_connection_resumed_fn *on_resumed;
    void *on_resumed_ud;
    aws_mqtt_client_on_connection_closed_fn *on_closed;
    void *on_closed_ud;
    aws_mqtt_client_publish_received_fn *on_any_publish;
    void *on_any_publish_ud;
    aws_mqtt_client_on_disconnect_fn *on_disconnect;
    void *on_disconnect_ud;
    aws_mqtt_client_on_connection_termination_fn *on_termination;
    void *on_termination_ud;
    aws_mqtt_on_operation_statistics_fn *on_any_operation_statistics;
    void *on_any_operation_statistics_ud;

    /* listener callbacks */
    struct aws_mqtt311_callback_set_manager callback_manager;

    /* Connection tasks. */
    struct aws_mqtt_reconnect_task *reconnect_task;
    struct aws_channel_task ping_task;

    /**
     * Number of times this connection has successfully CONNACK-ed, used
     * to ensure on_connection_completed is sent on the first completed
     * CONNECT/CONNACK cycle
     */
    size_t connection_count;
    bool use_tls; /* Only used by main thread */

    /* Only the event-loop thread may touch this data */
    struct {
        struct aws_mqtt311_decoder decoder;

        bool waiting_on_ping_response;

        /* Keeps track of all open subscriptions */
        /* TODO: The subscriptions are liveing with the connection object. So if the connection disconnect from one
         * endpoint and connect with another endpoint, the subscription tree will still be the same as before. */
        struct aws_mqtt_topic_tree subscriptions;

        /**
         * List of all requests waiting for response.
         */
        struct aws_linked_list ongoing_requests_list;
    } thread_data;

    /* Any thread may touch this data, but the lock must be held (unless it's an atomic) */
    struct {
        /* Note: never fire user callback with lock hold. */
        struct aws_mutex lock;

        /* The state of the connection */
        enum aws_mqtt_client_connection_state state;

        /**
         * Memory pool for all aws_mqtt_request.
         */
        struct aws_memory_pool requests_pool;

        /**
         * Store all requests that is not completed including the pending requests.
         *
         * hash table from uint16_t (packet_id) to aws_mqtt_outstanding_request
         */
        struct aws_hash_table outstanding_requests_table;

        /**
         * List of all requests that cannot be scheduled until the connection comes online.
         */
        struct aws_linked_list pending_requests_list;

        /**
         * Remember the last packet ID assigned.
         * Helps us find the next free ID faster.
         */
        uint16_t packet_id;

    } synced_data;

    struct {
        aws_mqtt_transform_websocket_handshake_fn *handshake_transformer;
        void *handshake_transformer_ud;
        aws_mqtt_validate_websocket_handshake_fn *handshake_validator;
        void *handshake_validator_ud;
        bool enabled;

        struct aws_http_message *handshake_request;
    } websocket;

    /**
     * The time that the next ping task should execute at. Note that this does not mean that
     * this IS when the ping task will execute, but rather that this is when the next ping
     * SHOULD execute. There may be an already scheduled PING task that will elapse sooner
     * than this time that has to be rescheduled.
     */
    uint64_t next_ping_time;

    /**
     * Statistics tracking operational state
     */
    struct aws_mqtt_connection_operation_statistics_impl operation_statistics_impl;
};

struct aws_channel_handler_vtable *aws_mqtt_get_client_channel_vtable(void);

/* Helper for getting a message object for a packet */
struct aws_io_message *mqtt_get_message_for_packet(
    struct aws_mqtt_client_connection_311_impl *connection,
    struct aws_mqtt_fixed_header *header);

void mqtt_connection_lock_synced_data(struct aws_mqtt_client_connection_311_impl *connection);
void mqtt_connection_unlock_synced_data(struct aws_mqtt_client_connection_311_impl *connection);

/* Note: needs to be called with lock held. */
void mqtt_connection_set_state(
    struct aws_mqtt_client_connection_311_impl *connection,
    enum aws_mqtt_client_connection_state state);

/**
 * This function registers a new outstanding request and returns the message identifier to use (or 0 on error).
 * send_request will be called from request_timeout_task if everything succeed. Not called with error.
 * on_complete will be called once the request completed, either either in success or error.
 * noRetry is true for the packets will never be retried or offline queued.
 */
AWS_MQTT_API uint16_t mqtt_create_request(
    struct aws_mqtt_client_connection_311_impl *connection,
    aws_mqtt_send_request_fn *send_request,
    void *send_request_ud,
    aws_mqtt_op_complete_fn *on_complete,
    void *on_complete_ud,
    bool noRetry,
    uint64_t packet_size);

/* Call when an ack packet comes back from the server. */
AWS_MQTT_API void mqtt_request_complete(
    struct aws_mqtt_client_connection_311_impl *connection,
    int error_code,
    uint16_t packet_id);

/* Call to close the connection with an error code */
AWS_MQTT_API void mqtt_disconnect_impl(struct aws_mqtt_client_connection_311_impl *connection, int error_code);

/* Creates the task used to reestablish a broken connection */
AWS_MQTT_API void aws_create_reconnect_task(struct aws_mqtt_client_connection_311_impl *connection);

/**
 * Sets the callback to call whenever the operation statistics change.
 *
 * \param[in] connection                  The connection object
 * \param[in] on_operation_statistics     The function to call when the operation statistics change (pass NULL to unset)
 * \param[in] on_operation_statistics_ud  Userdata for on_operation_statistics
 */
AWS_MQTT_API int aws_mqtt_client_connection_set_on_operation_statistics_handler(
    struct aws_mqtt_client_connection_311_impl *connection,
    aws_mqtt_on_operation_statistics_fn *on_operation_statistics,
    void *on_operation_statistics_ud);

/*
 * Sends a PINGREQ packet to the server to keep the connection alive. This is not exported and should not ever
 * be called directly. This function is driven by the timeout values passed to aws_mqtt_client_connect().
 * If a PINGRESP is not received within a reasonable period of time, the connection will be closed.
 *
 * \params[in] connection   The connection to ping on
 *
 * \returns AWS_OP_SUCCESS if the connection is open and the PINGREQ is sent or queued to send,
 *              otherwise AWS_OP_ERR and aws_last_error() is set.
 */
int aws_mqtt_client_connection_ping(struct aws_mqtt_client_connection_311_impl *connection);

/**
 * Changes the operation statistics for the passed-in aws_mqtt_request. Used for tracking
 * whether operations have been completed or not.
 *
 * NOTE: This function will get lock the synced data! Do NOT call with the synced data already
 * held or the function will deadlock trying to get the lock
 *
 * @param connection The connection whose operations are being tracked
 * @param request The request to change the state of
 * @param new_state_flags The new state to use
 */
void aws_mqtt_connection_statistics_change_operation_statistic_state(
    struct aws_mqtt_client_connection_311_impl *connection,
    struct aws_mqtt_request *request,
    enum aws_mqtt_operation_statistic_state_flags new_state_flags);

AWS_MQTT_API const struct aws_mqtt_client_connection_packet_handlers *aws_mqtt311_get_default_packet_handlers(void);

AWS_MQTT_API uint16_t aws_mqtt_client_connection_311_unsubscribe(
    struct aws_mqtt_client_connection_311_impl *connection,
    const struct aws_byte_cursor *topic_filter,
    aws_mqtt_op_complete_fn *on_unsuback,
    void *on_unsuback_ud,
    uint64_t timeout_ns);

AWS_MQTT_API uint16_t aws_mqtt_client_connection_311_subscribe(
    struct aws_mqtt_client_connection_311_impl *connection,
    const struct aws_byte_cursor *topic_filter,
    enum aws_mqtt_qos qos,
    aws_mqtt_client_publish_received_fn *on_publish,
    void *on_publish_ud,
    aws_mqtt_userdata_cleanup_fn *on_ud_cleanup,
    aws_mqtt_suback_fn *on_suback,
    void *on_suback_ud,
    uint64_t timeout_ns);

AWS_MQTT_API uint16_t aws_mqtt_client_connection_311_publish(
    struct aws_mqtt_client_connection_311_impl *connection,
    const struct aws_byte_cursor *topic,
    enum aws_mqtt_qos qos,
    bool retain,
    const struct aws_byte_cursor *payload,
    aws_mqtt_op_complete_fn *on_complete,
    void *userdata,
    uint64_t timeout_ns);

#endif /* AWS_MQTT_PRIVATE_CLIENT_IMPL_H */
