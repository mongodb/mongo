#ifndef AWS_IO_CHANNEL_BOOTSTRAP_H
#define AWS_IO_CHANNEL_BOOTSTRAP_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/ref_count.h>
#include <aws/io/channel.h>
#include <aws/io/host_resolver.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_client_bootstrap;
struct aws_socket;
struct aws_socket_options;
struct aws_socket_endpoint;

/**
 * Generic event function for channel lifecycle events.
 *
 * Callbacks are provided for:
 *   (1) Channel creation
 *   (2) Channel setup - If TLS is being used, this function is called once the socket has connected, the channel has
 * been initialized, and TLS has been successfully negotiated. A TLS handler has already been added to the channel. If
 * TLS negotiation fails, this function will be called with the corresponding error code. If TLS is not being used, this
 * function is called once the socket has connected and the channel has been initialized.
 *   (3) Channel shutdown
 *
 * These callbacks are always invoked within the thread of the event-loop that the channel is assigned to.
 *
 * This function does NOT always imply "success" -- if error_code is AWS_OP_SUCCESS then everything was successful,
 * otherwise an error condition occurred.
 */
typedef void(aws_client_bootstrap_on_channel_event_fn)(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data);

/**
 * If ALPN is being used this function will be invoked by the channel once an ALPN message is received. The returned
 * channel_handler will be added to, and managed by, the channel.
 */
typedef struct aws_channel_handler *(aws_channel_on_protocol_negotiated_fn)(struct aws_channel_slot *new_slot,
                                                                            struct aws_byte_buf *protocol,
                                                                            void *user_data);

struct aws_tls_connection_options;

struct aws_event_loop_group;

/**
 * Called after client bootstrap has been completely cleaned up, after its last refcount is released.
 */
typedef void aws_client_bootstrap_shutdown_complete_fn(void *user_data);

/**
 * aws_client_bootstrap handles creation and setup of channels that communicate via socket with a specific endpoint.
 */
struct aws_client_bootstrap {
    struct aws_allocator *allocator;
    struct aws_event_loop_group *event_loop_group;
    struct aws_host_resolver *host_resolver;
    struct aws_host_resolution_config host_resolver_config;
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated;
    struct aws_ref_count ref_count;
    aws_client_bootstrap_shutdown_complete_fn *on_shutdown_complete;
    void *user_data;
};

/**
 * aws_client_bootstrap creation options.
 */
struct aws_client_bootstrap_options {

    /* Required. Must outlive the client bootstrap. */
    struct aws_event_loop_group *event_loop_group;

    /* Required. Must outlive the client bootstrap. */
    struct aws_host_resolver *host_resolver;

    /* Optional. If none is provided then default settings are used.
     * This object is deep-copied by bootstrap.
     * */
    const struct aws_host_resolution_config *host_resolution_config;

    /* Optional. If provided, callback is invoked when client bootstrap has completely shut down. */
    aws_client_bootstrap_shutdown_complete_fn *on_shutdown_complete;

    /* Optional. Passed to callbacks */
    void *user_data;
};

struct aws_server_bootstrap;

/**
 * If TLS is being used, this function is called once the socket has received an incoming connection, the channel has
 * been initialized, and TLS has been successfully negotiated. A TLS handler has already been added to the channel. If
 * TLS negotiation fails, this function will be called with the corresponding error code.
 *
 * If TLS is not being used, this function is called once the socket has received an incoming connection and the channel
 * has been initialized.
 *
 * This function is always called within the thread of the event-loop that the new channel is assigned to upon success.
 *
 * On failure, the channel might not be assigned to an event loop yet, and will thus be invoked on the listener's
 * event-loop thread.
 *
 * This function does NOT mean "success", if error_code is AWS_OP_SUCCESS then everything was successful, otherwise an
 * error condition occurred.
 *
 * If an error occurred, you do not need to shutdown the channel. The `aws_channel_client_shutdown_callback` will be
 * invoked once the channel has finished shutting down.
 */
typedef void(aws_server_bootstrap_on_accept_channel_setup_fn)(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data);

/**
 * Once the channel shuts down, this function will be invoked within the thread of
 * the event-loop that the channel is assigned to.
 *
 * Note: this function is only invoked if the channel was successfully setup,
 * e.g. aws_server_bootstrap_on_accept_channel_setup_fn() was invoked without an error code.
 */
typedef void(aws_server_bootstrap_on_accept_channel_shutdown_fn)(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data);

/**
 * Once the server listener socket is finished destroying, and all the existing connections are closed, this fuction
 * will be invoked.
 */
typedef void(
    aws_server_bootstrap_on_server_listener_destroy_fn)(struct aws_server_bootstrap *bootstrap, void *user_data);

/**
 * aws_server_bootstrap manages listening sockets, creating and setting up channels to handle each incoming connection.
 */
struct aws_server_bootstrap {
    struct aws_allocator *allocator;
    struct aws_event_loop_group *event_loop_group;
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated;
    struct aws_ref_count ref_count;
};

/**
 * Socket-based channel creation options.
 *
 * bootstrap - configs name resolution and which event loop group the connection will be seated into
 * host_name - host to connect to; if a dns address, will be resolved prior to connecting
 * port - port to connect to
 * socket_options - socket properties, including type (tcp vs. udp vs. unix domain) and connect timeout.  TLS
 *   connections are currently restricted to tcp (AWS_SOCKET_STREAM) only.
 * tls_options - (optional) tls context to apply after connection establishment.  If NULL, the connection will
 *   not be protected by TLS.
 * creation_callback - (optional) callback invoked when the channel is first created.  This is always right after
 *   the connection was successfully established.  *Does NOT* get called if the initial connect failed.
 * setup_callback - callback invoked once the channel is ready for use and TLS has been negotiated or if an error
 *   is encountered
 * shutdown_callback - callback invoked once the channel has shutdown.
 * enable_read_back_pressure - controls whether or not back pressure will be applied in the channel
 * user_data - arbitrary data to pass back to the various callbacks
 * requested_event_loop - if set, the connection will be placed on the requested event loop rather than one
 *  chosen internally from the bootstrap's associated event loop group.  It is an error to pass in an event loop
 *  that is not associated with the bootstrap's event loop group.
 *
 * Immediately after the `shutdown_callback` returns, the channel is cleaned up automatically. All callbacks are invoked
 * in the thread of the event-loop that the new channel is assigned to.
 *
 */
struct aws_socket_channel_bootstrap_options {
    struct aws_client_bootstrap *bootstrap;
    const char *host_name;
    uint32_t port;
    const struct aws_socket_options *socket_options;
    const struct aws_tls_connection_options *tls_options;
    aws_client_bootstrap_on_channel_event_fn *creation_callback;
    aws_client_bootstrap_on_channel_event_fn *setup_callback;
    aws_client_bootstrap_on_channel_event_fn *shutdown_callback;
    bool enable_read_back_pressure;
    void *user_data;
    struct aws_event_loop *requested_event_loop;
    const struct aws_host_resolution_config *host_resolution_override_config;
};

/**
 * Arguments to setup a server socket listener which will also negotiate and configure TLS.
 * This creates a socket listener bound to `host` and 'port' using socket options `options`, and TLS options
 * `tls_options`. `incoming_callback` will be invoked once an incoming channel is ready for use and TLS is
 * finished negotiating, or if an error is encountered. `shutdown_callback` will be invoked once the channel has
 * shutdown. `destroy_callback` will be invoked after the server socket listener is destroyed, and all associated
 * connections and channels have finished shutting down. Immediately after the `shutdown_callback` returns, the channel
 * is cleaned up automatically. All callbacks are invoked in the thread of the event-loop that listener is assigned to.
 *
 * Upon shutdown of your application, you'll want to call `aws_server_bootstrap_destroy_socket_listener` with the return
 * value from this function.
 *
 * The socket type in `options` must be AWS_SOCKET_STREAM if tls_options is set.
 * DTLS is not currently supported for tls.
 */
struct aws_server_socket_channel_bootstrap_options {
    struct aws_server_bootstrap *bootstrap;
    const char *host_name;
    uint32_t port;
    const struct aws_socket_options *socket_options;
    const struct aws_tls_connection_options *tls_options;
    aws_server_bootstrap_on_accept_channel_setup_fn *incoming_callback;
    aws_server_bootstrap_on_accept_channel_shutdown_fn *shutdown_callback;
    aws_server_bootstrap_on_server_listener_destroy_fn *destroy_callback;
    bool enable_read_back_pressure;
    void *user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Create the client bootstrap.
 */
AWS_IO_API struct aws_client_bootstrap *aws_client_bootstrap_new(
    struct aws_allocator *allocator,
    const struct aws_client_bootstrap_options *options);

/**
 * Increments a client bootstrap's ref count, allowing the caller to take a reference to it.
 *
 * Returns the same client bootstrap passed in.
 */
AWS_IO_API struct aws_client_bootstrap *aws_client_bootstrap_acquire(struct aws_client_bootstrap *bootstrap);

/**
 * Decrements a client bootstrap's ref count.  When the ref count drops to zero, the bootstrap will be destroyed.
 */
AWS_IO_API void aws_client_bootstrap_release(struct aws_client_bootstrap *bootstrap);

/**
 * When using TLS, if ALPN is used, this callback will be invoked from the channel. The returned handler will be added
 * to the channel.
 */
AWS_IO_API int aws_client_bootstrap_set_alpn_callback(
    struct aws_client_bootstrap *bootstrap,
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated);

/**
 * Sets up a client socket channel.
 */
AWS_IO_API int aws_client_bootstrap_new_socket_channel(struct aws_socket_channel_bootstrap_options *options);

/**
 * Initializes the server bootstrap with `allocator` and `el_group`. This object manages listeners, server connections,
 * and channels.
 */
AWS_IO_API struct aws_server_bootstrap *aws_server_bootstrap_new(
    struct aws_allocator *allocator,
    struct aws_event_loop_group *el_group);

/**
 * Increments a server bootstrap's ref count, allowing the caller to take a reference to it.
 *
 * Returns the same server bootstrap passed in.
 */
AWS_IO_API struct aws_server_bootstrap *aws_server_bootstrap_acquire(struct aws_server_bootstrap *bootstrap);

/**
 * Decrements a server bootstrap's ref count.  When the ref count drops to zero, the bootstrap will be destroyed.
 */
AWS_IO_API void aws_server_bootstrap_release(struct aws_server_bootstrap *bootstrap);

/**
 * When using TLS, if ALPN is used, this callback will be invoked from the channel. The returned handler will be added
 * to the channel.
 */
AWS_IO_API int aws_server_bootstrap_set_alpn_callback(
    struct aws_server_bootstrap *bootstrap,
    aws_channel_on_protocol_negotiated_fn *on_protocol_negotiated);

/**
 * Sets up a server socket listener. If you are planning on using TLS, use
 * `aws_server_bootstrap_new_tls_socket_listener` instead. This creates a socket listener bound to `local_endpoint`
 * using socket options `options`. `incoming_callback` will be invoked once an incoming channel is ready for use or if
 * an error is encountered. `shutdown_callback` will be invoked once the channel has shutdown. `destroy_callback` will
 * be invoked after the server socket listener is destroyed, and all associated connections and channels have finished
 * shutting down. Immediately after the `shutdown_callback` returns, the channel is cleaned up automatically. All
 * callbacks are invoked the thread of the event-loop that the listening socket is assigned to
 *
 * Upon shutdown of your application, you'll want to call `aws_server_bootstrap_destroy_socket_listener` with the return
 * value from this function.
 *
 * bootstrap_options is copied.
 */
AWS_IO_API struct aws_socket *aws_server_bootstrap_new_socket_listener(
    const struct aws_server_socket_channel_bootstrap_options *bootstrap_options);

/**
 * Shuts down 'listener' and cleans up any resources associated with it. Any incoming channels on `listener` will still
 * be active. `destroy_callback` will be invoked after the server socket listener is destroyed, and all associated
 * connections and channels have finished shutting down.
 */
AWS_IO_API void aws_server_bootstrap_destroy_socket_listener(
    struct aws_server_bootstrap *bootstrap,
    struct aws_socket *listener);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_CHANNEL_BOOTSTRAP_H */
