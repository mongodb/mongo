/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/io/socket_channel_handler.h>

#include <aws/common/error.h>
#include <aws/common/task_scheduler.h>

#include <aws/io/event_loop.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/statistics.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

struct socket_handler {
    struct aws_socket *socket;
    struct aws_channel_slot *slot;
    size_t max_rw_size;
    struct aws_channel_task read_task_storage;
    struct aws_channel_task shutdown_task_storage;
    struct aws_crt_statistics_socket stats;
    int shutdown_err_code;
    bool shutdown_in_progress;
};

static int s_socket_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)handler;
    (void)slot;
    (void)message;

    AWS_LOGF_FATAL(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: process_read_message called on "
        "socket handler. This should never happen",
        (void *)handler);

    /*since a socket handler will ALWAYS be the first handler in a channel,
     * this should NEVER happen, if it does it's a programmer error.*/
    AWS_ASSERT(0);
    return aws_raise_error(AWS_IO_CHANNEL_ERROR_ERROR_CANT_ACCEPT_INPUT);
}

/* invoked by the socket when a write has completed or failed. */
static void s_on_socket_write_complete(
    struct aws_socket *socket,
    int error_code,
    size_t amount_written,
    void *user_data) {

    if (user_data) {
        struct aws_io_message *message = user_data;
        struct aws_channel *channel = message->owning_channel;
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET_HANDLER,
            "static: write of size %llu, completed on channel %p",
            (unsigned long long)amount_written,
            (void *)channel);

        if (message->on_completion) {
            message->on_completion(channel, message, error_code, message->user_data);
        }

        if (socket && socket->handler) {
            struct socket_handler *socket_handler = socket->handler->impl;
            socket_handler->stats.bytes_written += amount_written;
        }

        aws_mem_release(message->allocator, message);

        if (error_code) {
            aws_channel_shutdown(channel, error_code);
        }
    }
}

static int s_socket_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)slot;
    struct socket_handler *socket_handler = handler->impl;

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: writing message of size %llu",
        (void *)handler,
        (unsigned long long)message->message_data.len);

    if (!aws_socket_is_open(socket_handler->socket)) {
        return aws_raise_error(AWS_IO_SOCKET_CLOSED);
    }

    struct aws_byte_cursor cursor = aws_byte_cursor_from_buf(&message->message_data);
    if (aws_socket_write(socket_handler->socket, &cursor, s_on_socket_write_complete, message)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_read_task(struct aws_channel_task *task, void *arg, aws_task_status status);

static void s_on_readable_notification(struct aws_socket *socket, int error_code, void *user_data);

/* Ok this next function is VERY important for how back pressure works. Here's what it's supposed to be doing:
 *
 * See how much data downstream is willing to accept.
 * See how much we're actually willing to read per event loop tick (usually 16 kb).
 * Take the minimum of those two.
 * Try and read as much as possible up to the calculated max read.
 * If we didn't read up to the max_read, we go back to waiting on the event loop to tell us we can read more.
 * If we did read up to the max_read, we stop reading immediately and wait for either for a window update,
 * or schedule a task to enforce fairness for other sockets in the event loop if we read up to the max
 * read per event loop tick.
 */
static void s_do_read(struct socket_handler *socket_handler) {

    if (socket_handler->shutdown_in_progress) {
        return;
    }

    size_t downstream_window = aws_channel_slot_downstream_read_window(socket_handler->slot);
    size_t max_to_read =
        downstream_window > socket_handler->max_rw_size ? socket_handler->max_rw_size : downstream_window;

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: invoking read. Downstream window %llu, max_to_read %llu",
        (void *)socket_handler->slot->handler,
        (unsigned long long)downstream_window,
        (unsigned long long)max_to_read);

    if (max_to_read == 0) {
        return;
    }

    size_t total_read = 0;
    size_t read = 0;
    int last_error = 0;
    while (total_read < max_to_read) {
        size_t iter_max_read = max_to_read - total_read;

        struct aws_io_message *message = aws_channel_acquire_message_from_pool(
            socket_handler->slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, iter_max_read);

        if (aws_socket_read(socket_handler->socket, &message->message_data, &read)) {
            last_error = aws_last_error();
            aws_mem_release(message->allocator, message);
            break;
        }

        total_read += read;
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET_HANDLER,
            "id=%p: read %llu from socket",
            (void *)socket_handler->slot->handler,
            (unsigned long long)read);

        if (aws_channel_slot_send_message(socket_handler->slot, message, AWS_CHANNEL_DIR_READ)) {
            last_error = aws_last_error();
            aws_mem_release(message->allocator, message);
            break;
        }
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: total read on this tick %llu",
        (void *)socket_handler->slot->handler,
        (unsigned long long)total_read);

    socket_handler->stats.bytes_read += total_read;

    /* resubscribe as long as there's no error, just return if we're in a would block scenario. */
    if (total_read < max_to_read) {
        AWS_ASSERT(last_error != 0);

        if (last_error != AWS_IO_READ_WOULD_BLOCK) {
            aws_channel_shutdown(socket_handler->slot->channel, last_error);
        } else {
            AWS_LOGF_TRACE(
                AWS_LS_IO_SOCKET_HANDLER,
                "id=%p: out of data to read on socket. "
                "Waiting on event-loop notification.",
                (void *)socket_handler->slot->handler);
        }
        return;
    }
    /* in this case, everything was fine, but there's still pending reads. We need to schedule a task to do the read
     * again. */
    if (total_read == socket_handler->max_rw_size && !socket_handler->read_task_storage.task_fn) {

        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET_HANDLER,
            "id=%p: more data is pending read, but we've exceeded "
            "the max read on this tick. Scheduling a task to read on next tick.",
            (void *)socket_handler->slot->handler);
        aws_channel_task_init(
            &socket_handler->read_task_storage, s_read_task, socket_handler, "socket_handler_re_read");
        aws_channel_schedule_task_now(socket_handler->slot->channel, &socket_handler->read_task_storage);
    }
}

/* the socket is either readable or errored out. If it's readable, kick off s_do_read() to do its thing. */
static void s_on_readable_notification(struct aws_socket *socket, int error_code, void *user_data) {
    (void)socket;

    struct socket_handler *socket_handler = user_data;
    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: socket on-readable with error code %d(%s)",
        (void *)socket_handler->slot->handler,
        error_code,
        aws_error_name(error_code));

    /* Regardless of error code call read() until it reports error or EOF,
     * so we can pick up data that was sent prior to the close.
     *
     * For example, if peer closes the socket immediately after sending the last
     * bytes of data, the READABLE and HANGUP events arrive simultaneously.
     *
     * Another example, peer sends a TLS ALERT then immediately closes the socket.
     * On some platforms, we'll never see the readable flag. So we want to make
     * sure we read the ALERT, otherwise, we'll end up telling the user that the channel shutdown because of a socket
     * closure, when in reality it was a TLS error
     *
     * It may take more than one read() to get all remaining data.
     * Also, if the downstream read-window reaches 0, we need to patiently
     * wait until the window opens before we can call read() again. */
    (void)error_code;
    s_do_read(socket_handler);
}

/* Either the result of a context switch (for fairness in the event loop), or a window update. */
static void s_read_task(struct aws_channel_task *task, void *arg, aws_task_status status) {
    task->task_fn = NULL;
    task->arg = NULL;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        struct socket_handler *socket_handler = arg;
        s_do_read(socket_handler);
    }
}

static int s_socket_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {
    (void)size;

    struct socket_handler *socket_handler = handler->impl;

    if (!socket_handler->shutdown_in_progress && !socket_handler->read_task_storage.task_fn) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET_HANDLER,
            "id=%p: increment read window message received, scheduling"
            " task for another read operation.",
            (void *)handler);

        aws_channel_task_init(
            &socket_handler->read_task_storage, s_read_task, socket_handler, "socket_handler_read_on_window_increment");
        aws_channel_schedule_task_now(slot->channel, &socket_handler->read_task_storage);
    }

    return AWS_OP_SUCCESS;
}

static void s_close_task(struct aws_channel_task *task, void *arg, aws_task_status status) {
    (void)task;
    (void)status;

    struct aws_channel_handler *handler = arg;
    struct socket_handler *socket_handler = handler->impl;

    /*
     * Run this unconditionally regardless of status, otherwise channel will not
     * finish shutting down properly
     */

    /* this only happens in write direction. */
    /* we also don't care about the free_scarce_resource_immediately
     * code since we're always the last one in the shutdown sequence. */
    aws_channel_slot_on_handler_shutdown_complete(
        socket_handler->slot, AWS_CHANNEL_DIR_WRITE, socket_handler->shutdown_err_code, false);
}

static int s_socket_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resource_immediately) {
    struct socket_handler *socket_handler = (struct socket_handler *)handler->impl;

    socket_handler->shutdown_in_progress = true;
    if (dir == AWS_CHANNEL_DIR_READ) {
        AWS_LOGF_TRACE(
            AWS_LS_IO_SOCKET_HANDLER,
            "id=%p: shutting down read direction with error_code %d",
            (void *)handler,
            error_code);
        if (free_scarce_resource_immediately && aws_socket_is_open(socket_handler->socket)) {
            if (aws_socket_close(socket_handler->socket)) {
                return AWS_OP_ERR;
            }
        }

        return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resource_immediately);
    }

    AWS_LOGF_TRACE(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: shutting down write direction with error_code %d",
        (void *)handler,
        error_code);
    if (aws_socket_is_open(socket_handler->socket)) {
        aws_socket_close(socket_handler->socket);
    }

    /* Schedule a task to complete the shutdown, in case a do_read task is currently pending.
     * It's OK to delay the shutdown, even when free_scarce_resources_immediately is true,
     * because the socket has been closed: mitigating the risk that the socket is still being abused by
     * a hostile peer. */
    aws_channel_task_init(&socket_handler->shutdown_task_storage, s_close_task, handler, "socket_handler_close");
    socket_handler->shutdown_err_code = error_code;
    aws_channel_schedule_task_now(slot->channel, &socket_handler->shutdown_task_storage);
    return AWS_OP_SUCCESS;
}

static size_t s_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return 0;
}

static size_t s_socket_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;
    return SIZE_MAX;
}

static void s_socket_destroy(struct aws_channel_handler *handler) {
    if (handler != NULL) {
        struct socket_handler *socket_handler = (struct socket_handler *)handler->impl;
        if (socket_handler != NULL) {
            aws_crt_statistics_socket_cleanup(&socket_handler->stats);
        }

        aws_mem_release(handler->alloc, handler);
    }
}

static void s_reset_statistics(struct aws_channel_handler *handler) {
    struct socket_handler *socket_handler = (struct socket_handler *)handler->impl;

    aws_crt_statistics_socket_reset(&socket_handler->stats);
}

static void s_gather_statistics(struct aws_channel_handler *handler, struct aws_array_list *stats_list) {
    struct socket_handler *socket_handler = (struct socket_handler *)handler->impl;

    void *stats_base = &socket_handler->stats;
    aws_array_list_push_back(stats_list, &stats_base);
}

static void s_trigger_read(struct aws_channel_handler *handler) {
    struct socket_handler *socket_handler = (struct socket_handler *)handler->impl;

    s_do_read(socket_handler);
}

static struct aws_channel_handler_vtable s_vtable = {
    .process_read_message = s_socket_process_read_message,
    .destroy = s_socket_destroy,
    .process_write_message = s_socket_process_write_message,
    .initial_window_size = s_socket_initial_window_size,
    .increment_read_window = s_socket_increment_read_window,
    .shutdown = s_socket_shutdown,
    .message_overhead = s_message_overhead,
    .reset_statistics = s_reset_statistics,
    .gather_statistics = s_gather_statistics,
    .trigger_read = s_trigger_read,
};

struct aws_channel_handler *aws_socket_handler_new(
    struct aws_allocator *allocator,
    struct aws_socket *socket,
    struct aws_channel_slot *slot,
    size_t max_read_size) {

    /* make sure something has assigned this socket to an event loop, in client mode this will already have occurred.
       In server mode, someone should have assigned it before calling us.*/
    AWS_ASSERT(aws_socket_get_event_loop(socket));

    struct aws_channel_handler *handler = NULL;

    struct socket_handler *impl = NULL;

    if (!aws_mem_acquire_many(
            allocator, 2, &handler, sizeof(struct aws_channel_handler), &impl, sizeof(struct socket_handler))) {
        return NULL;
    }

    impl->socket = socket;
    impl->slot = slot;
    impl->max_rw_size = max_read_size;
    AWS_ZERO_STRUCT(impl->read_task_storage);
    AWS_ZERO_STRUCT(impl->shutdown_task_storage);
    impl->shutdown_in_progress = false;
    if (aws_crt_statistics_socket_init(&impl->stats)) {
        goto cleanup_handler;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_IO_SOCKET_HANDLER,
        "id=%p: Socket handler created with max_read_size of %llu",
        (void *)handler,
        (unsigned long long)max_read_size);

    handler->alloc = allocator;
    handler->impl = impl;
    handler->vtable = &s_vtable;
    handler->slot = slot;
    if (aws_socket_subscribe_to_readable_events(socket, s_on_readable_notification, impl)) {
        goto cleanup_handler;
    }

    socket->handler = handler;

    return handler;

cleanup_handler:
    aws_mem_release(allocator, handler);

    return NULL;
}

const struct aws_socket *aws_socket_handler_get_socket(const struct aws_channel_handler *handler) {
    AWS_PRECONDITION(handler);
    const struct socket_handler *socket_handler = handler->impl;
    return socket_handler->socket;
}
