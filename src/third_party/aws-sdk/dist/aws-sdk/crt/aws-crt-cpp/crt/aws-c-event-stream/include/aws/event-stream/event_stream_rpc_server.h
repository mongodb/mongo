#ifndef AWS_EVENT_STREAM_RPC_SERVER_H
#define AWS_EVENT_STREAM_RPC_SERVER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/event-stream/event_stream_rpc.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_channel;
struct aws_event_stream_rpc_server_connection;

struct aws_event_stream_rpc_server_continuation_token;

/**
 * Invoked when a connection receives a message on an existing stream. message_args contains the
 * message data.
 */
typedef void(aws_event_stream_rpc_server_stream_continuation_fn)(
    struct aws_event_stream_rpc_server_continuation_token *token,
    const struct aws_event_stream_rpc_message_args *message_args,
    void *user_data);

/**
 * Invoked when a continuation has either been closed with the TERMINATE_STREAM flag, or when the connection
 * shutsdown and deletes the continuation.
 */
typedef void(aws_event_stream_rpc_server_stream_continuation_closed_fn)(
    struct aws_event_stream_rpc_server_continuation_token *token,
    void *user_data);

struct aws_event_stream_rpc_server_stream_continuation_options {
    aws_event_stream_rpc_server_stream_continuation_fn *on_continuation;
    aws_event_stream_rpc_server_stream_continuation_closed_fn *on_continuation_closed;
    void *user_data;
};

/**
 * Invoked when a non-stream level message is received on a connection.
 */
typedef void(aws_event_stream_rpc_server_connection_protocol_message_fn)(
    struct aws_event_stream_rpc_server_connection *connection,
    const struct aws_event_stream_rpc_message_args *message_args,
    void *user_data);

/**
 * Invoked when a new stream has been received on the connection. If you return AWS_OP_SUCCESS (0),
 * You must fill in the fields for continuation options or the program will assert and exit.
 *
 * A failure path MUST leave the ref count of the continuation alone.
 *
 * A success path should probably take a ref which will leave the continuation (assuming no other interference)
 * at two AFTER creation is complete: 1 for the connection's continuation table, and one for the callback
 * recipient which is presumably tracking it as well.
 */
typedef int(aws_event_stream_rpc_server_on_incoming_stream_fn)(
    struct aws_event_stream_rpc_server_connection *connection,
    struct aws_event_stream_rpc_server_continuation_token *token,
    struct aws_byte_cursor operation_name,
    struct aws_event_stream_rpc_server_stream_continuation_options *continuation_options,
    void *user_data);

struct aws_event_stream_rpc_connection_options {
    aws_event_stream_rpc_server_on_incoming_stream_fn *on_incoming_stream;
    aws_event_stream_rpc_server_connection_protocol_message_fn *on_connection_protocol_message;
    void *user_data;
};

/**
 * Invoked when a new connection is received on a server listener. If you return AWS_OP_SUCCESS,
 * You must fill in the fields for connection_options or the program will assert and exit.
 *
 * If error_code is non-zero, an error occurred upon setting up the channel and connection will be NULL. Otherwise,
 * connection is non-null. If you intend to seat a pointer to connection, you MUST call
 * aws_event_stream_rpc_server_connection_acquire() and when you're finished with the connection you MUST call
 * aws_event_stream_server_connection_release().
 */
typedef int(aws_event_stream_rpc_server_on_new_connection_fn)(
    struct aws_event_stream_rpc_server_connection *connection,
    int error_code,
    struct aws_event_stream_rpc_connection_options *connection_options,
    void *user_data);

/**
 * Invoked when a successfully created connection is shutdown. error_code will indicate the reason for the shutdown.
 */
typedef void(aws_event_stream_rpc_server_on_connection_shutdown_fn)(
    struct aws_event_stream_rpc_server_connection *connection,
    int error_code,
    void *user_data);

/**
 * Invoked whenever a message has been flushed to the channel.
 */
typedef void(aws_event_stream_rpc_server_message_flush_fn)(int error_code, void *user_data);

struct aws_server_bootstrap;
struct aws_event_stream_rpc_server_listener;

/**
 * (Optional). Invoked when the listener has been successfully shutdown (after the last ref count release).
 */
typedef void(aws_event_stream_rpc_server_on_listener_destroy_fn)(
    struct aws_event_stream_rpc_server_listener *server,
    void *user_data);

struct aws_event_stream_rpc_server_listener_options {
    /** host name to use for the listener. This depends on your socket type. */
    const char *host_name;
    /** port to use for your listener, assuming for the appropriate socket type. */
    uint32_t port;
    const struct aws_socket_options *socket_options;
    /** optional: tls options for using when setting up your server. */
    const struct aws_tls_connection_options *tls_options;
    struct aws_server_bootstrap *bootstrap;
    aws_event_stream_rpc_server_on_new_connection_fn *on_new_connection;
    aws_event_stream_rpc_server_on_connection_shutdown_fn *on_connection_shutdown;
    aws_event_stream_rpc_server_on_listener_destroy_fn *on_destroy_callback;
    void *user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Creates a listener with a ref count of 1. You are responsible for calling
 * aws_event_stream_rpc_server_listener_release() when you're finished with the listener. Returns NULL if an error
 * occurs.
 */
AWS_EVENT_STREAM_API struct aws_event_stream_rpc_server_listener *aws_event_stream_rpc_server_new_listener(
    struct aws_allocator *allocator,
    struct aws_event_stream_rpc_server_listener_options *options);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_listener_acquire(
    struct aws_event_stream_rpc_server_listener *listener);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_listener_release(
    struct aws_event_stream_rpc_server_listener *listener);

/**
 * Get the local port which the listener's socket is bound to.
 */
AWS_EVENT_STREAM_API
uint32_t aws_event_stream_rpc_server_listener_get_bound_port(
    const struct aws_event_stream_rpc_server_listener *listener);

/**
 * Bypasses server, and creates a connection on an already existing channel. No connection lifetime callbacks will be
 * invoked on the returned connection. Returns NULL if an error occurs. If and only if, you use this API, the returned
 * connection is already ref counted and you must call aws_event_stream_rpc_server_connection_release() even if you did
 * not explictly call aws_event_stream_rpc_server_connection_acquire()
 */
AWS_EVENT_STREAM_API struct aws_event_stream_rpc_server_connection *
    aws_event_stream_rpc_server_connection_from_existing_channel(
        struct aws_event_stream_rpc_server_listener *server,
        struct aws_channel *channel,
        const struct aws_event_stream_rpc_connection_options *connection_options);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_connection_acquire(
    struct aws_event_stream_rpc_server_connection *connection);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_connection_release(
    struct aws_event_stream_rpc_server_connection *connection);

AWS_EVENT_STREAM_API void *aws_event_stream_rpc_server_connection_get_user_data(
    struct aws_event_stream_rpc_server_connection *connection);
/**
 * returns true if the connection is open. False otherwise.
 */
AWS_EVENT_STREAM_API bool aws_event_stream_rpc_server_connection_is_open(
    struct aws_event_stream_rpc_server_connection *connection);

/**
 * Closes the connection (including all continuations on the connection), and releases the connection ref count.
 * shutdown_error_code is the error code to use when shutting down the channel. Use AWS_ERROR_SUCCESS for non-error
 * cases.
 */
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_connection_close(
    struct aws_event_stream_rpc_server_connection *connection,
    int shutdown_error_code);

/**
 * Sends a protocol message on the connection (not application data). If the message is valid and successfully written
 * to the channel, flush_fn will be invoked.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_server_connection_send_protocol_message(
    struct aws_event_stream_rpc_server_connection *connection,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_server_message_flush_fn *flush_fn,
    void *user_data);

AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_continuation_acquire(
    struct aws_event_stream_rpc_server_continuation_token *continuation);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_server_continuation_release(
    struct aws_event_stream_rpc_server_continuation_token *continuation);

/**
 * returns true if the continuation is still in an open state.
 */
AWS_EVENT_STREAM_API bool aws_event_stream_rpc_server_continuation_is_closed(
    struct aws_event_stream_rpc_server_continuation_token *continuation);

/**
 * Sends an application message on the continuation. If the message is valid and successfully written
 * to the channel, flush_fn will be invoked.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_server_continuation_send_message(
    struct aws_event_stream_rpc_server_continuation_token *continuation,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_server_message_flush_fn *flush_fn,
    void *user_data);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_EVENT_STREAM_RPC_SERVER_H */
