#ifndef AWS_HTTP2_STREAM_MANAGER_H
#define AWS_HTTP2_STREAM_MANAGER_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/http.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_http2_stream_manager;
struct aws_client_bootstrap;
struct aws_http_connection;
struct aws_http_connection_manager;
struct aws_socket_options;
struct aws_tls_connection_options;
struct proxy_env_var_settings;
struct aws_http2_setting;
struct aws_http_make_request_options;
struct aws_http_stream;
struct aws_http_manager_metrics;

/**
 * Always invoked asynchronously when the stream was created, successfully or not.
 * When stream is NULL, error code will be set to indicate what happened.
 * If there is a stream returned, you own the stream completely.
 * Invoked on the same thread as other callback of the stream, which will be the thread of the connection, ideally.
 * If there is no connection made, the callback will be invoked from a sperate thread.
 */
typedef void(
    aws_http2_stream_manager_on_stream_acquired_fn)(struct aws_http_stream *stream, int error_code, void *user_data);

/**
 * Invoked asynchronously when the stream manager has been shutdown completely.
 * Never invoked when `aws_http2_stream_manager_new` failed.
 */
typedef void(aws_http2_stream_manager_shutdown_complete_fn)(void *user_data);

/**
 * HTTP/2 stream manager configuration struct.
 *
 * Contains all of the configuration needed to create an http2 connection as well as
 * connection manager under the hood.
 */
struct aws_http2_stream_manager_options {
    /**
     * basic http connection configuration
     */
    struct aws_client_bootstrap *bootstrap;
    const struct aws_socket_options *socket_options;

    /**
     * Options to create secure (HTTPS) connections.
     * For secure connections, the ALPN string must be "h2".
     *
     * To create cleartext (HTTP) connections, leave this NULL
     * and set `http2_prior_knowledge` (RFC-7540 3.4).
     */
    const struct aws_tls_connection_options *tls_connection_options;

    /**
     * Specify whether you have prior knowledge that cleartext (HTTP) connections are HTTP/2 (RFC-7540 3.4).
     * It is illegal to set this true when secure connections are being used.
     * Note that upgrading from HTTP/1.1 to HTTP/2 is not supported (RFC-7540 3.2).
     */
    bool http2_prior_knowledge;

    struct aws_byte_cursor host;
    uint32_t port;

    /**
     * Optional.
     * HTTP/2 connection configuration. Check `struct aws_http2_connection_options` for details of each config.
     * Notes for window control:
     * - By default, client will will maintain its flow-control windows such that no back-pressure is applied and data
     * arrives as fast as possible.
     * - For connection level window control, `conn_manual_window_management` will enable manual control. The
     * inital window size is not controllable.
     * - For stream level window control, `enable_read_back_pressure` will enable manual control. The initial window
     * size needs to be set through `initial_settings_array`.
     */
    const struct aws_http2_setting *initial_settings_array;
    size_t num_initial_settings;
    size_t max_closed_streams;
    bool conn_manual_window_management;

    /**
     * HTTP/2 Stream window control.
     * If set to true, the read back pressure mechanism will be enabled for streams created.
     * The initial window size can be set by `AWS_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE` via `initial_settings_array`
     */
    bool enable_read_back_pressure;

    /* Connection monitor for the underlying connections made */
    const struct aws_http_connection_monitoring_options *monitoring_options;

    /* Optional. Proxy configuration for underlying http connection */
    const struct aws_http_proxy_options *proxy_options;
    const struct proxy_env_var_settings *proxy_ev_settings;

    /**
     * Required.
     * When the stream manager finishes deleting all the resources, the callback will be invoked.
     */
    void *shutdown_complete_user_data;
    aws_http2_stream_manager_shutdown_complete_fn *shutdown_complete_callback;

    /**
     * Optional.
     * When set, connection will be closed if 5xx response received from server.
     */
    bool close_connection_on_server_error;
    /**
     * Optional.
     * The period for all the connections held by stream manager to send a PING in milliseconds.
     * If you specify 0, manager will NOT send any PING.
     * Note: if set, it must be large than the time of ping timeout setting.
     */
    size_t connection_ping_period_ms;
    /**
     * Optional.
     * Network connection will be closed if a ping response is not received
     * within this amount of time (milliseconds).
     * If you specify 0, a default value will be used.
     */
    size_t connection_ping_timeout_ms;

    /* TODO: More flexible policy about the connections, but will always has these three values below. */
    /**
     * Optional.
     * 0 will be considered as using a default value.
     * The ideal number of concurrent streams for a connection. Stream manager will try to create a new connection if
     * one connection reaches this number. But, if the max connections reaches, manager will reuse connections to create
     * the acquired steams as much as possible. */
    size_t ideal_concurrent_streams_per_connection;
    /**
     * Optional.
     * Default is no limit, which will use the limit from the server. 0 will be considered as using the default value.
     * The real number of concurrent streams per connection will be controlled by the minmal value of the setting from
     * other end and the value here.
     */
    size_t max_concurrent_streams_per_connection;
    /**
     * Required.
     * The max number of connections will be open at same time. If all the connections are full, manager will wait until
     * available to vender more streams */
    size_t max_connections;
};

struct aws_http2_stream_manager_acquire_stream_options {
    /**
     * Required.
     * Invoked when the stream finishes acquiring by stream manager.
     */
    aws_http2_stream_manager_on_stream_acquired_fn *callback;
    /**
     * Optional.
     * User data for the callback.
     */
    void *user_data;
    /* Required. see `aws_http_make_request_options` */
    const struct aws_http_make_request_options *options;
};

AWS_EXTERN_C_BEGIN

/**
 * Acquire a refcount from the stream manager, stream manager will start to destroy after the refcount drops to zero.
 * NULL is acceptable. Initial refcount after new is 1.
 *
 * @param manager
 * @return The same pointer acquiring.
 */
AWS_HTTP_API
struct aws_http2_stream_manager *aws_http2_stream_manager_acquire(struct aws_http2_stream_manager *manager);

/**
 * Release a refcount from the stream manager, stream manager will start to destroy after the refcount drops to zero.
 * NULL is acceptable. Initial refcount after new is 1.
 *
 * @param manager
 * @return NULL
 */
AWS_HTTP_API
struct aws_http2_stream_manager *aws_http2_stream_manager_release(struct aws_http2_stream_manager *manager);

AWS_HTTP_API
struct aws_http2_stream_manager *aws_http2_stream_manager_new(
    struct aws_allocator *allocator,
    const struct aws_http2_stream_manager_options *options);

/**
 * Acquire a stream from stream manager asynchronously.
 *
 * @param http2_stream_manager
 * @param acquire_stream_option see `aws_http2_stream_manager_acquire_stream_options`
 */
AWS_HTTP_API
void aws_http2_stream_manager_acquire_stream(
    struct aws_http2_stream_manager *http2_stream_manager,
    const struct aws_http2_stream_manager_acquire_stream_options *acquire_stream_option);

/**
 * Fetch the current metrics from stream manager.
 *
 * @param http2_stream_manager
 * @param out_metrics The metrics to be fetched
 */
AWS_HTTP_API
void aws_http2_stream_manager_fetch_metrics(
    const struct aws_http2_stream_manager *http2_stream_manager,
    struct aws_http_manager_metrics *out_metrics);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_HTTP2_STREAM_MANAGER_H */
