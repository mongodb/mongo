#ifndef AWS_IO_SOCKET_H
#define AWS_IO_SOCKET_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/channel.h>
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_socket_domain {
    AWS_SOCKET_IPV4,
    AWS_SOCKET_IPV6,
    /* Unix domain sockets (or at least something like them) */
    AWS_SOCKET_LOCAL,
    /* VSOCK used in inter-VM communication */
    AWS_SOCKET_VSOCK,
};

enum aws_socket_type {
    /* A streaming socket sends reliable messages over a two-way connection.
     * This means TCP when used with IPV4/6, and Unix domain sockets, when used with
     * AWS_SOCKET_LOCAL*/
    AWS_SOCKET_STREAM,
    /* A datagram socket is connectionless and sends unreliable messages.
     * This means UDP when used with IPV4/6.
     * LOCAL and VSOCK sockets are not compatible with DGRAM.*/
    AWS_SOCKET_DGRAM,
};

#define AWS_NETWORK_INTERFACE_NAME_MAX 16

struct aws_socket_options {
    enum aws_socket_type type;
    enum aws_socket_domain domain;
    uint32_t connect_timeout_ms;
    /* Keepalive properties are TCP only.
     * Set keepalive true to periodically transmit messages for detecting a disconnected peer.
     * If interval or timeout are zero, then default values are used. */
    uint16_t keep_alive_interval_sec;
    uint16_t keep_alive_timeout_sec;
    /* If set, sets the number of keep alive probes allowed to fail before the connection is considered
     * lost. If zero OS defaults are used. On Windows, this option is meaningless until Windows 10 1703.*/
    uint16_t keep_alive_max_failed_probes;
    bool keepalive;

    /**
     * THIS IS AN EXPERIMENTAL AND UNSTABLE API
     * (Optional)
     * This property is used to bind the socket to a particular network interface by name, such as eth0 and ens32.
     * If this is empty, the socket will not be bound to any interface and will use OS defaults. If the provided name
     * is invalid, `aws_socket_init()` will error out with AWS_IO_SOCKET_INVALID_OPTIONS. This option is only
     * supported on Linux, macOS, and platforms that have either SO_BINDTODEVICE or IP_BOUND_IF. It is not supported on
     * Windows. `AWS_ERROR_PLATFORM_NOT_SUPPORTED` will be raised on unsupported platforms.
     */
    char network_interface_name[AWS_NETWORK_INTERFACE_NAME_MAX];
};

struct aws_socket;
struct aws_event_loop;

/**
 * Called in client mode when an outgoing connection has succeeded or an error has occurred.
 * If the connection was successful error_code will be AWS_ERROR_SUCCESS and the socket has already been assigned
 * to the event loop specified in aws_socket_connect().
 *
 * If an error occurred error_code will be non-zero.
 */
typedef void(aws_socket_on_connection_result_fn)(struct aws_socket *socket, int error_code, void *user_data);

/**
 * Called by a listening socket when either an incoming connection has been received or an error occurred.
 *
 * In the normal use-case, this function will be called multiple times over the lifetime of a single listening socket.
 * new_socket is already connected and initialized, and is using the same options and allocator as the listening socket.
 * A user may want to call aws_socket_set_options() on the new socket if different options are desired.
 *
 * new_socket is not yet assigned to an event-loop. The user should call aws_socket_assign_to_event_loop() before
 * performing IO operations.
 *
 * When error_code is AWS_ERROR_SUCCESS, new_socket is the recently accepted connection.
 * If error_code is non-zero, an error occurred and you should aws_socket_close() the socket.
 *
 * Do not call aws_socket_clean_up() from this callback.
 */
typedef void(aws_socket_on_accept_result_fn)(
    struct aws_socket *socket,
    int error_code,
    struct aws_socket *new_socket,
    void *user_data);

/**
 * Callback for when the data passed to a call to aws_socket_write() has either completed or failed.
 * On success, error_code will be AWS_ERROR_SUCCESS.
 */
typedef void(
    aws_socket_on_write_completed_fn)(struct aws_socket *socket, int error_code, size_t bytes_written, void *user_data);
/**
 * Callback for when socket is either readable (edge-triggered) or when an error has occurred. If the socket is
 * readable, error_code will be AWS_ERROR_SUCCESS.
 */
typedef void(aws_socket_on_readable_fn)(struct aws_socket *socket, int error_code, void *user_data);

#ifdef _WIN32
#    define AWS_ADDRESS_MAX_LEN 256
#else
#    include <sys/un.h>
#    define AWS_ADDRESS_MAX_LEN sizeof(((struct sockaddr_un *)0)->sun_path)
#endif
struct aws_socket_endpoint {
    char address[AWS_ADDRESS_MAX_LEN];
    uint32_t port;
};

struct aws_socket {
    struct aws_allocator *allocator;
    struct aws_socket_endpoint local_endpoint;
    struct aws_socket_endpoint remote_endpoint;
    struct aws_socket_options options;
    struct aws_io_handle io_handle;
    struct aws_event_loop *event_loop;
    struct aws_channel_handler *handler;
    int state;
    aws_socket_on_readable_fn *readable_fn;
    void *readable_user_data;
    aws_socket_on_connection_result_fn *connection_result_fn;
    aws_socket_on_accept_result_fn *accept_result_fn;
    void *connect_accept_user_data;
    void *impl;
};

struct aws_byte_buf;
struct aws_byte_cursor;

/* These are hacks for working around headers and functions we need for IO work but aren't directly includable or
   linkable. these are purposely not exported. These functions only get called internally. The awkward aws_ prefixes are
   just in case someone includes this header somewhere they were able to get these definitions included. */
#ifdef _WIN32
typedef void (*aws_ms_fn_ptr)(void);

void aws_check_and_init_winsock(void);
aws_ms_fn_ptr aws_winsock_get_connectex_fn(void);
aws_ms_fn_ptr aws_winsock_get_acceptex_fn(void);
#endif

AWS_EXTERN_C_BEGIN

/**
 * Initializes a socket object with socket options. options will be copied.
 */
AWS_IO_API int aws_socket_init(
    struct aws_socket *socket,
    struct aws_allocator *alloc,
    const struct aws_socket_options *options);

/**
 * Shuts down any pending operations on the socket, and cleans up state. The socket object can be re-initialized after
 * this operation. This function calls aws_socket_close. If you have not already called aws_socket_close() on the
 * socket, all of the rules for aws_socket_close() apply here. In this case it will not fail if you use the function
 * improperly, but on some platforms you will certainly leak memory.
 *
 * If the socket has already been closed, you can safely, call this from any thread.
 */
AWS_IO_API void aws_socket_clean_up(struct aws_socket *socket);

/**
 * Connects to a remote endpoint. In UDP, this simply binds the socket to a remote address for use with
 * `aws_socket_write()`, and if the operation is successful, the socket can immediately be used for write operations.
 *
 * In TCP, LOCAL and VSOCK this function will not block. If the return value is successful, then you must wait on the
 * `on_connection_result()` callback to be invoked before using the socket.
 *
 * If an event_loop is provided for UDP sockets, a notification will be sent on
 * on_connection_result in the event-loop's thread. Upon completion, the socket will already be assigned
 * an event loop. If NULL is passed for UDP, it will immediately return upon success, but you must call
 * aws_socket_assign_to_event_loop before use.
 */
AWS_IO_API int aws_socket_connect(
    struct aws_socket *socket,
    const struct aws_socket_endpoint *remote_endpoint,
    struct aws_event_loop *event_loop,
    aws_socket_on_connection_result_fn *on_connection_result,
    void *user_data);

/**
 * Binds the socket to a local address. In UDP mode, the socket is ready for `aws_socket_read()` operations. In
 * connection oriented modes, you still must call `aws_socket_listen()` and `aws_socket_start_accept()` before using the
 * socket. local_endpoint is copied.
 */
AWS_IO_API int aws_socket_bind(struct aws_socket *socket, const struct aws_socket_endpoint *local_endpoint);

/**
 * Get the local address which the socket is bound to.
 * Raises an error if no address is bound.
 */
AWS_IO_API int aws_socket_get_bound_address(const struct aws_socket *socket, struct aws_socket_endpoint *out_address);

/**
 * TCP, LOCAL and VSOCK only. Sets up the socket to listen on the address bound to in `aws_socket_bind()`.
 */
AWS_IO_API int aws_socket_listen(struct aws_socket *socket, int backlog_size);

/**
 * TCP, LOCAL and VSOCK only. The socket will begin accepting new connections. This is an asynchronous operation. New
 * connections or errors will arrive via the `on_accept_result` callback.
 *
 * aws_socket_bind() and aws_socket_listen() must be called before calling this function.
 */
AWS_IO_API int aws_socket_start_accept(
    struct aws_socket *socket,
    struct aws_event_loop *accept_loop,
    aws_socket_on_accept_result_fn *on_accept_result,
    void *user_data);

/**
 * TCP, LOCAL and VSOCK only. The listening socket will stop accepting new connections.
 * It is safe to call `aws_socket_start_accept()` again after
 * this operation. This can be called from any thread but be aware,
 * on some platforms, if you call this from outside of the current event loop's thread, it will block
 * until the event loop finishes processing the request for unsubscribe in it's own thread.
 */
AWS_IO_API int aws_socket_stop_accept(struct aws_socket *socket);

/**
 * Calls `close()` on the socket and unregisters all io operations from the event loop. This function must be called
 * from the event-loop's thread unless this is a listening socket. If it's a listening socket it can be called from any
 * non-event-loop thread or the event-loop the socket is currently assigned to. If called from outside the event-loop,
 * this function will block waiting on the socket to close. If this is called from an event-loop thread other than
 * the one it's assigned to, it presents the possibility of a deadlock, so don't do it.
 */
AWS_IO_API int aws_socket_close(struct aws_socket *socket);

/**
 * Calls `shutdown()` on the socket based on direction.
 */
AWS_IO_API int aws_socket_shutdown_dir(struct aws_socket *socket, enum aws_channel_direction dir);

/**
 * Sets new socket options on the underlying socket. This is mainly useful in context of accepting a new connection via:
 * `on_incoming_connection()`. options is copied.
 */
AWS_IO_API int aws_socket_set_options(struct aws_socket *socket, const struct aws_socket_options *options);

/**
 * Assigns the socket to the event-loop. The socket will begin receiving read/write/error notifications after this call.
 *
 * Note: If you called connect for TCP or Unix Domain Sockets and received a connection_success callback, this has
 * already happened. You only need to call this function when:
 *
 * a.) This socket is a server socket (e.g. a result of a call to start_accept())
 * b.) This socket is a UDP socket.
 */
AWS_IO_API int aws_socket_assign_to_event_loop(struct aws_socket *socket, struct aws_event_loop *event_loop);

/**
 * Gets the event-loop the socket is assigned to.
 */
AWS_IO_API struct aws_event_loop *aws_socket_get_event_loop(struct aws_socket *socket);

/**
 * Subscribes on_readable to notifications when the socket goes readable (edge-triggered). Errors will also be recieved
 * in the callback.
 *
 * Note! This function is technically not thread safe, but we do not enforce which thread you call from.
 * It's your responsibility to either call this in safely (e.g. just don't call it in parallel from multiple threads) or
 * schedule a task to call it. If you call it before your first call to read, it will be fine.
 */
AWS_IO_API int aws_socket_subscribe_to_readable_events(
    struct aws_socket *socket,
    aws_socket_on_readable_fn *on_readable,
    void *user_data);

/**
 * Reads from the socket. This call is non-blocking and will return `AWS_IO_SOCKET_READ_WOULD_BLOCK` if no data is
 * available. `read` is the amount of data read into `buffer`.
 *
 * Attempts to read enough to fill all remaining space in the buffer, from `buffer->len` to `buffer->capacity`.
 * `buffer->len` is updated to reflect the buffer's new length.
 *
 *
 * Use aws_socket_subscribe_to_readable_events() to receive notifications of when the socket goes readable.
 *
 * NOTE! This function must be called from the event-loop used in aws_socket_assign_to_event_loop
 */
AWS_IO_API int aws_socket_read(struct aws_socket *socket, struct aws_byte_buf *buffer, size_t *amount_read);

/**
 * Writes to the socket. This call is non-blocking and will attempt to write as much as it can, but will queue any
 * remaining portion of the data for write when available. written_fn will be invoked once the entire cursor has been
 * written, or the write failed or was cancelled.
 *
 * NOTE! This function must be called from the event-loop used in aws_socket_assign_to_event_loop
 *
 * For client sockets, connect() and aws_socket_assign_to_event_loop() must be called before calling this.
 *
 * For incoming sockets from a listener, aws_socket_assign_to_event_loop() must be called first.
 */
AWS_IO_API int aws_socket_write(
    struct aws_socket *socket,
    const struct aws_byte_cursor *cursor,
    aws_socket_on_write_completed_fn *written_fn,
    void *user_data);

/**
 * Gets the latest error from the socket. If no error has occurred AWS_OP_SUCCESS will be returned. This function does
 * not raise any errors to the installed error handlers.
 */
AWS_IO_API int aws_socket_get_error(struct aws_socket *socket);

/**
 * Returns true if the socket is still open (doesn't mean connected or listening, only that it hasn't had close()
 * called.
 */
AWS_IO_API bool aws_socket_is_open(struct aws_socket *socket);

/**
 * Raises AWS_IO_SOCKET_INVALID_ADDRESS and logs an error if connecting to this port is illegal.
 * For example, port must be in range 1-65535 to connect with IPv4.
 * These port values would fail eventually in aws_socket_connect(),
 * but you can use this function to validate earlier.
 */
AWS_IO_API int aws_socket_validate_port_for_connect(uint32_t port, enum aws_socket_domain domain);

/**
 * Raises AWS_IO_SOCKET_INVALID_ADDRESS and logs an error if binding to this port is illegal.
 * For example, port must in range 0-65535 to bind with IPv4.
 * These port values would fail eventually in aws_socket_bind(),
 * but you can use this function to validate earlier.
 */
AWS_IO_API int aws_socket_validate_port_for_bind(uint32_t port, enum aws_socket_domain domain);

/**
 * Assigns a random address (UUID) for use with AWS_SOCKET_LOCAL (Unix Domain Sockets).
 * For use in internal tests only.
 */
AWS_IO_API void aws_socket_endpoint_init_local_address_for_test(struct aws_socket_endpoint *endpoint);

/**
 * Validates whether the network interface name is valid. On Windows, it will always return false since we don't support
 * network_interface_name on Windows */
AWS_IO_API bool aws_is_network_interface_name_valid(const char *interface_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_SOCKET_H */
