/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/socket.h>

#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/uuid.h>

#include <aws/io/event_loop.h>
#include <aws/io/logging.h>
#include <aws/io/private/event_loop_impl.h>

#include <arpa/inet.h>
#include <aws/io/io.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h> /* Required when VSOCK is used */
#include <net/if.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * On OsX, suppress NoPipe signals via flags to setsockopt()
 * On Linux, suppress NoPipe signals via flags to send()
 */
#if defined(__MACH__)
#    define NO_SIGNAL_SOCK_OPT SO_NOSIGPIPE
#    define NO_SIGNAL_SEND 0
#    define TCP_KEEPIDLE TCP_KEEPALIVE
#else
#    define NO_SIGNAL_SEND MSG_NOSIGNAL
#endif

/* This isn't defined on ancient linux distros (breaking the builds).
 * However, if this is a prebuild, we purposely build on an ancient system, but
 * we want the kernel calls to still be the same as a modern build since that's likely the target of the application
 * calling this code. Just define this if it isn't there already. GlibC and the kernel don't really care how the flag
 * gets passed as long as it does.
 */
#ifndef O_CLOEXEC
#    define O_CLOEXEC 02000000
#endif

#ifdef USE_VSOCK
#    if defined(__linux__) && defined(AF_VSOCK)
#        include <linux/vm_sockets.h>
#    else
#        error "USE_VSOCK not supported on current platform"
#    endif
#endif

/* other than CONNECTED_READ | CONNECTED_WRITE
 * a socket is only in one of these states at a time. */
enum socket_state {
    INIT = 0x01,
    CONNECTING = 0x02,
    CONNECTED_READ = 0x04,
    CONNECTED_WRITE = 0x08,
    BOUND = 0x10,
    LISTENING = 0x20,
    TIMEDOUT = 0x40,
    ERROR = 0x80,
    CLOSED,
};

static int s_convert_domain(enum aws_socket_domain domain) {
    switch (domain) {
        case AWS_SOCKET_IPV4:
            return AF_INET;
        case AWS_SOCKET_IPV6:
            return AF_INET6;
        case AWS_SOCKET_LOCAL:
            return AF_UNIX;
#ifdef USE_VSOCK
        case AWS_SOCKET_VSOCK:
            return AF_VSOCK;
#endif
        default:
            AWS_ASSERT(0);
            return AF_INET;
    }
}

static int s_convert_type(enum aws_socket_type type) {
    switch (type) {
        case AWS_SOCKET_STREAM:
            return SOCK_STREAM;
        case AWS_SOCKET_DGRAM:
            return SOCK_DGRAM;
        default:
            AWS_ASSERT(0);
            return SOCK_STREAM;
    }
}

static int s_determine_socket_error(int error) {
    switch (error) {
        case ECONNREFUSED:
            return AWS_IO_SOCKET_CONNECTION_REFUSED;
        case ECONNRESET:
            return AWS_IO_SOCKET_CLOSED;
        case ETIMEDOUT:
            return AWS_IO_SOCKET_TIMEOUT;
        case EHOSTUNREACH:
        case ENETUNREACH:
            return AWS_IO_SOCKET_NO_ROUTE_TO_HOST;
        case EADDRNOTAVAIL:
            return AWS_IO_SOCKET_INVALID_ADDRESS;
        case ENETDOWN:
            return AWS_IO_SOCKET_NETWORK_DOWN;
        case ECONNABORTED:
            return AWS_IO_SOCKET_CONNECT_ABORTED;
        case EADDRINUSE:
            return AWS_IO_SOCKET_ADDRESS_IN_USE;
        case ENOBUFS:
        case ENOMEM:
            return AWS_ERROR_OOM;
        case EAGAIN:
            return AWS_IO_READ_WOULD_BLOCK;
        case EMFILE:
        case ENFILE:
            return AWS_ERROR_MAX_FDS_EXCEEDED;
        case ENOENT:
        case EINVAL:
            return AWS_ERROR_FILE_INVALID_PATH;
        case EAFNOSUPPORT:
            return AWS_IO_SOCKET_UNSUPPORTED_ADDRESS_FAMILY;
        case EACCES:
            return AWS_ERROR_NO_PERMISSION;
        default:
            return AWS_IO_SOCKET_NOT_CONNECTED;
    }
}

static int s_create_socket(struct aws_socket *sock, const struct aws_socket_options *options) {

    int fd = socket(s_convert_domain(options->domain), s_convert_type(options->type), 0);
    int errno_value = errno; /* Always cache errno before potential side-effect */

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: initializing with domain %d and type %d",
        (void *)sock,
        fd,
        options->domain,
        options->type);
    if (fd != -1) {
        int flags = fcntl(fd, F_GETFL, 0);
        flags |= O_NONBLOCK | O_CLOEXEC;
        int success = fcntl(fd, F_SETFL, flags);
        (void)success;
        sock->io_handle.data.fd = fd;
        sock->io_handle.additional_data = NULL;
        return aws_socket_set_options(sock, options);
    }

    int aws_error = s_determine_socket_error(errno_value);
    return aws_raise_error(aws_error);
}

struct posix_socket_connect_args {
    struct aws_task task;
    struct aws_allocator *allocator;
    struct aws_socket *socket;
};

struct posix_socket {
    struct aws_linked_list write_queue;
    struct aws_linked_list written_queue;
    struct aws_task written_task;
    struct posix_socket_connect_args *connect_args;
    /* Note that only the posix_socket impl part is refcounted.
     * The public aws_socket can be a stack variable and cleaned up synchronously
     * (by blocking until the event-loop cleans up the impl part).
     * In hindsight, aws_socket should have been heap-allocated and refcounted, but alas */
    struct aws_ref_count internal_refcount;
    struct aws_allocator *allocator;
    bool written_task_scheduled;
    bool currently_subscribed;
    bool continue_accept;
    bool *close_happened;
};

static void s_socket_destroy_impl(void *user_data) {
    struct posix_socket *socket_impl = user_data;
    aws_mem_release(socket_impl->allocator, socket_impl);
}

static int s_socket_init(
    struct aws_socket *socket,
    struct aws_allocator *alloc,
    const struct aws_socket_options *options,
    int existing_socket_fd) {
    AWS_ASSERT(options);
    AWS_ZERO_STRUCT(*socket);

    struct posix_socket *posix_socket = aws_mem_calloc(alloc, 1, sizeof(struct posix_socket));
    if (!posix_socket) {
        socket->impl = NULL;
        return AWS_OP_ERR;
    }

    socket->allocator = alloc;
    socket->io_handle.data.fd = -1;
    socket->state = INIT;
    socket->options = *options;

    if (existing_socket_fd < 0) {
        int err = s_create_socket(socket, options);
        if (err) {
            aws_mem_release(alloc, posix_socket);
            socket->impl = NULL;
            return AWS_OP_ERR;
        }
    } else {
        socket->io_handle = (struct aws_io_handle){
            .data = {.fd = existing_socket_fd},
            .additional_data = NULL,
        };
        aws_socket_set_options(socket, options);
    }

    aws_linked_list_init(&posix_socket->write_queue);
    aws_linked_list_init(&posix_socket->written_queue);
    posix_socket->currently_subscribed = false;
    posix_socket->continue_accept = false;
    aws_ref_count_init(&posix_socket->internal_refcount, posix_socket, s_socket_destroy_impl);
    posix_socket->allocator = alloc;
    posix_socket->connect_args = NULL;
    posix_socket->close_happened = NULL;
    socket->impl = posix_socket;
    return AWS_OP_SUCCESS;
}

int aws_socket_init(struct aws_socket *socket, struct aws_allocator *alloc, const struct aws_socket_options *options) {
    AWS_ASSERT(options);
    return s_socket_init(socket, alloc, options, -1);
}

void aws_socket_clean_up(struct aws_socket *socket) {
    if (!socket->impl) {
        /* protect from double clean */
        return;
    }

    int fd_for_logging = socket->io_handle.data.fd; /* socket's fd gets reset before final log */
    (void)fd_for_logging;

    if (aws_socket_is_open(socket)) {
        AWS_LOGF_DEBUG(AWS_LS_IO_SOCKET, "id=%p fd=%d: is still open, closing...", (void *)socket, fd_for_logging);
        aws_socket_close(socket);
    }
    struct posix_socket *socket_impl = socket->impl;

    if (aws_ref_count_release(&socket_impl->internal_refcount) != 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: is still pending io letting it dangle and cleaning up later.",
            (void *)socket,
            fd_for_logging);
    }

    AWS_ZERO_STRUCT(*socket);
    socket->io_handle.data.fd = -1;
}

/* Update socket->local_endpoint based on the results of getsockname() */
static int s_update_local_endpoint(struct aws_socket *socket) {
    struct aws_socket_endpoint tmp_endpoint;
    AWS_ZERO_STRUCT(tmp_endpoint);

    struct sockaddr_storage address;
    AWS_ZERO_STRUCT(address);
    socklen_t address_size = sizeof(address);

    if (getsockname(socket->io_handle.data.fd, (struct sockaddr *)&address, &address_size) != 0) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: getsockname() failed with error %d",
            (void *)socket,
            socket->io_handle.data.fd,
            errno_value);
        int aws_error = s_determine_socket_error(errno_value);
        return aws_raise_error(aws_error);
    }

    if (address.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&address;
        tmp_endpoint.port = ntohs(s->sin_port);
        if (inet_ntop(AF_INET, &s->sin_addr, tmp_endpoint.address, sizeof(tmp_endpoint.address)) == NULL) {
            int errno_value = errno; /* Always cache errno before potential side-effect */

            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: inet_ntop() failed with error %d",
                (void *)socket,
                socket->io_handle.data.fd,
                errno_value);
            int aws_error = s_determine_socket_error(errno_value);
            return aws_raise_error(aws_error);
        }
    } else if (address.ss_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&address;
        tmp_endpoint.port = ntohs(s->sin6_port);
        if (inet_ntop(AF_INET6, &s->sin6_addr, tmp_endpoint.address, sizeof(tmp_endpoint.address)) == NULL) {
            int errno_value = errno; /* Always cache errno before potential side-effect */
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: inet_ntop() failed with error %d",
                (void *)socket,
                socket->io_handle.data.fd,
                errno_value);
            int aws_error = s_determine_socket_error(errno_value);
            return aws_raise_error(aws_error);
        }
    } else if (address.ss_family == AF_UNIX) {
        struct sockaddr_un *s = (struct sockaddr_un *)&address;

        /* Ensure there's a null-terminator.
         * On some platforms it may be missing when the path gets very long. See:
         * https://man7.org/linux/man-pages/man7/unix.7.html#BUGS
         * But let's keep it simple, and not deal with that madness until someone demands it. */
        size_t sun_len;
        if (aws_secure_strlen(s->sun_path, sizeof(tmp_endpoint.address), &sun_len)) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: UNIX domain socket name is too long",
                (void *)socket,
                socket->io_handle.data.fd);
            return aws_raise_error(AWS_IO_SOCKET_INVALID_ADDRESS);
        }
        memcpy(tmp_endpoint.address, s->sun_path, sun_len);
#if USE_VSOCK
    } else if (address.ss_family == AF_VSOCK) {
        struct sockaddr_vm *s = (struct sockaddr_vm *)&address;

        tmp_endpoint.port = s->svm_port;

        snprintf(tmp_endpoint.address, sizeof(tmp_endpoint.address), "%" PRIu32, s->svm_cid);
        return AWS_OP_SUCCESS;
#endif /* USE_VSOCK */
    } else {
        AWS_ASSERT(0);
        return aws_raise_error(AWS_IO_SOCKET_UNSUPPORTED_ADDRESS_FAMILY);
    }

    socket->local_endpoint = tmp_endpoint;
    return AWS_OP_SUCCESS;
}

static void s_on_connection_error(struct aws_socket *socket, int error);

static int s_on_connection_success(struct aws_socket *socket) {

    struct aws_event_loop *event_loop = socket->event_loop;
    struct posix_socket *socket_impl = socket->impl;

    if (socket_impl->currently_subscribed) {
        aws_event_loop_unsubscribe_from_io_events(socket->event_loop, &socket->io_handle);
        socket_impl->currently_subscribed = false;
    }

    socket->event_loop = NULL;

    int connect_result;
    socklen_t result_length = sizeof(connect_result);

    if (getsockopt(socket->io_handle.data.fd, SOL_SOCKET, SO_ERROR, &connect_result, &result_length) < 0) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_DEBUG(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: failed to determine connection error %d",
            (void *)socket,
            socket->io_handle.data.fd,
            errno_value);
        int aws_error = s_determine_socket_error(errno_value);
        aws_raise_error(aws_error);
        s_on_connection_error(socket, aws_error);
        return AWS_OP_ERR;
    }

    if (connect_result) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: connection error %d",
            (void *)socket,
            socket->io_handle.data.fd,
            connect_result);
        int aws_error = s_determine_socket_error(connect_result);
        aws_raise_error(aws_error);
        s_on_connection_error(socket, aws_error);
        return AWS_OP_ERR;
    }

    AWS_LOGF_INFO(AWS_LS_IO_SOCKET, "id=%p fd=%d: connection success", (void *)socket, socket->io_handle.data.fd);

    if (s_update_local_endpoint(socket)) {
        s_on_connection_error(socket, aws_last_error());
        return AWS_OP_ERR;
    }

    socket->state = CONNECTED_WRITE | CONNECTED_READ;

    if (aws_socket_assign_to_event_loop(socket, event_loop)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: assignment to event loop %p failed with error %d",
            (void *)socket,
            socket->io_handle.data.fd,
            (void *)event_loop,
            aws_last_error());
        s_on_connection_error(socket, aws_last_error());
        return AWS_OP_ERR;
    }

    socket->connection_result_fn(socket, AWS_ERROR_SUCCESS, socket->connect_accept_user_data);

    return AWS_OP_SUCCESS;
}

static void s_on_connection_error(struct aws_socket *socket, int error) {
    socket->state = ERROR;
    AWS_LOGF_DEBUG(AWS_LS_IO_SOCKET, "id=%p fd=%d: connection failure", (void *)socket, socket->io_handle.data.fd);
    if (socket->connection_result_fn) {
        socket->connection_result_fn(socket, error, socket->connect_accept_user_data);
    } else if (socket->accept_result_fn) {
        socket->accept_result_fn(socket, error, NULL, socket->connect_accept_user_data);
    }
}

/* the next two callbacks compete based on which one runs first. if s_socket_connect_event
 * comes back first, then we set socket_args->socket = NULL and continue on with the connection.
 * if s_handle_socket_timeout() runs first, is sees socket_args->socket is NULL and just cleans up its memory.
 * s_handle_socket_timeout() will always run so the memory for socket_connect_args is always cleaned up there. */
static void s_socket_connect_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {

    (void)event_loop;
    (void)handle;

    struct posix_socket_connect_args *socket_args = (struct posix_socket_connect_args *)user_data;
    AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "fd=%d: connection activity handler triggered ", handle->data.fd);

    if (socket_args->socket) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: has not timed out yet proceeding with connection.",
            (void *)socket_args->socket,
            handle->data.fd);

        struct posix_socket *socket_impl = socket_args->socket->impl;
        if (!(events & AWS_IO_EVENT_TYPE_ERROR || events & AWS_IO_EVENT_TYPE_CLOSED) &&
            (events & AWS_IO_EVENT_TYPE_READABLE || events & AWS_IO_EVENT_TYPE_WRITABLE)) {
            struct aws_socket *socket = socket_args->socket;
            socket_args->socket = NULL;
            socket_impl->connect_args = NULL;
            s_on_connection_success(socket);
            return;
        }

        int aws_error = aws_socket_get_error(socket_args->socket);
        /* we'll get another notification. */
        if (aws_error == AWS_IO_READ_WOULD_BLOCK) {
            AWS_LOGF_TRACE(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: spurious event, waiting for another notification.",
                (void *)socket_args->socket,
                handle->data.fd);
            return;
        }

        struct aws_socket *socket = socket_args->socket;
        socket_args->socket = NULL;
        socket_impl->connect_args = NULL;
        aws_raise_error(aws_error);
        s_on_connection_error(socket, aws_error);
    }
}

static void s_handle_socket_timeout(struct aws_task *task, void *args, aws_task_status status) {
    (void)task;
    (void)status;

    struct posix_socket_connect_args *socket_args = args;

    AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "task_id=%p: timeout task triggered, evaluating timeouts.", (void *)task);
    /* successful connection will have nulled out connect_args->socket */
    if (socket_args->socket) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: timed out, shutting down.",
            (void *)socket_args->socket,
            socket_args->socket->io_handle.data.fd);

        socket_args->socket->state = TIMEDOUT;
        int error_code = AWS_IO_SOCKET_TIMEOUT;

        if (status == AWS_TASK_STATUS_RUN_READY) {
            aws_event_loop_unsubscribe_from_io_events(socket_args->socket->event_loop, &socket_args->socket->io_handle);
        } else {
            error_code = AWS_IO_EVENT_LOOP_SHUTDOWN;
            aws_event_loop_free_io_event_resources(socket_args->socket->event_loop, &socket_args->socket->io_handle);
        }
        socket_args->socket->event_loop = NULL;
        struct posix_socket *socket_impl = socket_args->socket->impl;
        socket_impl->currently_subscribed = false;
        aws_raise_error(error_code);
        struct aws_socket *socket = socket_args->socket;
        /*socket close sets socket_args->socket to NULL and
         * socket_impl->connect_args to NULL. */
        aws_socket_close(socket);
        s_on_connection_error(socket, error_code);
    }

    aws_mem_release(socket_args->allocator, socket_args);
}

/* this is used simply for moving a connect_success callback when the connect finished immediately
 * (like for unix domain sockets) into the event loop's thread. Also note, in that case there was no
 * timeout task scheduled, so in this case the socket_args are cleaned up. */
static void s_run_connect_success(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct posix_socket_connect_args *socket_args = arg;

    if (socket_args->socket) {
        struct posix_socket *socket_impl = socket_args->socket->impl;
        if (status == AWS_TASK_STATUS_RUN_READY) {
            s_on_connection_success(socket_args->socket);
        } else {
            aws_raise_error(AWS_IO_SOCKET_CONNECT_ABORTED);
            socket_args->socket->event_loop = NULL;
            s_on_connection_error(socket_args->socket, AWS_IO_SOCKET_CONNECT_ABORTED);
        }
        socket_impl->connect_args = NULL;
    }

    aws_mem_release(socket_args->allocator, socket_args);
}

static inline int s_convert_pton_error(int pton_code, int errno_value) {
    if (pton_code == 0) {
        return AWS_IO_SOCKET_INVALID_ADDRESS;
    }

    return s_determine_socket_error(errno_value);
}

struct socket_address {
    union sock_addr_types {
        struct sockaddr_in addr_in;
        struct sockaddr_in6 addr_in6;
        struct sockaddr_un un_addr;
#ifdef USE_VSOCK
        struct sockaddr_vm vm_addr;
#endif
    } sock_addr_types;
};

#ifdef USE_VSOCK
/** Convert a string to a VSOCK CID. Respects the calling convetion of inet_pton:
 * 0 on error, 1 on success. */
static int parse_cid(const char *cid_str, unsigned int *value) {
    if (cid_str == NULL || value == NULL) {
        errno = EINVAL;
        return 0;
    }
    /* strtoll returns 0 as both error and correct value */
    errno = 0;
    /* unsigned long long to handle edge cases in convention explicitly */
    long long cid = strtoll(cid_str, NULL, 10);
    if (errno != 0) {
        return 0;
    }

    /* -1U means any, so it's a valid value, but it needs to be converted to
     * unsigned int. */
    if (cid == -1) {
        *value = VMADDR_CID_ANY;
        return 1;
    }

    if (cid < 0 || cid > UINT_MAX) {
        errno = ERANGE;
        return 0;
    }

    /* cast is safe here, edge cases already checked */
    *value = (unsigned int)cid;
    return 1;
}
#endif

int aws_socket_connect(
    struct aws_socket *socket,
    const struct aws_socket_endpoint *remote_endpoint,
    struct aws_event_loop *event_loop,
    aws_socket_on_connection_result_fn *on_connection_result,
    void *user_data) {
    AWS_ASSERT(event_loop);
    AWS_ASSERT(!socket->event_loop);

    AWS_LOGF_DEBUG(AWS_LS_IO_SOCKET, "id=%p fd=%d: beginning connect.", (void *)socket, socket->io_handle.data.fd);

    if (socket->event_loop) {
        return aws_raise_error(AWS_IO_EVENT_LOOP_ALREADY_ASSIGNED);
    }

    if (socket->options.type != AWS_SOCKET_DGRAM) {
        AWS_ASSERT(on_connection_result);
        if (socket->state != INIT) {
            return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
        }
    } else { /* UDP socket */
        /* UDP sockets jump to CONNECT_READ if bind is called first */
        if (socket->state != CONNECTED_READ && socket->state != INIT) {
            return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
        }
    }

    size_t address_strlen;
    if (aws_secure_strlen(remote_endpoint->address, AWS_ADDRESS_MAX_LEN, &address_strlen)) {
        return AWS_OP_ERR;
    }

    if (aws_socket_validate_port_for_connect(remote_endpoint->port, socket->options.domain)) {
        return AWS_OP_ERR;
    }

    struct socket_address address;
    AWS_ZERO_STRUCT(address);
    socklen_t sock_size = 0;
    int pton_err = 1;
    if (socket->options.domain == AWS_SOCKET_IPV4) {
        pton_err = inet_pton(AF_INET, remote_endpoint->address, &address.sock_addr_types.addr_in.sin_addr);
        address.sock_addr_types.addr_in.sin_port = htons((uint16_t)remote_endpoint->port);
        address.sock_addr_types.addr_in.sin_family = AF_INET;
        sock_size = sizeof(address.sock_addr_types.addr_in);
    } else if (socket->options.domain == AWS_SOCKET_IPV6) {
        pton_err = inet_pton(AF_INET6, remote_endpoint->address, &address.sock_addr_types.addr_in6.sin6_addr);
        address.sock_addr_types.addr_in6.sin6_port = htons((uint16_t)remote_endpoint->port);
        address.sock_addr_types.addr_in6.sin6_family = AF_INET6;
        sock_size = sizeof(address.sock_addr_types.addr_in6);
    } else if (socket->options.domain == AWS_SOCKET_LOCAL) {
        address.sock_addr_types.un_addr.sun_family = AF_UNIX;
        strncpy(address.sock_addr_types.un_addr.sun_path, remote_endpoint->address, AWS_ADDRESS_MAX_LEN);
        sock_size = sizeof(address.sock_addr_types.un_addr);
#ifdef USE_VSOCK
    } else if (socket->options.domain == AWS_SOCKET_VSOCK) {
        pton_err = parse_cid(remote_endpoint->address, &address.sock_addr_types.vm_addr.svm_cid);
        address.sock_addr_types.vm_addr.svm_family = AF_VSOCK;
        address.sock_addr_types.vm_addr.svm_port = remote_endpoint->port;
        sock_size = sizeof(address.sock_addr_types.vm_addr);
#endif
    } else {
        AWS_ASSERT(0);
        return aws_raise_error(AWS_IO_SOCKET_UNSUPPORTED_ADDRESS_FAMILY);
    }

    if (pton_err != 1) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_DEBUG(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: failed to parse address %s:%u.",
            (void *)socket,
            socket->io_handle.data.fd,
            remote_endpoint->address,
            remote_endpoint->port);
        return aws_raise_error(s_convert_pton_error(pton_err, errno_value));
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: connecting to endpoint %s:%u.",
        (void *)socket,
        socket->io_handle.data.fd,
        remote_endpoint->address,
        remote_endpoint->port);

    socket->state = CONNECTING;
    socket->remote_endpoint = *remote_endpoint;
    socket->connect_accept_user_data = user_data;
    socket->connection_result_fn = on_connection_result;

    struct posix_socket *socket_impl = socket->impl;

    socket_impl->connect_args = aws_mem_calloc(socket->allocator, 1, sizeof(struct posix_socket_connect_args));
    if (!socket_impl->connect_args) {
        return AWS_OP_ERR;
    }

    socket_impl->connect_args->socket = socket;
    socket_impl->connect_args->allocator = socket->allocator;

    socket_impl->connect_args->task.fn = s_handle_socket_timeout;
    socket_impl->connect_args->task.arg = socket_impl->connect_args;

    int error_code = connect(socket->io_handle.data.fd, (struct sockaddr *)&address.sock_addr_types, sock_size);
    socket->event_loop = event_loop;

    if (!error_code) {
        AWS_LOGF_INFO(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: connected immediately, not scheduling timeout.",
            (void *)socket,
            socket->io_handle.data.fd);
        socket_impl->connect_args->task.fn = s_run_connect_success;
        /* the subscription for IO will happen once we setup the connection in the task. Since we already
         * know the connection succeeded, we don't need to register for events yet. */
        aws_event_loop_schedule_task_now(event_loop, &socket_impl->connect_args->task);
    }

    if (error_code) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        if (errno_value == EINPROGRESS || errno_value == EALREADY) {
            AWS_LOGF_TRACE(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: connection pending waiting on event-loop notification or timeout.",
                (void *)socket,
                socket->io_handle.data.fd);
            /* cache the timeout task; it is possible for the IO subscription to come back virtually immediately
             * and null out the connect args */
            struct aws_task *timeout_task = &socket_impl->connect_args->task;

            socket_impl->currently_subscribed = true;
            /* This event is for when the connection finishes. (the fd will flip writable). */
            if (aws_event_loop_subscribe_to_io_events(
                    event_loop,
                    &socket->io_handle,
                    AWS_IO_EVENT_TYPE_WRITABLE,
                    s_socket_connect_event,
                    socket_impl->connect_args)) {
                AWS_LOGF_ERROR(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: failed to register with event-loop %p.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    (void *)event_loop);
                socket_impl->currently_subscribed = false;
                socket->event_loop = NULL;
                goto err_clean_up;
            }

            /* schedule a task to run at the connect timeout interval, if this task runs before the connect
             * happens, we consider that a timeout. */
            uint64_t timeout = 0;
            aws_event_loop_current_clock_time(event_loop, &timeout);
            timeout += aws_timestamp_convert(
                socket->options.connect_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);
            AWS_LOGF_TRACE(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: scheduling timeout task for %llu.",
                (void *)socket,
                socket->io_handle.data.fd,
                (unsigned long long)timeout);
            aws_event_loop_schedule_task_future(event_loop, timeout_task, timeout);
        } else {
            AWS_LOGF_DEBUG(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: connect failed with error code %d.",
                (void *)socket,
                socket->io_handle.data.fd,
                errno_value);
            int aws_error = s_determine_socket_error(errno_value);
            aws_raise_error(aws_error);
            socket->event_loop = NULL;
            socket_impl->currently_subscribed = false;
            goto err_clean_up;
        }
    }
    return AWS_OP_SUCCESS;

err_clean_up:
    aws_mem_release(socket->allocator, socket_impl->connect_args);
    socket_impl->connect_args = NULL;
    return AWS_OP_ERR;
}

int aws_socket_bind(struct aws_socket *socket, const struct aws_socket_endpoint *local_endpoint) {
    if (socket->state != INIT) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: invalid state for bind operation.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
    }

    size_t address_strlen;
    if (aws_secure_strlen(local_endpoint->address, AWS_ADDRESS_MAX_LEN, &address_strlen)) {
        return AWS_OP_ERR;
    }

    if (aws_socket_validate_port_for_bind(local_endpoint->port, socket->options.domain)) {
        return AWS_OP_ERR;
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: binding to %s:%u.",
        (void *)socket,
        socket->io_handle.data.fd,
        local_endpoint->address,
        local_endpoint->port);

    struct socket_address address;
    AWS_ZERO_STRUCT(address);
    socklen_t sock_size = 0;
    int pton_err = 1;
    if (socket->options.domain == AWS_SOCKET_IPV4) {
        pton_err = inet_pton(AF_INET, local_endpoint->address, &address.sock_addr_types.addr_in.sin_addr);
        address.sock_addr_types.addr_in.sin_port = htons((uint16_t)local_endpoint->port);
        address.sock_addr_types.addr_in.sin_family = AF_INET;
        sock_size = sizeof(address.sock_addr_types.addr_in);
    } else if (socket->options.domain == AWS_SOCKET_IPV6) {
        pton_err = inet_pton(AF_INET6, local_endpoint->address, &address.sock_addr_types.addr_in6.sin6_addr);
        address.sock_addr_types.addr_in6.sin6_port = htons((uint16_t)local_endpoint->port);
        address.sock_addr_types.addr_in6.sin6_family = AF_INET6;
        sock_size = sizeof(address.sock_addr_types.addr_in6);
    } else if (socket->options.domain == AWS_SOCKET_LOCAL) {
        address.sock_addr_types.un_addr.sun_family = AF_UNIX;
        strncpy(address.sock_addr_types.un_addr.sun_path, local_endpoint->address, AWS_ADDRESS_MAX_LEN);
        sock_size = sizeof(address.sock_addr_types.un_addr);
#ifdef USE_VSOCK
    } else if (socket->options.domain == AWS_SOCKET_VSOCK) {
        pton_err = parse_cid(local_endpoint->address, &address.sock_addr_types.vm_addr.svm_cid);
        address.sock_addr_types.vm_addr.svm_family = AF_VSOCK;
        address.sock_addr_types.vm_addr.svm_port = local_endpoint->port;
        sock_size = sizeof(address.sock_addr_types.vm_addr);
#endif
    } else {
        AWS_ASSERT(0);
        return aws_raise_error(AWS_IO_SOCKET_UNSUPPORTED_ADDRESS_FAMILY);
    }

    if (pton_err != 1) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: failed to parse address %s:%u.",
            (void *)socket,
            socket->io_handle.data.fd,
            local_endpoint->address,
            local_endpoint->port);
        return aws_raise_error(s_convert_pton_error(pton_err, errno_value));
    }

    if (bind(socket->io_handle.data.fd, (struct sockaddr *)&address.sock_addr_types, sock_size) != 0) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: bind failed with error code %d",
            (void *)socket,
            socket->io_handle.data.fd,
            errno_value);

        aws_raise_error(s_determine_socket_error(errno_value));
        goto error;
    }

    if (s_update_local_endpoint(socket)) {
        goto error;
    }

    if (socket->options.type == AWS_SOCKET_STREAM) {
        socket->state = BOUND;
    } else {
        /* e.g. UDP is now readable */
        socket->state = CONNECTED_READ;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: successfully bound to %s:%u",
        (void *)socket,
        socket->io_handle.data.fd,
        socket->local_endpoint.address,
        socket->local_endpoint.port);

    return AWS_OP_SUCCESS;

error:
    socket->state = ERROR;
    return AWS_OP_ERR;
}

int aws_socket_get_bound_address(const struct aws_socket *socket, struct aws_socket_endpoint *out_address) {
    if (socket->local_endpoint.address[0] == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: Socket has no local address. Socket must be bound first.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
    }
    *out_address = socket->local_endpoint;
    return AWS_OP_SUCCESS;
}

int aws_socket_listen(struct aws_socket *socket, int backlog_size) {
    if (socket->state != BOUND) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: invalid state for listen operation. You must call bind first.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
    }

    int error_code = listen(socket->io_handle.data.fd, backlog_size);

    if (!error_code) {
        AWS_LOGF_INFO(
            AWS_LS_IO_SOCKET, "id=%p fd=%d: successfully listening", (void *)socket, socket->io_handle.data.fd);
        socket->state = LISTENING;
        return AWS_OP_SUCCESS;
    }

    int errno_value = errno; /* Always cache errno before potential side-effect */

    AWS_LOGF_ERROR(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: listen failed with error code %d",
        (void *)socket,
        socket->io_handle.data.fd,
        errno_value);

    socket->state = ERROR;

    return aws_raise_error(s_determine_socket_error(errno_value));
}

/* this is called by the event loop handler that was installed in start_accept(). It runs once the FD goes readable,
 * accepts as many as it can and then returns control to the event loop. */
static void s_socket_accept_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {

    (void)event_loop;

    struct aws_socket *socket = user_data;
    struct posix_socket *socket_impl = socket->impl;

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET, "id=%p fd=%d: listening event received", (void *)socket, socket->io_handle.data.fd);

    if (socket_impl->continue_accept && events & AWS_IO_EVENT_TYPE_READABLE) {
        int in_fd = 0;
        while (socket_impl->continue_accept && in_fd != -1) {
            struct sockaddr_storage in_addr;
            socklen_t in_len = sizeof(struct sockaddr_storage);

            in_fd = accept(handle->data.fd, (struct sockaddr *)&in_addr, &in_len);
            if (in_fd == -1) {
                int errno_value = errno; /* Always cache errno before potential side-effect */

                if (errno_value == EAGAIN || errno_value == EWOULDBLOCK) {
                    break;
                }

                int aws_error = aws_socket_get_error(socket);
                aws_raise_error(aws_error);
                s_on_connection_error(socket, aws_error);
                break;
            }

            AWS_LOGF_DEBUG(
                AWS_LS_IO_SOCKET, "id=%p fd=%d: incoming connection", (void *)socket, socket->io_handle.data.fd);

            struct aws_socket *new_sock = aws_mem_acquire(socket->allocator, sizeof(struct aws_socket));

            if (!new_sock) {
                close(in_fd);
                s_on_connection_error(socket, aws_last_error());
                continue;
            }

            if (s_socket_init(new_sock, socket->allocator, &socket->options, in_fd)) {
                aws_mem_release(socket->allocator, new_sock);
                s_on_connection_error(socket, aws_last_error());
                continue;
            }

            new_sock->local_endpoint = socket->local_endpoint;
            new_sock->state = CONNECTED_READ | CONNECTED_WRITE;
            uint32_t port = 0;

            /* get the info on the incoming socket's address */
            if (in_addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&in_addr;
                port = ntohs(s->sin_port);
                /* this came from the kernel, a.) it won't fail. b.) even if it does
                 * its not fatal. come back and add logging later. */
                if (!inet_ntop(
                        AF_INET,
                        &s->sin_addr,
                        new_sock->remote_endpoint.address,
                        sizeof(new_sock->remote_endpoint.address))) {
                    AWS_LOGF_WARN(
                        AWS_LS_IO_SOCKET,
                        "id=%p fd=%d:. Failed to determine remote address.",
                        (void *)socket,
                        socket->io_handle.data.fd);
                }
                new_sock->options.domain = AWS_SOCKET_IPV4;
            } else if (in_addr.ss_family == AF_INET6) {
                /* this came from the kernel, a.) it won't fail. b.) even if it does
                 * its not fatal. come back and add logging later. */
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&in_addr;
                port = ntohs(s->sin6_port);
                if (!inet_ntop(
                        AF_INET6,
                        &s->sin6_addr,
                        new_sock->remote_endpoint.address,
                        sizeof(new_sock->remote_endpoint.address))) {
                    AWS_LOGF_WARN(
                        AWS_LS_IO_SOCKET,
                        "id=%p fd=%d:. Failed to determine remote address.",
                        (void *)socket,
                        socket->io_handle.data.fd);
                }
                new_sock->options.domain = AWS_SOCKET_IPV6;
            } else if (in_addr.ss_family == AF_UNIX) {
                new_sock->remote_endpoint = socket->local_endpoint;
                new_sock->options.domain = AWS_SOCKET_LOCAL;
            }

            new_sock->remote_endpoint.port = port;

            AWS_LOGF_INFO(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: connected to %s:%d, incoming fd %d",
                (void *)socket,
                socket->io_handle.data.fd,
                new_sock->remote_endpoint.address,
                new_sock->remote_endpoint.port,
                in_fd);

            int flags = fcntl(in_fd, F_GETFL, 0);

            flags |= O_NONBLOCK | O_CLOEXEC;
            fcntl(in_fd, F_SETFL, flags);

            bool close_occurred = false;
            socket_impl->close_happened = &close_occurred;
            socket->accept_result_fn(socket, AWS_ERROR_SUCCESS, new_sock, socket->connect_accept_user_data);

            if (close_occurred) {
                return;
            }

            socket_impl->close_happened = NULL;
        }
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: finished processing incoming connections, "
        "waiting on event-loop notification",
        (void *)socket,
        socket->io_handle.data.fd);
}

int aws_socket_start_accept(
    struct aws_socket *socket,
    struct aws_event_loop *accept_loop,
    aws_socket_on_accept_result_fn *on_accept_result,
    void *user_data) {
    AWS_ASSERT(on_accept_result);
    AWS_ASSERT(accept_loop);

    if (socket->event_loop) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: is already assigned to event-loop %p.",
            (void *)socket,
            socket->io_handle.data.fd,
            (void *)socket->event_loop);
        return aws_raise_error(AWS_IO_EVENT_LOOP_ALREADY_ASSIGNED);
    }

    if (socket->state != LISTENING) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: invalid state for start_accept operation. You must call listen first.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
    }

    socket->accept_result_fn = on_accept_result;
    socket->connect_accept_user_data = user_data;
    socket->event_loop = accept_loop;
    struct posix_socket *socket_impl = socket->impl;
    socket_impl->continue_accept = true;
    socket_impl->currently_subscribed = true;

    if (aws_event_loop_subscribe_to_io_events(
            socket->event_loop, &socket->io_handle, AWS_IO_EVENT_TYPE_READABLE, s_socket_accept_event, socket)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: failed to subscribe to event-loop %p.",
            (void *)socket,
            socket->io_handle.data.fd,
            (void *)socket->event_loop);
        socket_impl->continue_accept = false;
        socket_impl->currently_subscribed = false;
        socket->event_loop = NULL;

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

struct stop_accept_args {
    struct aws_task task;
    struct aws_mutex mutex;
    struct aws_condition_variable condition_variable;
    struct aws_socket *socket;
    int ret_code;
    bool invoked;
};

static bool s_stop_accept_pred(void *arg) {
    struct stop_accept_args *stop_accept_args = arg;
    return stop_accept_args->invoked;
}

static void s_stop_accept_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;

    struct stop_accept_args *stop_accept_args = arg;
    aws_mutex_lock(&stop_accept_args->mutex);
    stop_accept_args->ret_code = AWS_OP_SUCCESS;
    if (aws_socket_stop_accept(stop_accept_args->socket)) {
        stop_accept_args->ret_code = aws_last_error();
    }
    stop_accept_args->invoked = true;
    aws_condition_variable_notify_one(&stop_accept_args->condition_variable);
    aws_mutex_unlock(&stop_accept_args->mutex);
}

int aws_socket_stop_accept(struct aws_socket *socket) {
    if (socket->state != LISTENING) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: is not in a listening state, can't stop_accept.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
    }

    AWS_LOGF_INFO(
        AWS_LS_IO_SOCKET, "id=%p fd=%d: stopping accepting new connections", (void *)socket, socket->io_handle.data.fd);

    if (!aws_event_loop_thread_is_callers_thread(socket->event_loop)) {
        struct stop_accept_args args = {
            .mutex = AWS_MUTEX_INIT,
            .condition_variable = AWS_CONDITION_VARIABLE_INIT,
            .invoked = false,
            .socket = socket,
            .ret_code = AWS_OP_SUCCESS,
            .task = {.fn = s_stop_accept_task},
        };
        AWS_LOGF_INFO(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: stopping accepting new connections from a different thread than "
            "the socket is running from. Blocking until it shuts down.",
            (void *)socket,
            socket->io_handle.data.fd);
        /* Look.... I know what I'm doing.... trust me, I'm an engineer.
         * We wait on the completion before 'args' goes out of scope.
         * NOLINTNEXTLINE */
        args.task.arg = &args;
        aws_mutex_lock(&args.mutex);
        aws_event_loop_schedule_task_now(socket->event_loop, &args.task);
        aws_condition_variable_wait_pred(&args.condition_variable, &args.mutex, s_stop_accept_pred, &args);
        aws_mutex_unlock(&args.mutex);
        AWS_LOGF_INFO(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: stop accept task finished running.",
            (void *)socket,
            socket->io_handle.data.fd);

        if (args.ret_code) {
            return aws_raise_error(args.ret_code);
        }
        return AWS_OP_SUCCESS;
    }

    int ret_val = AWS_OP_SUCCESS;
    struct posix_socket *socket_impl = socket->impl;
    if (socket_impl->currently_subscribed) {
        ret_val = aws_event_loop_unsubscribe_from_io_events(socket->event_loop, &socket->io_handle);
        socket_impl->currently_subscribed = false;
        socket_impl->continue_accept = false;
        socket->event_loop = NULL;
    }

    return ret_val;
}

int aws_socket_set_options(struct aws_socket *socket, const struct aws_socket_options *options) {
    if (socket->options.domain != options->domain || socket->options.type != options->type) {
        return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: setting socket options to: keep-alive %d, keep-alive timeout %d, keep-alive interval %d, "
        "keep-alive probe "
        "count %d.",
        (void *)socket,
        socket->io_handle.data.fd,
        (int)options->keepalive,
        (int)options->keep_alive_timeout_sec,
        (int)options->keep_alive_interval_sec,
        (int)options->keep_alive_max_failed_probes);

    socket->options = *options;

#ifdef NO_SIGNAL_SOCK_OPT
    int option_value = 1;
    if (AWS_UNLIKELY(setsockopt(
            socket->io_handle.data.fd, SOL_SOCKET, NO_SIGNAL_SOCK_OPT, &option_value, sizeof(option_value)))) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_WARN(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: setsockopt() for NO_SIGNAL_SOCK_OPT failed with errno %d.",
            (void *)socket,
            socket->io_handle.data.fd,
            errno_value);
    }
#endif /* NO_SIGNAL_SOCK_OPT */

    int reuse = 1;
    if (AWS_UNLIKELY(setsockopt(socket->io_handle.data.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)))) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        AWS_LOGF_WARN(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: setsockopt() for SO_REUSEADDR failed with errno %d.",
            (void *)socket,
            socket->io_handle.data.fd,
            errno_value);
    }
    size_t network_interface_length = 0;
    if (aws_secure_strlen(options->network_interface_name, AWS_NETWORK_INTERFACE_NAME_MAX, &network_interface_length)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: network_interface_name max length must be %d length and NULL terminated",
            (void *)socket,
            socket->io_handle.data.fd,
            AWS_NETWORK_INTERFACE_NAME_MAX);
        return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
    }
    if (network_interface_length != 0) {
#if defined(SO_BINDTODEVICE)
        if (setsockopt(
                socket->io_handle.data.fd,
                SOL_SOCKET,
                SO_BINDTODEVICE,
                options->network_interface_name,
                network_interface_length)) {
            int errno_value = errno; /* Always cache errno before potential side-effect */
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: setsockopt() with SO_BINDTODEVICE for \"%s\" failed with errno %d.",
                (void *)socket,
                socket->io_handle.data.fd,
                options->network_interface_name,
                errno_value);
            return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
        }
#elif defined(IP_BOUND_IF)
        /*
         * If SO_BINDTODEVICE is not supported, the alternative is IP_BOUND_IF which requires an index instead
         * of a name. We are not using this everywhere because this requires 2 system calls instead of 1, and is
         * dependent upon the type of sockets, which doesn't support AWS_SOCKET_LOCAL. As a future optimization, we can
         * look into caching the result of if_nametoindex.
         */
        uint network_interface_index = if_nametoindex(options->network_interface_name);
        if (network_interface_index == 0) {
            int errno_value = errno; /* Always cache errno before potential side-effect */
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: network_interface_name \"%s\" not found. if_nametoindex() failed with errno %d.",
                (void *)socket,
                socket->io_handle.data.fd,
                options->network_interface_name,
                errno_value);
            return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
        }
        if (options->domain == AWS_SOCKET_IPV6) {
            if (setsockopt(
                    socket->io_handle.data.fd,
                    IPPROTO_IPV6,
                    IPV6_BOUND_IF,
                    &network_interface_index,
                    sizeof(network_interface_index))) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                AWS_LOGF_ERROR(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: setsockopt() with IPV6_BOUND_IF for \"%s\" failed with errno %d.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    options->network_interface_name,
                    errno_value);
                return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
            }
        } else if (setsockopt(
                       socket->io_handle.data.fd,
                       IPPROTO_IP,
                       IP_BOUND_IF,
                       &network_interface_index,
                       sizeof(network_interface_index))) {
            int errno_value = errno; /* Always cache errno before potential side-effect */
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: setsockopt() with IP_BOUND_IF for \"%s\" failed with errno %d.",
                (void *)socket,
                socket->io_handle.data.fd,
                options->network_interface_name,
                errno_value);
            return aws_raise_error(AWS_IO_SOCKET_INVALID_OPTIONS);
        }
#else
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: network_interface_name is not supported on this platform.",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_ERROR_PLATFORM_NOT_SUPPORTED);
#endif
    }
    if (options->type == AWS_SOCKET_STREAM && options->domain != AWS_SOCKET_LOCAL) {
        if (socket->options.keepalive) {
            int keep_alive = 1;
            if (AWS_UNLIKELY(
                    setsockopt(socket->io_handle.data.fd, SOL_SOCKET, SO_KEEPALIVE, &keep_alive, sizeof(int)))) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                AWS_LOGF_WARN(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: setsockopt() for enabling SO_KEEPALIVE failed with errno %d.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    errno_value);
            }
        }

#if !defined(__OpenBSD__)
        if (socket->options.keep_alive_interval_sec && socket->options.keep_alive_timeout_sec) {
            int ival_in_secs = socket->options.keep_alive_interval_sec;
            if (AWS_UNLIKELY(setsockopt(
                    socket->io_handle.data.fd, IPPROTO_TCP, TCP_KEEPIDLE, &ival_in_secs, sizeof(ival_in_secs)))) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                AWS_LOGF_WARN(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: setsockopt() for enabling TCP_KEEPIDLE for TCP failed with errno %d.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    errno_value);
            }

            ival_in_secs = socket->options.keep_alive_timeout_sec;
            if (AWS_UNLIKELY(setsockopt(
                    socket->io_handle.data.fd, IPPROTO_TCP, TCP_KEEPINTVL, &ival_in_secs, sizeof(ival_in_secs)))) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                AWS_LOGF_WARN(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: setsockopt() for enabling TCP_KEEPINTVL for TCP failed with errno %d.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    errno_value);
            }
        }

        if (socket->options.keep_alive_max_failed_probes) {
            int max_probes = socket->options.keep_alive_max_failed_probes;
            if (AWS_UNLIKELY(
                    setsockopt(socket->io_handle.data.fd, IPPROTO_TCP, TCP_KEEPCNT, &max_probes, sizeof(max_probes)))) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                AWS_LOGF_WARN(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: setsockopt() for enabling TCP_KEEPCNT for TCP failed with errno %d.",
                    (void *)socket,
                    socket->io_handle.data.fd,
                    errno_value);
            }
        }
#endif /* __OpenBSD__ */
    }

    return AWS_OP_SUCCESS;
}

struct socket_write_request {
    struct aws_byte_cursor cursor_cpy;
    aws_socket_on_write_completed_fn *written_fn;
    void *write_user_data;
    struct aws_linked_list_node node;
    size_t original_buffer_len;
    int error_code;
};

struct posix_socket_close_args {
    struct aws_mutex mutex;
    struct aws_condition_variable condition_variable;
    struct aws_socket *socket;
    bool invoked;
    int ret_code;
};

static bool s_close_predicate(void *arg) {
    struct posix_socket_close_args *close_args = arg;
    return close_args->invoked;
}

static void s_close_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;

    struct posix_socket_close_args *close_args = arg;
    aws_mutex_lock(&close_args->mutex);
    close_args->ret_code = AWS_OP_SUCCESS;

    if (aws_socket_close(close_args->socket)) {
        close_args->ret_code = aws_last_error();
    }

    close_args->invoked = true;
    aws_condition_variable_notify_one(&close_args->condition_variable);
    aws_mutex_unlock(&close_args->mutex);
}

int aws_socket_close(struct aws_socket *socket) {
    struct posix_socket *socket_impl = socket->impl;
    AWS_LOGF_DEBUG(AWS_LS_IO_SOCKET, "id=%p fd=%d: closing", (void *)socket, socket->io_handle.data.fd);
    struct aws_event_loop *event_loop = socket->event_loop;
    if (socket->event_loop) {
        /* don't freak out on me, this almost never happens, and never occurs inside a channel
         * it only gets hit from a listening socket shutting down or from a unit test. */
        if (!aws_event_loop_thread_is_callers_thread(socket->event_loop)) {
            AWS_LOGF_INFO(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: closing from a different thread than "
                "the socket is running from. Blocking until it closes down.",
                (void *)socket,
                socket->io_handle.data.fd);
            /* the only time we allow this kind of thing is when you're a listener.*/
            if (socket->state != LISTENING) {
                return aws_raise_error(AWS_IO_SOCKET_ILLEGAL_OPERATION_FOR_STATE);
            }

            struct posix_socket_close_args args = {
                .mutex = AWS_MUTEX_INIT,
                .condition_variable = AWS_CONDITION_VARIABLE_INIT,
                .socket = socket,
                .ret_code = AWS_OP_SUCCESS,
                .invoked = false,
            };

            struct aws_task close_task = {
                .fn = s_close_task,
                .arg = &args,
            };

            int fd_for_logging = socket->io_handle.data.fd; /* socket's fd gets reset before final log */
            (void)fd_for_logging;

            aws_mutex_lock(&args.mutex);
            aws_event_loop_schedule_task_now(socket->event_loop, &close_task);
            aws_condition_variable_wait_pred(&args.condition_variable, &args.mutex, s_close_predicate, &args);
            aws_mutex_unlock(&args.mutex);
            AWS_LOGF_INFO(AWS_LS_IO_SOCKET, "id=%p fd=%d: close task completed.", (void *)socket, fd_for_logging);
            if (args.ret_code) {
                return aws_raise_error(args.ret_code);
            }

            return AWS_OP_SUCCESS;
        }

        if (socket_impl->currently_subscribed) {
            if (socket->state & LISTENING) {
                aws_socket_stop_accept(socket);
            } else {
                int err_code = aws_event_loop_unsubscribe_from_io_events(socket->event_loop, &socket->io_handle);

                if (err_code) {
                    return AWS_OP_ERR;
                }
            }
            socket_impl->currently_subscribed = false;
            socket->event_loop = NULL;
        }
    }

    if (socket_impl->close_happened) {
        *socket_impl->close_happened = true;
    }

    if (socket_impl->connect_args) {
        socket_impl->connect_args->socket = NULL;
        socket_impl->connect_args = NULL;
    }

    if (aws_socket_is_open(socket)) {
        close(socket->io_handle.data.fd);
        socket->io_handle.data.fd = -1;
        socket->state = CLOSED;

        /* ensure callbacks for pending writes fire (in order) before this close function returns */

        if (socket_impl->written_task_scheduled) {
            aws_event_loop_cancel_task(event_loop, &socket_impl->written_task);
        }

        while (!aws_linked_list_empty(&socket_impl->written_queue)) {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&socket_impl->written_queue);
            struct socket_write_request *write_request = AWS_CONTAINER_OF(node, struct socket_write_request, node);
            size_t bytes_written = write_request->original_buffer_len - write_request->cursor_cpy.len;
            write_request->written_fn(socket, write_request->error_code, bytes_written, write_request->write_user_data);
            aws_mem_release(socket->allocator, write_request);
        }

        while (!aws_linked_list_empty(&socket_impl->write_queue)) {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&socket_impl->write_queue);
            struct socket_write_request *write_request = AWS_CONTAINER_OF(node, struct socket_write_request, node);
            size_t bytes_written = write_request->original_buffer_len - write_request->cursor_cpy.len;
            write_request->written_fn(socket, AWS_IO_SOCKET_CLOSED, bytes_written, write_request->write_user_data);
            aws_mem_release(socket->allocator, write_request);
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_socket_shutdown_dir(struct aws_socket *socket, enum aws_channel_direction dir) {
    int how = dir == AWS_CHANNEL_DIR_READ ? 0 : 1;
    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET, "id=%p fd=%d: shutting down in direction %d", (void *)socket, socket->io_handle.data.fd, dir);
    if (shutdown(socket->io_handle.data.fd, how)) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        int aws_error = s_determine_socket_error(errno_value);
        return aws_raise_error(aws_error);
    }

    if (dir == AWS_CHANNEL_DIR_READ) {
        socket->state &= ~CONNECTED_READ;
    } else {
        socket->state &= ~CONNECTED_WRITE;
    }

    return AWS_OP_SUCCESS;
}

static void s_written_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    (void)status;

    struct aws_socket *socket = arg;
    struct posix_socket *socket_impl = socket->impl;

    socket_impl->written_task_scheduled = false;

    /* this is to handle a race condition when a callback kicks off a cleanup, or the user decides
     * to close the socket based on something they read (SSL validation failed for example).
     * if clean_up happens when internal_refcount > 0, socket_impl is kept dangling */
    aws_ref_count_acquire(&socket_impl->internal_refcount);

    /* Notes about weird loop:
     * 1) Only process the initial contents of queue when this task is run,
     *    ignoring any writes queued during delivery.
     *    If we simply looped until the queue was empty, we could get into a
     *    synchronous loop of completing and writing and completing and writing...
     *    and it would be tough for multiple sockets to share an event-loop fairly.
     * 2) Check if queue is empty with each iteration.
     *    If user calls close() from the callback, close() will process all
     *    nodes in the written_queue, and the queue will be empty when the
     *    callstack gets back to here. */
    if (!aws_linked_list_empty(&socket_impl->written_queue)) {
        struct aws_linked_list_node *stop_after = aws_linked_list_back(&socket_impl->written_queue);
        do {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&socket_impl->written_queue);
            struct socket_write_request *write_request = AWS_CONTAINER_OF(node, struct socket_write_request, node);
            size_t bytes_written = write_request->original_buffer_len - write_request->cursor_cpy.len;
            write_request->written_fn(socket, write_request->error_code, bytes_written, write_request->write_user_data);
            aws_mem_release(socket_impl->allocator, write_request);
            if (node == stop_after) {
                break;
            }
        } while (!aws_linked_list_empty(&socket_impl->written_queue));
    }

    aws_ref_count_release(&socket_impl->internal_refcount);
}

/* this gets called in two scenarios.
 * 1st scenario, someone called aws_socket_write() and we want to try writing now, so an error can be returned
 * immediately if something bad has happened to the socket. In this case, `parent_request` is set.
 * 2nd scenario, the event loop notified us that the socket went writable. In this case `parent_request` is NULL */
static int s_process_socket_write_requests(struct aws_socket *socket, struct socket_write_request *parent_request) {
    struct posix_socket *socket_impl = socket->impl;

    if (parent_request) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: processing write requests, called from aws_socket_write",
            (void *)socket,
            socket->io_handle.data.fd);
    } else {
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: processing write requests, invoked by the event-loop",
            (void *)socket,
            socket->io_handle.data.fd);
    }

    bool purge = false;
    int aws_error = AWS_OP_SUCCESS;
    bool parent_request_failed = false;
    bool pushed_to_written_queue = false;

    /* if a close call happens in the middle, this queue will have been cleaned out from under us. */
    while (!aws_linked_list_empty(&socket_impl->write_queue)) {
        struct aws_linked_list_node *node = aws_linked_list_front(&socket_impl->write_queue);
        struct socket_write_request *write_request = AWS_CONTAINER_OF(node, struct socket_write_request, node);

        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: dequeued write request of size %llu, remaining to write %llu",
            (void *)socket,
            socket->io_handle.data.fd,
            (unsigned long long)write_request->original_buffer_len,
            (unsigned long long)write_request->cursor_cpy.len);

        ssize_t written = send(
            socket->io_handle.data.fd, write_request->cursor_cpy.ptr, write_request->cursor_cpy.len, NO_SIGNAL_SEND);
        int errno_value = errno; /* Always cache errno before potential side-effect */

        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: send written size %d",
            (void *)socket,
            socket->io_handle.data.fd,
            (int)written);

        if (written < 0) {
            if (errno_value == EAGAIN) {
                AWS_LOGF_TRACE(
                    AWS_LS_IO_SOCKET, "id=%p fd=%d: returned would block", (void *)socket, socket->io_handle.data.fd);
                break;
            }

            if (errno_value == EPIPE) {
                AWS_LOGF_DEBUG(
                    AWS_LS_IO_SOCKET,
                    "id=%p fd=%d: already closed before write",
                    (void *)socket,
                    socket->io_handle.data.fd);
                aws_error = AWS_IO_SOCKET_CLOSED;
                aws_raise_error(aws_error);
                purge = true;
                break;
            }

            purge = true;
            AWS_LOGF_DEBUG(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: write error with error code %d",
                (void *)socket,
                socket->io_handle.data.fd,
                errno_value);
            aws_error = s_determine_socket_error(errno_value);
            aws_raise_error(aws_error);
            break;
        }

        size_t remaining_to_write = write_request->cursor_cpy.len;

        aws_byte_cursor_advance(&write_request->cursor_cpy, (size_t)written);
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: remaining write request to write %llu",
            (void *)socket,
            socket->io_handle.data.fd,
            (unsigned long long)write_request->cursor_cpy.len);

        if ((size_t)written == remaining_to_write) {
            AWS_LOGF_TRACE(
                AWS_LS_IO_SOCKET, "id=%p fd=%d: write request completed", (void *)socket, socket->io_handle.data.fd);

            aws_linked_list_remove(node);
            write_request->error_code = AWS_ERROR_SUCCESS;
            aws_linked_list_push_back(&socket_impl->written_queue, node);
            pushed_to_written_queue = true;
        }
    }

    if (purge) {
        while (!aws_linked_list_empty(&socket_impl->write_queue)) {
            struct aws_linked_list_node *node = aws_linked_list_pop_front(&socket_impl->write_queue);
            struct socket_write_request *write_request = AWS_CONTAINER_OF(node, struct socket_write_request, node);

            /* If this fn was invoked directly from aws_socket_write(), don't invoke the error callback
             * as the user will be able to rely on the return value from aws_socket_write() */
            if (write_request == parent_request) {
                parent_request_failed = true;
                aws_mem_release(socket->allocator, write_request);
            } else {
                write_request->error_code = aws_error;
                aws_linked_list_push_back(&socket_impl->written_queue, node);
                pushed_to_written_queue = true;
            }
        }
    }

    if (pushed_to_written_queue && !socket_impl->written_task_scheduled) {
        socket_impl->written_task_scheduled = true;
        aws_task_init(&socket_impl->written_task, s_written_task, socket, "socket_written_task");
        aws_event_loop_schedule_task_now(socket->event_loop, &socket_impl->written_task);
    }

    /* Only report error if aws_socket_write() invoked this function and its write_request failed */
    if (!parent_request_failed) {
        return AWS_OP_SUCCESS;
    }

    aws_raise_error(aws_error);
    return AWS_OP_ERR;
}

static void s_on_socket_io_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {
    (void)event_loop;
    (void)handle;
    struct aws_socket *socket = user_data;
    struct posix_socket *socket_impl = socket->impl;

    /* this is to handle a race condition when an error kicks off a cleanup, or the user decides
     * to close the socket based on something they read (SSL validation failed for example).
     * if clean_up happens when internal_refcount > 0, socket_impl is kept dangling but currently
     * subscribed is set to false. */
    aws_ref_count_acquire(&socket_impl->internal_refcount);

    /* NOTE: READABLE|WRITABLE|HANG_UP events might arrive simultaneously
     * (e.g. peer sends last few bytes and immediately hangs up).
     * Notify user of READABLE|WRITABLE events first, so they try to read any remaining bytes. */

    if (socket_impl->currently_subscribed && events & AWS_IO_EVENT_TYPE_READABLE) {
        AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "id=%p fd=%d: is readable", (void *)socket, socket->io_handle.data.fd);
        if (socket->readable_fn) {
            socket->readable_fn(socket, AWS_OP_SUCCESS, socket->readable_user_data);
        }
    }
    /* if socket closed in between these branches, the currently_subscribed will be false and socket_impl will not
     * have been cleaned up, so this next branch is safe. */
    if (socket_impl->currently_subscribed && events & AWS_IO_EVENT_TYPE_WRITABLE) {
        AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "id=%p fd=%d: is writable", (void *)socket, socket->io_handle.data.fd);
        s_process_socket_write_requests(socket, NULL);
    }

    if (events & AWS_IO_EVENT_TYPE_REMOTE_HANG_UP || events & AWS_IO_EVENT_TYPE_CLOSED) {
        aws_raise_error(AWS_IO_SOCKET_CLOSED);
        AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "id=%p fd=%d: closed remotely", (void *)socket, socket->io_handle.data.fd);
        if (socket->readable_fn) {
            socket->readable_fn(socket, AWS_IO_SOCKET_CLOSED, socket->readable_user_data);
        }
        goto end_check;
    }

    if (socket_impl->currently_subscribed && events & AWS_IO_EVENT_TYPE_ERROR) {
        int aws_error = aws_socket_get_error(socket);
        aws_raise_error(aws_error);
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET, "id=%p fd=%d: error event occurred", (void *)socket, socket->io_handle.data.fd);
        if (socket->readable_fn) {
            socket->readable_fn(socket, aws_error, socket->readable_user_data);
        }
        goto end_check;
    }

end_check:
    aws_ref_count_release(&socket_impl->internal_refcount);
}

int aws_socket_assign_to_event_loop(struct aws_socket *socket, struct aws_event_loop *event_loop) {
    if (!socket->event_loop) {
        AWS_LOGF_DEBUG(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: assigning to event loop %p",
            (void *)socket,
            socket->io_handle.data.fd,
            (void *)event_loop);
        socket->event_loop = event_loop;
        struct posix_socket *socket_impl = socket->impl;
        socket_impl->currently_subscribed = true;
        if (aws_event_loop_subscribe_to_io_events(
                event_loop,
                &socket->io_handle,
                AWS_IO_EVENT_TYPE_WRITABLE | AWS_IO_EVENT_TYPE_READABLE,
                s_on_socket_io_event,
                socket)) {
            AWS_LOGF_ERROR(
                AWS_LS_IO_SOCKET,
                "id=%p fd=%d: assigning to event loop %p failed with error %d",
                (void *)socket,
                socket->io_handle.data.fd,
                (void *)event_loop,
                aws_last_error());
            socket_impl->currently_subscribed = false;
            socket->event_loop = NULL;
            return AWS_OP_ERR;
        }

        return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_IO_EVENT_LOOP_ALREADY_ASSIGNED);
}

struct aws_event_loop *aws_socket_get_event_loop(struct aws_socket *socket) {
    return socket->event_loop;
}

int aws_socket_subscribe_to_readable_events(
    struct aws_socket *socket,
    aws_socket_on_readable_fn *on_readable,
    void *user_data) {

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET, " id=%p fd=%d: subscribing to readable events", (void *)socket, socket->io_handle.data.fd);
    if (!(socket->state & CONNECTED_READ)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: can't subscribe to readable events since the socket is not connected",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_NOT_CONNECTED);
    }

    if (socket->readable_fn) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: can't subscribe to readable events since it is already subscribed",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_ERROR_IO_ALREADY_SUBSCRIBED);
    }

    AWS_ASSERT(on_readable);
    socket->readable_user_data = user_data;
    socket->readable_fn = on_readable;

    return AWS_OP_SUCCESS;
}

int aws_socket_read(struct aws_socket *socket, struct aws_byte_buf *buffer, size_t *amount_read) {
    AWS_ASSERT(amount_read);

    if (!aws_event_loop_thread_is_callers_thread(socket->event_loop)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: cannot read from a different thread than event loop %p",
            (void *)socket,
            socket->io_handle.data.fd,
            (void *)socket->event_loop);
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    if (!(socket->state & CONNECTED_READ)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: cannot read because it is not connected",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_NOT_CONNECTED);
    }

    ssize_t read_val = read(socket->io_handle.data.fd, buffer->buffer + buffer->len, buffer->capacity - buffer->len);
    int errno_value = errno; /* Always cache errno before potential side-effect */

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET, "id=%p fd=%d: read of %d", (void *)socket, socket->io_handle.data.fd, (int)read_val);

    if (read_val > 0) {
        *amount_read = (size_t)read_val;
        buffer->len += *amount_read;
        return AWS_OP_SUCCESS;
    }

    /* read_val of 0 means EOF which we'll treat as AWS_IO_SOCKET_CLOSED */
    if (read_val == 0) {
        AWS_LOGF_INFO(
            AWS_LS_IO_SOCKET, "id=%p fd=%d: zero read, socket is closed", (void *)socket, socket->io_handle.data.fd);
        *amount_read = 0;

        if (buffer->capacity - buffer->len > 0) {
            return aws_raise_error(AWS_IO_SOCKET_CLOSED);
        }

        return AWS_OP_SUCCESS;
    }

#if defined(EWOULDBLOCK)
    if (errno_value == EAGAIN || errno_value == EWOULDBLOCK) {
#else
    if (errno_value == EAGAIN) {
#endif
        AWS_LOGF_TRACE(AWS_LS_IO_SOCKET, "id=%p fd=%d: read would block", (void *)socket, socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_READ_WOULD_BLOCK);
    }

    if (errno_value == EPIPE || errno_value == ECONNRESET) {
        AWS_LOGF_INFO(AWS_LS_IO_SOCKET, "id=%p fd=%d: socket is closed.", (void *)socket, socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_CLOSED);
    }

    if (errno_value == ETIMEDOUT) {
        AWS_LOGF_ERROR(AWS_LS_IO_SOCKET, "id=%p fd=%d: socket timed out.", (void *)socket, socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_TIMEOUT);
    }

    AWS_LOGF_ERROR(
        AWS_LS_IO_SOCKET,
        "id=%p fd=%d: read failed with error: %s",
        (void *)socket,
        socket->io_handle.data.fd,
        strerror(errno_value));
    return aws_raise_error(s_determine_socket_error(errno_value));
}

int aws_socket_write(
    struct aws_socket *socket,
    const struct aws_byte_cursor *cursor,
    aws_socket_on_write_completed_fn *written_fn,
    void *user_data) {
    if (!aws_event_loop_thread_is_callers_thread(socket->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    if (!(socket->state & CONNECTED_WRITE)) {
        AWS_LOGF_ERROR(
            AWS_LS_IO_SOCKET,
            "id=%p fd=%d: cannot write to because it is not connected",
            (void *)socket,
            socket->io_handle.data.fd);
        return aws_raise_error(AWS_IO_SOCKET_NOT_CONNECTED);
    }

    AWS_ASSERT(written_fn);
    struct posix_socket *socket_impl = socket->impl;
    struct socket_write_request *write_request =
        aws_mem_calloc(socket->allocator, 1, sizeof(struct socket_write_request));

    if (!write_request) {
        return AWS_OP_ERR;
    }

    write_request->original_buffer_len = cursor->len;
    write_request->written_fn = written_fn;
    write_request->write_user_data = user_data;
    write_request->cursor_cpy = *cursor;
    aws_linked_list_push_back(&socket_impl->write_queue, &write_request->node);

    return s_process_socket_write_requests(socket, write_request);
}

int aws_socket_get_error(struct aws_socket *socket) {
    int connect_result;
    socklen_t result_length = sizeof(connect_result);

    if (getsockopt(socket->io_handle.data.fd, SOL_SOCKET, SO_ERROR, &connect_result, &result_length) < 0) {
        return s_determine_socket_error(errno);
    }

    if (connect_result) {
        return s_determine_socket_error(connect_result);
    }

    return AWS_OP_SUCCESS;
}

bool aws_socket_is_open(struct aws_socket *socket) {
    return socket->io_handle.data.fd >= 0;
}

void aws_socket_endpoint_init_local_address_for_test(struct aws_socket_endpoint *endpoint) {
    struct aws_uuid uuid;
    AWS_FATAL_ASSERT(aws_uuid_init(&uuid) == AWS_OP_SUCCESS);
    char uuid_str[AWS_UUID_STR_LEN] = {0};
    struct aws_byte_buf uuid_buf = aws_byte_buf_from_empty_array(uuid_str, sizeof(uuid_str));
    AWS_FATAL_ASSERT(aws_uuid_to_str(&uuid, &uuid_buf) == AWS_OP_SUCCESS);
    snprintf(endpoint->address, sizeof(endpoint->address), "testsock" PRInSTR ".sock", AWS_BYTE_BUF_PRI(uuid_buf));
}

bool aws_is_network_interface_name_valid(const char *interface_name) {
    if (if_nametoindex(interface_name) == 0) {
        AWS_LOGF_ERROR(AWS_LS_IO_SOCKET, "network_interface_name(%s) is invalid with errno: %d", interface_name, errno);
        return false;
    }
    return true;
}
