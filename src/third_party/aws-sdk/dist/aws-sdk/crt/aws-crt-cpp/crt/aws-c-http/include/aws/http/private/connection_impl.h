#ifndef AWS_HTTP_CONNECTION_IMPL_H
#define AWS_HTTP_CONNECTION_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/connection.h>

#include <aws/http/private/http_impl.h>
#include <aws/http/server.h>

#include <aws/common/atomics.h>
#include <aws/io/channel.h>
#include <aws/io/channel_bootstrap.h>

struct aws_http_message;
struct aws_http_make_request_options;
struct aws_http_request_handler_options;
struct aws_http_stream;

/* vtable of functions that aws_http_connection uses to interact with external systems.
 * tests override the vtable to mock those systems */
struct aws_http_connection_system_vtable {
    int (*aws_client_bootstrap_new_socket_channel)(struct aws_socket_channel_bootstrap_options *options);
};

struct aws_http_connection_vtable {
    struct aws_channel_handler_vtable channel_handler_vtable;

    /* This is a callback I wish was in aws_channel_handler_vtable. */
    void (*on_channel_handler_installed)(struct aws_channel_handler *handler, struct aws_channel_slot *slot);

    struct aws_http_stream *(*make_request)(
        struct aws_http_connection *client_connection,
        const struct aws_http_make_request_options *options);

    struct aws_http_stream *(*new_server_request_handler_stream)(
        const struct aws_http_request_handler_options *options);
    int (*stream_send_response)(struct aws_http_stream *stream, struct aws_http_message *response);
    void (*close)(struct aws_http_connection *connection);
    void (*stop_new_requests)(struct aws_http_connection *connection);
    bool (*is_open)(const struct aws_http_connection *connection);
    bool (*new_requests_allowed)(const struct aws_http_connection *connection);

    /* HTTP/2 specific functions */
    void (*update_window)(struct aws_http_connection *connection, uint32_t increment_size);
    int (*change_settings)(
        struct aws_http_connection *http2_connection,
        const struct aws_http2_setting *settings_array,
        size_t num_settings,
        aws_http2_on_change_settings_complete_fn *on_completed,
        void *user_data);
    int (*send_ping)(
        struct aws_http_connection *http2_connection,
        const struct aws_byte_cursor *optional_opaque_data,
        aws_http2_on_ping_complete_fn *on_completed,
        void *user_data);
    void (*send_goaway)(
        struct aws_http_connection *http2_connection,
        uint32_t http2_error,
        bool allow_more_streams,
        const struct aws_byte_cursor *optional_debug_data);
    int (*get_sent_goaway)(
        struct aws_http_connection *http2_connection,
        uint32_t *out_http2_error,
        uint32_t *out_last_stream_id);
    int (*get_received_goaway)(
        struct aws_http_connection *http2_connection,
        uint32_t *out_http2_error,
        uint32_t *out_last_stream_id);
    void (*get_local_settings)(
        const struct aws_http_connection *http2_connection,
        struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);
    void (*get_remote_settings)(
        const struct aws_http_connection *http2_connection,
        struct aws_http2_setting out_settings[AWS_HTTP2_SETTINGS_COUNT]);
};

typedef int(aws_http_proxy_request_transform_fn)(struct aws_http_message *request, void *user_data);

/**
 * Base class for connections.
 * There are specific implementations for each HTTP version.
 */
struct aws_http_connection {
    const struct aws_http_connection_vtable *vtable;
    struct aws_channel_handler channel_handler;
    struct aws_channel_slot *channel_slot;
    struct aws_allocator *alloc;
    enum aws_http_version http_version;

    aws_http_proxy_request_transform_fn *proxy_request_transform;
    void *user_data;

    /* Connection starts with 1 hold for the user.
     * aws_http_streams will also acquire holds on their connection for the duration of their lifetime */
    struct aws_atomic_var refcount;

    /* Starts at either 1 or 2, increments by two with each new stream */
    uint32_t next_stream_id;

    union {
        struct aws_http_connection_client_data {
            uint64_t response_first_byte_timeout_ms;
        } client;

        struct aws_http_connection_server_data {
            aws_http_on_incoming_request_fn *on_incoming_request;
            aws_http_on_server_connection_shutdown_fn *on_shutdown;
        } server;
    } client_or_server_data;

    /* On client connections, `client_data` points to client_or_server_data.client and `server_data` is null.
     * Opposite is true on server connections */
    struct aws_http_connection_client_data *client_data;
    struct aws_http_connection_server_data *server_data;

    bool stream_manual_window_management;
};

/* Gets a client connection up and running.
 * Responsible for firing on_setup and on_shutdown callbacks. */
struct aws_http_client_bootstrap {
    struct aws_allocator *alloc;
    bool is_using_tls;
    bool stream_manual_window_management;
    bool prior_knowledge_http2;
    size_t initial_window_size;
    struct aws_http_connection_monitoring_options monitoring_options;
    void *user_data;
    aws_http_on_client_connection_setup_fn *on_setup;
    aws_http_on_client_connection_shutdown_fn *on_shutdown;
    aws_http_proxy_request_transform_fn *proxy_request_transform;
    uint64_t response_first_byte_timeout_ms;

    struct aws_http1_connection_options http1_options;
    struct aws_http2_connection_options http2_options; /* allocated with bootstrap */
    struct aws_hash_table *alpn_string_map;            /* allocated with bootstrap */
    struct aws_http_connection *connection;
};

AWS_EXTERN_C_BEGIN
AWS_HTTP_API
void aws_http_client_bootstrap_destroy(struct aws_http_client_bootstrap *bootstrap);

AWS_HTTP_API
void aws_http_connection_set_system_vtable(const struct aws_http_connection_system_vtable *system_vtable);

AWS_HTTP_API
int aws_http_client_connect_internal(
    const struct aws_http_client_connection_options *options,
    aws_http_proxy_request_transform_fn *proxy_request_transform);

/**
 * Internal API for adding a reference to a connection
 */
AWS_HTTP_API
void aws_http_connection_acquire(struct aws_http_connection *connection);

/**
 * Allow tests to fake stats data
 */
AWS_HTTP_API
struct aws_crt_statistics_http1_channel *aws_h1_connection_get_statistics(struct aws_http_connection *connection);

/**
 * Gets the next available stream id within the connection.  Valid for creating both h1 and h2 streams.
 *
 * This function is not thread-safe.
 *
 * Returns 0 if there was an error.
 */
AWS_HTTP_API
uint32_t aws_http_connection_get_next_stream_id(struct aws_http_connection *connection);

/**
 * Layers an http channel handler/connection onto a channel.  Moved from internal to private so that the proxy
 * logic could apply a new http connection/handler after tunneling proxy negotiation (into http) is finished.
 * This is a synchronous operation.
 *
 * @param alloc memory allocator to use
 * @param channel channel to apply the http handler/connection to
 * @param is_server should the handler behave like an http server
 * @param is_using_tls is tls is being used (do an alpn check of the to-the-left channel handler)
 * @param manual_window_management is manual window management enabled
 * @param prior_knowledge_http2 prior knowledge about http2 connection to be used
 * @param initial_window_size what should the initial window size be
 * @param alpn_string_map the customized ALPN string map from `struct aws_string *` to `enum aws_http_version`.
 * @param http1_options http1 options
 * @param http2_options http2 options
 * @return a new http connection or NULL on failure
 */
AWS_HTTP_API
struct aws_http_connection *aws_http_connection_new_channel_handler(
    struct aws_allocator *alloc,
    struct aws_channel *channel,
    bool is_server,
    bool is_using_tls,
    bool manual_window_management,
    bool prior_knowledge_http2,
    size_t initial_window_size,
    const struct aws_hash_table *alpn_string_map,
    const struct aws_http1_connection_options *http1_options,
    const struct aws_http2_connection_options *http2_options,
    void *connection_user_data);

AWS_EXTERN_C_END

#endif /* AWS_HTTP_CONNECTION_IMPL_H */
