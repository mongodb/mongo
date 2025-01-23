#ifndef AWS_EVENT_STREAM_RPC_CLIENT_H
#define AWS_EVENT_STREAM_RPC_CLIENT_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/event-stream/event_stream_rpc.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_channel;
struct aws_event_stream_rpc_client_connection;
struct aws_event_stream_rpc_client_continuation_token;

/**
 * Invoked when a connection receives a message on an existing stream. message_args contains the
 * message data.
 */
typedef void(aws_event_stream_rpc_client_stream_continuation_fn)(
    struct aws_event_stream_rpc_client_continuation_token *token,
    const struct aws_event_stream_rpc_message_args *message_args,
    void *user_data);

/**
 * Invoked when a continuation has either been closed with the TERMINATE_STREAM flag, or when the connection
 * shuts down and deletes the continuation.
 */
typedef void(aws_event_stream_rpc_client_stream_continuation_closed_fn)(
    struct aws_event_stream_rpc_client_continuation_token *token,
    void *user_data);

struct aws_event_stream_rpc_client_stream_continuation_options {
    aws_event_stream_rpc_client_stream_continuation_fn *on_continuation;
    aws_event_stream_rpc_client_stream_continuation_closed_fn *on_continuation_closed;
    void *user_data;
};

/**
 * Invoked when a non-stream level message is received on a connection.
 */
typedef void(aws_event_stream_rpc_client_connection_protocol_message_fn)(
    struct aws_event_stream_rpc_client_connection *connection,
    const struct aws_event_stream_rpc_message_args *message_args,
    void *user_data);

/**
 * Invoked when a successfully created connection is shutdown. error_code will indicate the reason for the shutdown.
 */
typedef void(aws_event_stream_rpc_client_on_connection_shutdown_fn)(
    struct aws_event_stream_rpc_client_connection *connection,
    int error_code,
    void *user_data);

/**
 * Invoked when a connection attempt completes.
 *
 * If the attempt was unsuccessful, the error_code will be non-zero and the connection pointer will be NULL,
 * and aws_event_stream_rpc_client_on_connection_shutdown_fn will not be invoked.
 *
 * If the attempt was successful, error_code will be 0 and the connection pointer will be valid.
 * You must call aws_event_stream_rpc_client_connection_acquire()
 * to prevent the pointer's memory from being destroyed before you are ready.
 * When you are completely done with the connection pointer you must call
 * aws_event_stream_rpc_client_connection_release() or its memory will leak.
 * aws_event_stream_rpc_client_on_connection_shutdown_fn will be invoked
 * when the network connection has closed. If you are done with the connection,
 * but it is still open, you must call aws_aws_event_stream_rpc_client_close()
 * or network connection will remain open, even if you call release().
 */
typedef void(aws_event_stream_rpc_client_on_connection_setup_fn)(
    struct aws_event_stream_rpc_client_connection *connection,
    int error_code,
    void *user_data);

/**
 * Invoked whenever a message has been flushed to the channel.
 */
typedef void(aws_event_stream_rpc_client_message_flush_fn)(int error_code, void *user_data);

struct aws_client_bootstrap;

struct aws_event_stream_rpc_client_connection_options {
    /** host name to use for the connection. This depends on your socket type. */
    const char *host_name;
    /** port to use for your connection, assuming for the appropriate socket type. */
    uint32_t port;
    /** socket options for establishing the connection to the RPC server. */
    const struct aws_socket_options *socket_options;
    /** optional: tls options for using when establishing your connection. */
    const struct aws_tls_connection_options *tls_options;
    struct aws_client_bootstrap *bootstrap;
    aws_event_stream_rpc_client_on_connection_setup_fn *on_connection_setup;
    aws_event_stream_rpc_client_connection_protocol_message_fn *on_connection_protocol_message;
    aws_event_stream_rpc_client_on_connection_shutdown_fn *on_connection_shutdown;
    void *user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Initiate a new connection. If this function returns AWS_OP_SUCESSS, the
 * aws_event_stream_rpc_client_connection_options::on_connection_setup is guaranteed to be called exactly once. If that
 * callback successfully creates a connection, aws_event_stream_rpc_client_connection_options::on_connection_shutdown
 * will be invoked upon connection closure. However if the connection was never successfully setup,
 * aws_event_stream_rpc_client_connection_options::on_connection_shutdown will not be invoked later.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_client_connection_connect(
    struct aws_allocator *allocator,
    const struct aws_event_stream_rpc_client_connection_options *conn_options);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_client_connection_acquire(
    const struct aws_event_stream_rpc_client_connection *connection);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_client_connection_release(
    const struct aws_event_stream_rpc_client_connection *connection);

/**
 * Closes the connection if it is open and aws_event_stream_rpc_client_connection_options::on_connection_shutdown will
 * be invoked upon shutdown. shutdown_error_code will indicate the reason for shutdown. For a graceful shutdown pass 0
 * or AWS_ERROR_SUCCESS.
 */
AWS_EVENT_STREAM_API void aws_event_stream_rpc_client_connection_close(
    struct aws_event_stream_rpc_client_connection *connection,
    int shutdown_error_code);

/**
 * Returns true if the connection is open, false otherwise.
 */
AWS_EVENT_STREAM_API bool aws_event_stream_rpc_client_connection_is_open(
    const struct aws_event_stream_rpc_client_connection *connection);

/**
 * Sends a message on the connection. These must be connection level messages (not application messages).
 *
 * flush_fn will be invoked when the message has been successfully writen to the wire or when it fails.
 *
 * returns AWS_OP_SUCCESS if the message was successfully created and queued, and in that case flush_fn will always be
 * invoked. Otherwise, flush_fn will not be invoked.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_client_connection_send_protocol_message(
    struct aws_event_stream_rpc_client_connection *connection,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_client_message_flush_fn *flush_fn,
    void *user_data);

/**
 * Create a new stream. continuation_option's callbacks will not be invoked, and nothing will be sent across the wire
 * until aws_event_stream_rpc_client_continuation_activate() is invoked.
 *
 * returns an instance of a aws_event_stream_rpc_client_continuation_token on success with a reference count of 1. You
 * must call aws_event_stream_rpc_client_continuation_release() when you're finished with it. Returns NULL on failure.
 */
AWS_EVENT_STREAM_API struct aws_event_stream_rpc_client_continuation_token *
    aws_event_stream_rpc_client_connection_new_stream(
        struct aws_event_stream_rpc_client_connection *connection,
        const struct aws_event_stream_rpc_client_stream_continuation_options *continuation_options);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_client_continuation_acquire(
    const struct aws_event_stream_rpc_client_continuation_token *continuation);
AWS_EVENT_STREAM_API void aws_event_stream_rpc_client_continuation_release(
    const struct aws_event_stream_rpc_client_continuation_token *continuation);

/**
 * returns true if the continuation has been closed.
 */
AWS_EVENT_STREAM_API bool aws_event_stream_rpc_client_continuation_is_closed(
    const struct aws_event_stream_rpc_client_continuation_token *continuation);

/**
 * Actually sends the initial stream to the peer. Callbacks from aws_event_stream_rpc_client_connection_new_stream()
 * will actually be invoked if this function returns AWS_OP_SUCCESS, otherwise, the stream has not been queued and no
 * callbacks will be invoked.
 *
 * operation_name is the name to identify which logical rpc call you want to kick off with the peer. It must be
 * non-empty. flush_fn will be invoked once the message has either been written to the wire or it fails.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_client_continuation_activate(
    struct aws_event_stream_rpc_client_continuation_token *continuation,
    struct aws_byte_cursor operation_name,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_client_message_flush_fn *flush_fn,
    void *user_data);

AWS_EVENT_STREAM_API void *aws_event_stream_rpc_client_continuation_get_user_data(
    struct aws_event_stream_rpc_client_continuation_token *continuation);

/**
 * Sends a message on the continuation. aws_event_stream_rpc_client_continuation_activate() must be successfully invoked
 * prior to calling this function.
 *
 * If this function returns AWS_OP_SUCCESS, flush_fn will be invoked once the message has either been written to the
 * wire or it fails.
 */
AWS_EVENT_STREAM_API int aws_event_stream_rpc_client_continuation_send_message(
    struct aws_event_stream_rpc_client_continuation_token *continuation,
    const struct aws_event_stream_rpc_message_args *message_args,
    aws_event_stream_rpc_client_message_flush_fn *flush_fn,
    void *user_data);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_EVENT_STREAM_RPC_CLIENT_H */
