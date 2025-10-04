/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/pipe.h>

#include <aws/io/event_loop.h>
#include <aws/io/private/event_loop_impl.h>

#ifdef __GLIBC__
#    define __USE_GNU
#endif

/* TODO: move this detection to CMAKE and a config header */
#if !defined(COMPAT_MODE) && defined(__GLIBC__) && ((__GLIBC__ == 2 && __GLIBC_MINOR__ >= 9) || __GLIBC__ > 2)
#    define HAVE_PIPE2 1
#else
#    define HAVE_PIPE2 0
#endif

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* This isn't defined on ancient linux distros (breaking the builds).
 * However, if this is a prebuild, we purposely build on an ancient system, but
 * we want the kernel calls to still be the same as a modern build since that's likely the target of the application
 * calling this code. Just define this if it isn't there already. GlibC and the kernel don't really care how the flag
 * gets passed as long as it does.
 */
#ifndef O_CLOEXEC
#    define O_CLOEXEC 02000000
#endif

struct read_end_impl {
    struct aws_allocator *alloc;
    struct aws_io_handle handle;
    struct aws_event_loop *event_loop;
    aws_pipe_on_readable_fn *on_readable_user_callback;
    void *on_readable_user_data;

    /* Used in handshake for detecting whether user callback resulted in read-end being cleaned up.
     * If clean_up() sees that the pointer is set, the bool it points to will get set true. */
    bool *did_user_callback_clean_up_read_end;

    bool is_subscribed;
};

struct pipe_write_request {
    struct aws_byte_cursor original_cursor;
    struct aws_byte_cursor cursor; /* tracks progress of write */
    size_t num_bytes_written;
    aws_pipe_on_write_completed_fn *user_callback;
    void *user_data;
    struct aws_linked_list_node list_node;

    /* True if the write-end is cleaned up while the user callback is being invoked */
    bool did_user_callback_clean_up_write_end;
};

struct write_end_impl {
    struct aws_allocator *alloc;
    struct aws_io_handle handle;
    struct aws_event_loop *event_loop;
    struct aws_linked_list write_list;

    /* Valid while invoking user callback on a completed write request. */
    struct pipe_write_request *currently_invoking_write_callback;

    bool is_writable;

    /* Future optimization idea: avoid an allocation on each write by keeping 1 pre-allocated pipe_write_request around
     * and re-using it whenever possible */
};

static void s_write_end_on_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data);

static int s_translate_posix_error(int err) {
    AWS_ASSERT(err);

    switch (err) {
        case EPIPE:
            return AWS_IO_BROKEN_PIPE;
        default:
            return AWS_ERROR_SYS_CALL_FAILURE;
    }
}

static int s_raise_posix_error(int err) {
    return aws_raise_error(s_translate_posix_error(err));
}

AWS_IO_API int aws_open_nonblocking_posix_pipe(int pipe_fds[2]) {
    int err;

#if HAVE_PIPE2
    err = pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC);
    if (err) {
        return s_raise_posix_error(err);
    }

    return AWS_OP_SUCCESS;
#else
    err = pipe(pipe_fds);
    if (err) {
        return s_raise_posix_error(err);
    }

    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(pipe_fds[i], F_GETFL);
        if (flags == -1) {
            s_raise_posix_error(err);
            goto error;
        }

        flags |= O_NONBLOCK | O_CLOEXEC;
        if (fcntl(pipe_fds[i], F_SETFL, flags) == -1) {
            s_raise_posix_error(err);
            goto error;
        }
    }

    return AWS_OP_SUCCESS;
error:
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return AWS_OP_ERR;
#endif
}

int aws_pipe_init(
    struct aws_pipe_read_end *read_end,
    struct aws_event_loop *read_end_event_loop,
    struct aws_pipe_write_end *write_end,
    struct aws_event_loop *write_end_event_loop,
    struct aws_allocator *allocator) {

    AWS_ASSERT(read_end);
    AWS_ASSERT(read_end_event_loop);
    AWS_ASSERT(write_end);
    AWS_ASSERT(write_end_event_loop);
    AWS_ASSERT(allocator);

    AWS_ZERO_STRUCT(*read_end);
    AWS_ZERO_STRUCT(*write_end);

    struct read_end_impl *read_impl = NULL;
    struct write_end_impl *write_impl = NULL;
    int err;

    /* Open pipe */
    int pipe_fds[2];
    err = aws_open_nonblocking_posix_pipe(pipe_fds);
    if (err) {
        return AWS_OP_ERR;
    }

    /* Init read-end */
    read_impl = aws_mem_calloc(allocator, 1, sizeof(struct read_end_impl));
    if (!read_impl) {
        goto error;
    }

    read_impl->alloc = allocator;
    read_impl->handle.data.fd = pipe_fds[0];
    read_impl->event_loop = read_end_event_loop;

    /* Init write-end */
    write_impl = aws_mem_calloc(allocator, 1, sizeof(struct write_end_impl));
    if (!write_impl) {
        goto error;
    }

    write_impl->alloc = allocator;
    write_impl->handle.data.fd = pipe_fds[1];
    write_impl->event_loop = write_end_event_loop;
    write_impl->is_writable = true; /* Assume pipe is writable to start. Even if it's not, things shouldn't break */
    aws_linked_list_init(&write_impl->write_list);

    read_end->impl_data = read_impl;
    write_end->impl_data = write_impl;

    err = aws_event_loop_subscribe_to_io_events(
        write_end_event_loop, &write_impl->handle, AWS_IO_EVENT_TYPE_WRITABLE, s_write_end_on_event, write_end);
    if (err) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    if (read_impl) {
        aws_mem_release(allocator, read_impl);
    }

    if (write_impl) {
        aws_mem_release(allocator, write_impl);
    }

    read_end->impl_data = NULL;
    write_end->impl_data = NULL;

    return AWS_OP_ERR;
}

int aws_pipe_clean_up_read_end(struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    if (!read_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    if (read_impl->is_subscribed) {
        int err = aws_pipe_unsubscribe_from_readable_events(read_end);
        if (err) {
            return AWS_OP_ERR;
        }
    }

    /* If the event-handler is invoking a user callback, let it know that the read-end was cleaned up */
    if (read_impl->did_user_callback_clean_up_read_end) {
        *read_impl->did_user_callback_clean_up_read_end = true;
    }

    close(read_impl->handle.data.fd);

    aws_mem_release(read_impl->alloc, read_impl);
    AWS_ZERO_STRUCT(*read_end);
    return AWS_OP_SUCCESS;
}

struct aws_event_loop *aws_pipe_get_read_end_event_loop(const struct aws_pipe_read_end *read_end) {
    const struct read_end_impl *read_impl = read_end->impl_data;
    if (!read_impl) {
        aws_raise_error(AWS_IO_BROKEN_PIPE);
        return NULL;
    }

    return read_impl->event_loop;
}

struct aws_event_loop *aws_pipe_get_write_end_event_loop(const struct aws_pipe_write_end *write_end) {
    const struct write_end_impl *write_impl = write_end->impl_data;
    if (!write_impl) {
        aws_raise_error(AWS_IO_BROKEN_PIPE);
        return NULL;
    }

    return write_impl->event_loop;
}

int aws_pipe_read(struct aws_pipe_read_end *read_end, struct aws_byte_buf *dst_buffer, size_t *num_bytes_read) {
    AWS_ASSERT(dst_buffer && dst_buffer->buffer);

    struct read_end_impl *read_impl = read_end->impl_data;
    if (!read_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (num_bytes_read) {
        *num_bytes_read = 0;
    }

    size_t num_bytes_to_read = dst_buffer->capacity - dst_buffer->len;

    ssize_t read_val = read(read_impl->handle.data.fd, dst_buffer->buffer + dst_buffer->len, num_bytes_to_read);

    if (read_val < 0) {
        int errno_value = errno; /* Always cache errno before potential side-effect */
        if (errno_value == EAGAIN || errno_value == EWOULDBLOCK) {
            return aws_raise_error(AWS_IO_READ_WOULD_BLOCK);
        }
        return s_raise_posix_error(errno_value);
    }

    /* Success */
    dst_buffer->len += read_val;

    if (num_bytes_read) {
        *num_bytes_read = read_val;
    }

    return AWS_OP_SUCCESS;
}

static void s_read_end_on_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {

    (void)event_loop;
    (void)handle;

    /* Note that it should be impossible for this to run after read-end has been unsubscribed or cleaned up */
    struct aws_pipe_read_end *read_end = user_data;
    struct read_end_impl *read_impl = read_end->impl_data;
    AWS_ASSERT(read_impl);
    AWS_ASSERT(read_impl->event_loop == event_loop);
    AWS_ASSERT(&read_impl->handle == handle);
    AWS_ASSERT(read_impl->is_subscribed);
    AWS_ASSERT(events != 0);
    AWS_ASSERT(read_impl->did_user_callback_clean_up_read_end == NULL);

    /* Set up handshake, so we can be informed if the read-end is cleaned up while invoking a user callback */
    bool did_user_callback_clean_up_read_end = false;
    read_impl->did_user_callback_clean_up_read_end = &did_user_callback_clean_up_read_end;

    /* If readable event received, tell user to try and read, even if "error" events have also occurred. */
    if (events & AWS_IO_EVENT_TYPE_READABLE) {
        read_impl->on_readable_user_callback(read_end, AWS_ERROR_SUCCESS, read_impl->on_readable_user_data);

        if (did_user_callback_clean_up_read_end) {
            return;
        }

        events &= ~AWS_IO_EVENT_TYPE_READABLE;
    }

    if (events) {
        /* Check that user didn't unsubscribe in the previous callback */
        if (read_impl->is_subscribed) {
            read_impl->on_readable_user_callback(read_end, AWS_IO_BROKEN_PIPE, read_impl->on_readable_user_data);

            if (did_user_callback_clean_up_read_end) {
                return;
            }
        }
    }

    read_impl->did_user_callback_clean_up_read_end = NULL;
}

int aws_pipe_subscribe_to_readable_events(
    struct aws_pipe_read_end *read_end,
    aws_pipe_on_readable_fn *on_readable,
    void *user_data) {

    AWS_ASSERT(on_readable);

    struct read_end_impl *read_impl = read_end->impl_data;
    if (!read_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    if (read_impl->is_subscribed) {
        return aws_raise_error(AWS_ERROR_IO_ALREADY_SUBSCRIBED);
    }

    read_impl->is_subscribed = true;
    read_impl->on_readable_user_callback = on_readable;
    read_impl->on_readable_user_data = user_data;

    int err = aws_event_loop_subscribe_to_io_events(
        read_impl->event_loop, &read_impl->handle, AWS_IO_EVENT_TYPE_READABLE, s_read_end_on_event, read_end);
    if (err) {
        read_impl->is_subscribed = false;
        read_impl->on_readable_user_callback = NULL;
        read_impl->on_readable_user_data = NULL;

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

int aws_pipe_unsubscribe_from_readable_events(struct aws_pipe_read_end *read_end) {
    struct read_end_impl *read_impl = read_end->impl_data;
    if (!read_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (!aws_event_loop_thread_is_callers_thread(read_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    if (!read_impl->is_subscribed) {
        return aws_raise_error(AWS_ERROR_IO_NOT_SUBSCRIBED);
    }

    int err = aws_event_loop_unsubscribe_from_io_events(read_impl->event_loop, &read_impl->handle);
    if (err) {
        return AWS_OP_ERR;
    }

    read_impl->is_subscribed = false;
    read_impl->on_readable_user_callback = NULL;
    read_impl->on_readable_user_data = NULL;

    return AWS_OP_SUCCESS;
}

/* Pop front write request, invoke its callback, and delete it.
 * Returns whether the callback resulted in the write-end getting cleaned up */
static bool s_write_end_complete_front_write_request(struct aws_pipe_write_end *write_end, int error_code) {
    struct write_end_impl *write_impl = write_end->impl_data;

    AWS_ASSERT(!aws_linked_list_empty(&write_impl->write_list));
    struct aws_linked_list_node *node = aws_linked_list_pop_front(&write_impl->write_list);
    struct pipe_write_request *request = AWS_CONTAINER_OF(node, struct pipe_write_request, list_node);

    struct aws_allocator *alloc = write_impl->alloc;

    /* Let the write-end know that a callback is in process, so the write-end can inform the callback
     * whether it resulted in clean_up() being called. */
    bool write_end_cleaned_up_during_callback = false;
    struct pipe_write_request *prev_invoking_request = write_impl->currently_invoking_write_callback;
    write_impl->currently_invoking_write_callback = request;

    if (request->user_callback) {
        request->user_callback(write_end, error_code, request->original_cursor, request->user_data);
        write_end_cleaned_up_during_callback = request->did_user_callback_clean_up_write_end;
    }

    if (!write_end_cleaned_up_during_callback) {
        write_impl->currently_invoking_write_callback = prev_invoking_request;
    }

    aws_mem_release(alloc, request);

    return write_end_cleaned_up_during_callback;
}

/* Process write requests as long as the pipe remains writable */
static void s_write_end_process_requests(struct aws_pipe_write_end *write_end) {
    struct write_end_impl *write_impl = write_end->impl_data;
    AWS_ASSERT(write_impl);

    while (!aws_linked_list_empty(&write_impl->write_list)) {
        struct aws_linked_list_node *node = aws_linked_list_front(&write_impl->write_list);
        struct pipe_write_request *request = AWS_CONTAINER_OF(node, struct pipe_write_request, list_node);

        int completed_error_code = AWS_ERROR_SUCCESS;

        if (request->cursor.len > 0) {
            ssize_t write_val = write(write_impl->handle.data.fd, request->cursor.ptr, request->cursor.len);

            if (write_val < 0) {
                int errno_value = errno; /* Always cache errno before potential side-effect */
                if (errno_value == EAGAIN || errno_value == EWOULDBLOCK) {
                    /* The pipe is no longer writable. Bail out */
                    write_impl->is_writable = false;
                    return;
                }

                /* A non-recoverable error occurred during this write */
                completed_error_code = s_translate_posix_error(errno_value);

            } else {
                aws_byte_cursor_advance(&request->cursor, write_val);

                if (request->cursor.len > 0) {
                    /* There was a partial write, loop again to try and write the rest. */
                    continue;
                }
            }
        }

        /* If we got this far in the loop, then the write request is complete.
         * Note that the callback may result in the pipe being cleaned up. */
        bool write_end_cleaned_up = s_write_end_complete_front_write_request(write_end, completed_error_code);
        if (write_end_cleaned_up) {
            /* Bail out! Any remaining requests were canceled during clean_up() */
            return;
        }
    }
}

/* Handle events on the write-end's file handle */
static void s_write_end_on_event(
    struct aws_event_loop *event_loop,
    struct aws_io_handle *handle,
    int events,
    void *user_data) {

    (void)event_loop;
    (void)handle;

    /* Note that it should be impossible for this to run after write-end has been unsubscribed or cleaned up */
    struct aws_pipe_write_end *write_end = user_data;
    struct write_end_impl *write_impl = write_end->impl_data;
    AWS_ASSERT(write_impl);
    AWS_ASSERT(write_impl->event_loop == event_loop);
    AWS_ASSERT(&write_impl->handle == handle);

    /* Only care about the writable event. */
    if ((events & AWS_IO_EVENT_TYPE_WRITABLE) == 0) {
        return;
    }

    write_impl->is_writable = true;

    s_write_end_process_requests(write_end);
}

int aws_pipe_write(
    struct aws_pipe_write_end *write_end,
    struct aws_byte_cursor src_buffer,
    aws_pipe_on_write_completed_fn *on_completed,
    void *user_data) {

    AWS_ASSERT(src_buffer.ptr);

    struct write_end_impl *write_impl = write_end->impl_data;
    if (!write_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (!aws_event_loop_thread_is_callers_thread(write_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    struct pipe_write_request *request = aws_mem_calloc(write_impl->alloc, 1, sizeof(struct pipe_write_request));
    if (!request) {
        return AWS_OP_ERR;
    }

    request->original_cursor = src_buffer;
    request->cursor = src_buffer;
    request->user_callback = on_completed;
    request->user_data = user_data;

    aws_linked_list_push_back(&write_impl->write_list, &request->list_node);

    /* If the pipe is writable, process the request (unless pipe is already in the middle of processing, which could
     * happen if a this aws_pipe_write() call was made by another write's completion callback */
    if (write_impl->is_writable && !write_impl->currently_invoking_write_callback) {
        s_write_end_process_requests(write_end);
    }

    return AWS_OP_SUCCESS;
}

int aws_pipe_clean_up_write_end(struct aws_pipe_write_end *write_end) {
    struct write_end_impl *write_impl = write_end->impl_data;
    if (!write_impl) {
        return aws_raise_error(AWS_IO_BROKEN_PIPE);
    }

    if (!aws_event_loop_thread_is_callers_thread(write_impl->event_loop)) {
        return aws_raise_error(AWS_ERROR_IO_EVENT_LOOP_THREAD_ONLY);
    }

    int err = aws_event_loop_unsubscribe_from_io_events(write_impl->event_loop, &write_impl->handle);
    if (err) {
        return AWS_OP_ERR;
    }

    close(write_impl->handle.data.fd);

    /* Zero out write-end before invoking user callbacks so that it won't work anymore with public functions. */
    AWS_ZERO_STRUCT(*write_end);

    /* If a request callback is currently being invoked, let it know that the write-end was cleaned up */
    if (write_impl->currently_invoking_write_callback) {
        write_impl->currently_invoking_write_callback->did_user_callback_clean_up_write_end = true;
    }

    /* Force any outstanding write requests to complete with an error status. */
    while (!aws_linked_list_empty(&write_impl->write_list)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&write_impl->write_list);
        struct pipe_write_request *request = AWS_CONTAINER_OF(node, struct pipe_write_request, list_node);
        if (request->user_callback) {
            request->user_callback(NULL, AWS_IO_BROKEN_PIPE, request->original_cursor, request->user_data);
        }
        aws_mem_release(write_impl->alloc, request);
    }

    aws_mem_release(write_impl->alloc, write_impl);
    return AWS_OP_SUCCESS;
}
