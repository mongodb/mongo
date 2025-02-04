#ifndef AWS_HTTP_SERVER_H
#define AWS_HTTP_SERVER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http_connection;
struct aws_server_bootstrap;
struct aws_socket_options;
struct aws_tls_connection_options;
/**
 * A listening socket which accepts incoming HTTP connections,
 * creating a server-side aws_http_connection to handle each one.
 */
struct aws_http_server;
struct aws_http_stream;

typedef void(aws_http_server_on_incoming_connection_fn)(
    struct aws_http_server *server,
    struct aws_http_connection *connection,
    int error_code,
    void *user_data);

typedef void(aws_http_server_on_destroy_fn)(void *user_data);

/**
 * Options for creating an HTTP server.
 * Initialize with AWS_HTTP_SERVER_OPTIONS_INIT to set default values.
 */
struct aws_http_server_options {
    /**
     * The sizeof() this struct, used for versioning.
     * Set by AWS_HTTP_SERVER_OPTIONS_INIT.
     */
    size_t self_size;

    /**
     * Required.
     * Must outlive server.
     */
    struct aws_allocator *allocator;

    /**
     * Required.
     * Must outlive server.
     */
    struct aws_server_bootstrap *bootstrap;

    /**
     * Required.
     * Server makes copy.
     */
    struct aws_socket_endpoint *endpoint;

    /**
     * Required.
     * Server makes a copy.
     */
    struct aws_socket_options *socket_options;

    /**
     * Optional.
     * Server copies all contents except the `aws_tls_ctx`, which must outlive the server.
     */
    struct aws_tls_connection_options *tls_options;

    /**
     * Initial window size for incoming connections.
     * Optional.
     * A default size is set by AWS_HTTP_SERVER_OPTIONS_INIT.
     */
    size_t initial_window_size;

    /**
     * User data passed to callbacks.
     * Optional.
     */
    void *server_user_data;

    /**
     * Invoked when an incoming connection has been set up, or when setup has failed.
     * Required.
     * If setup succeeds, the user must call aws_http_connection_configure_server().
     */
    aws_http_server_on_incoming_connection_fn *on_incoming_connection;

    /**
     * Invoked when the server finishes the destroy operation.
     * Optional.
     */
    aws_http_server_on_destroy_fn *on_destroy_complete;

    /**
     * Set to true to manually manage the read window size.
     *
     * If this is false, the connection will maintain a constant window size.
     *
     * If this is true, the caller must manually increment the window size using aws_http_stream_update_window().
     * If the window is not incremented, it will shrink by the amount of body data received. If the window size
     * reaches 0, no further data will be received.
     **/
    bool manual_window_management;
};

/**
 * Initializes aws_http_server_options with default values.
 */
#define AWS_HTTP_SERVER_OPTIONS_INIT                                                                                   \
    {                                                                                                                  \
        .self_size = sizeof(struct aws_http_server_options),                                                           \
        .initial_window_size = SIZE_MAX,                                                                               \
    }

/**
 * Invoked at the start of an incoming request.
 * To process the request, the user must create a request handler stream and return it to the connection.
 * If NULL is returned, the request will not be processed and the last error will be reported as the reason for failure.
 */
typedef struct aws_http_stream *(aws_http_on_incoming_request_fn)(struct aws_http_connection *connection,
                                                                  void *user_data);

typedef void(aws_http_on_server_connection_shutdown_fn)(
    struct aws_http_connection *connection,
    int error_code,
    void *connection_user_data);

/**
 * Options for configuring a server-side aws_http_connection.
 * Initialized with AWS_HTTP_SERVER_CONNECTION_OPTIONS_INIT to set default values.
 */
struct aws_http_server_connection_options {
    /**
     * The sizeof() this struct, used for versioning.
     * Set by AWS_HTTP_SERVER_CONNECTION_OPTIONS_INIT.
     */
    size_t self_size;

    /**
     * User data specific to this connection.
     * Optional.
     */
    void *connection_user_data;

    /**
     * Invoked at the start of an incoming request.
     * Required.
     * The user must create a request handler stream and return it to the connection.
     * See `aws_http_on_incoming_request_fn`.
     */
    aws_http_on_incoming_request_fn *on_incoming_request;

    /**
     * Invoked when the connection is shut down.
     * Optional.
     */
    aws_http_on_server_connection_shutdown_fn *on_shutdown;
};

/**
 * Initializes aws_http_server_connection_options with default values.
 */
#define AWS_HTTP_SERVER_CONNECTION_OPTIONS_INIT                                                                        \
    {                                                                                                                  \
        .self_size = sizeof(struct aws_http_server_connection_options),                                                \
    }

AWS_EXTERN_C_BEGIN

/**
 * Create server, a listening socket that accepts incoming connections.
 */
AWS_HTTP_API
struct aws_http_server *aws_http_server_new(const struct aws_http_server_options *options);

/**
 * Release the server. It will close the listening socket and all the connections existing in the server.
 * The on_destroy_complete will be invoked when the destroy operation completes
 */
AWS_HTTP_API
void aws_http_server_release(struct aws_http_server *server);

/**
 * Configure a server connection.
 * This must be called from the server's on_incoming_connection callback.
 */
AWS_HTTP_API
int aws_http_connection_configure_server(
    struct aws_http_connection *connection,
    const struct aws_http_server_connection_options *options);

/**
 * Returns true if this is a server connection.
 */
AWS_HTTP_API
bool aws_http_connection_is_server(const struct aws_http_connection *connection);

/**
 * Returns the local listener endpoint of the HTTP server.  Only valid as long as the server remains valid.
 */
AWS_HTTP_API
const struct aws_socket_endpoint *aws_http_server_get_listener_endpoint(const struct aws_http_server *server);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_SERVER_H */
