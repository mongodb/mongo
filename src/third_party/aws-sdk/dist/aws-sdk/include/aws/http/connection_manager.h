#ifndef AWS_HTTP_CONNECTION_MANAGER_H
#define AWS_HTTP_CONNECTION_MANAGER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

#include <aws/common/byte_buf.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_client_bootstrap;
struct aws_http_connection;
struct aws_http_connection_manager;
struct aws_socket_options;
struct aws_tls_connection_options;
struct proxy_env_var_settings;
struct aws_http2_setting;

typedef void(aws_http_connection_manager_on_connection_setup_fn)(
    struct aws_http_connection *connection,
    int error_code,
    void *user_data);

typedef void(aws_http_connection_manager_shutdown_complete_fn)(void *user_data);

/**
 * Metrics for logging and debugging purpose.
 */
struct aws_http_manager_metrics {
    /**
     * The number of additional concurrent requests that can be supported by the HTTP manager without needing to
     * establish additional connections to the target server.
     *
     * For connection manager, it equals to connections that's idle.
     * For stream manager, it equals to the number of streams that are possible to be made without creating new
     * connection, although the implementation can create new connection without fully filling it.
     */
    size_t available_concurrency;
    /* The number of requests that are awaiting concurrency to be made available from the HTTP manager. */
    size_t pending_concurrency_acquires;
    /* The number of connections (http/1.1) or streams (for h2 via. stream manager) currently vended to user. */
    size_t leased_concurrency;
};

/*
 * Connection manager configuration struct.
 *
 * Contains all of the configuration needed to create an http connection as well as
 * the maximum number of connections to ever have in existence.
 */
struct aws_http_connection_manager_options {
    /*
     * http connection configuration, check `struct aws_http_client_connection_options` for details of each config
     */
    struct aws_client_bootstrap *bootstrap;
    size_t initial_window_size;
    const struct aws_socket_options *socket_options;

    /**
     * Options to create secure (HTTPS) connections.
     * For secure connections, set "h2" in the ALPN string for HTTP/2, otherwise HTTP/1.1 is used.
     *
     * Leave NULL to create cleartext (HTTP) connections.
     * For cleartext connections, use `http2_prior_knowledge` (RFC-7540 3.4)
     * to control whether that are treated as HTTP/1.1 or HTTP/2.
     */
    const struct aws_tls_connection_options *tls_connection_options;

    /**
     * Specify whether you have prior knowledge that cleartext (HTTP) connections are HTTP/2 (RFC-7540 3.4).
     * If false, then cleartext connections are treated as HTTP/1.1.
     * It is illegal to set this true when secure connections are being used.
     * Note that upgrading from HTTP/1.1 to HTTP/2 is not supported (RFC-7540 3.2).
     */
    bool http2_prior_knowledge;

    const struct aws_http_connection_monitoring_options *monitoring_options;
    struct aws_byte_cursor host;
    uint32_t port;

    /**
     * Optional.
     * HTTP/2 specific configuration. Check `struct aws_http2_connection_options` for details of each config
     */
    const struct aws_http2_setting *initial_settings_array;
    size_t num_initial_settings;
    size_t max_closed_streams;
    bool http2_conn_manual_window_management;

    /* Proxy configuration for http connection */
    const struct aws_http_proxy_options *proxy_options;

    /*
     * Optional.
     * Configuration for using proxy from environment variable.
     * Only works when proxy_options is not set.
     */
    const struct proxy_env_var_settings *proxy_ev_settings;

    /*
     * Maximum number of connections this manager is allowed to contain
     */
    size_t max_connections;

    /*
     * Callback and associated user data to invoke when the connection manager has
     * completely shutdown and has finished deleting itself.
     * Technically optional, but correctness may be impossible without it.
     */
    void *shutdown_complete_user_data;
    aws_http_connection_manager_shutdown_complete_fn *shutdown_complete_callback;

    /**
     * If set to true, the read back pressure mechanism will be enabled.
     */
    bool enable_read_back_pressure;

    /**
     * If set to a non-zero value, then connections that stay in the pool longer than the specified
     * timeout will be closed automatically.
     */
    uint64_t max_connection_idle_in_milliseconds;

    /**
     * If set to a non-zero value, aws_http_connection_manager_acquire_connection() calls
     * will give up after waiting this long for a connection from the pool,
     * failing with error AWS_ERROR_HTTP_CONNECTION_MANAGER_ACQUISITION_TIMEOUT.
     */
    uint64_t connection_acquisition_timeout_ms;

    /*
     * If set to a non-zero value, aws_http_connection_manager_acquire_connection() calls will fail with
     * AWS_ERROR_HTTP_CONNECTION_MANAGER_MAX_PENDING_ACQUISITIONS_EXCEEDED if the number of pending acquisitions
     * reaches `max_pending_connection_acquisitions` after the connection pool has reached its capacity (i.e., all
     * `num_connections` have been vended).
     */
    uint64_t max_pending_connection_acquisitions;

    /**
     * THIS IS AN EXPERIMENTAL AND UNSTABLE API
     * (Optional)
     * An array of network interface names. The manager will distribute the
     * connections across network interface names provided in this array. If any interface name is invalid, goes down,
     * or has any issues like network access, you will see connection failures. If
     * `socket_options.network_interface_name` is also set, an `AWS_ERROR_INVALID_ARGUMENT` error will be raised.
     *
     * This option is only supported on Linux, MacOS, and platforms that have either SO_BINDTODEVICE or IP_BOUND_IF. It
     * is not supported on Windows. `AWS_ERROR_PLATFORM_NOT_SUPPORTED` will be raised on unsupported platforms.
     */
    const struct aws_byte_cursor *network_interface_names_array;
    size_t num_network_interface_names;
};

AWS_EXTERN_C_BEGIN

/*
 * Connection managers are ref counted.  Adds one external ref to the manager.
 */
AWS_HTTP_API
void aws_http_connection_manager_acquire(struct aws_http_connection_manager *manager);

/*
 * Connection managers are ref counted.  Removes one external ref from the manager.
 *
 * When the ref count goes to zero, the connection manager begins its shut down
 * process.  All pending connection acquisitions are failed (with callbacks
 * invoked) and any (erroneous) subsequent attempts to acquire a connection
 * fail immediately.  The connection manager destroys itself once all pending
 * asynchronous activities have resolved.
 */
AWS_HTTP_API
void aws_http_connection_manager_release(struct aws_http_connection_manager *manager);

/*
 * Creates a new connection manager with the supplied configuration options.
 *
 * The returned connection manager begins with a ref count of 1.
 */
AWS_HTTP_API
struct aws_http_connection_manager *aws_http_connection_manager_new(
    struct aws_allocator *allocator,
    const struct aws_http_connection_manager_options *options);

/*
 * Requests a connection from the manager.  The requester is notified of
 * an acquired connection (or failure to acquire) via the supplied callback.
 *
 * For HTTP/2 connections, the callback will not fire until the server's settings have been received.
 *
 * Once a connection has been successfully acquired from the manager it
 * must be released back (via aws_http_connection_manager_release_connection)
 * at some point.  Failure to do so will cause a resource leak.
 */
AWS_HTTP_API
void aws_http_connection_manager_acquire_connection(
    struct aws_http_connection_manager *manager,
    aws_http_connection_manager_on_connection_setup_fn *callback,
    void *user_data);

/*
 * Returns a connection back to the manager.  All acquired connections must
 * eventually be released back to the manager in order to avoid a resource leak.
 *
 * Note: it can lead to another acquired callback to be invoked within the thread.
 */
AWS_HTTP_API
int aws_http_connection_manager_release_connection(
    struct aws_http_connection_manager *manager,
    struct aws_http_connection *connection);

/**
 * Fetch the current manager metrics from connection manager.
 */
AWS_HTTP_API
void aws_http_connection_manager_fetch_metrics(
    const struct aws_http_connection_manager *manager,
    struct aws_http_manager_metrics *out_metrics);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP_CONNECTION_MANAGER_H */
