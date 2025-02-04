#ifndef AWS_IO_PIPE_H
#define AWS_IO_PIPE_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_event_loop;

struct aws_pipe_read_end {
    void *impl_data;
};

struct aws_pipe_write_end {
    void *impl_data;
};

/**
 * Callback for when the pipe is readable (edge-triggered), or an error has occurred.
 * Afer subscribing, the callback is invoked when the pipe has data to read, or the pipe has an error.
 * The readable callback is invoked again any time the user reads all data, and then more data arrives.
 * Note that it will not be invoked again if the pipe still has unread data when more data arrives.
 * `error_code` of AWS_ERROR_SUCCESS indicates a readable event, and otherwise contains the value of the error.
 * `user_data` corresponds to the `user_data` passed into aws_pipe_subscribe_to_read_events().
 * This callback is always invoked on the read-end's event-loop thread.
 */
typedef void(aws_pipe_on_readable_fn)(struct aws_pipe_read_end *read_end, int error_code, void *user_data);

/**
 * Callback for when the asynchronous aws_pipe_write() operation has either completed or failed.
 * `write_end` will be NULL if this callback is invoked after the the write-end has been cleaned up,
 * this does not necessarily mean that the write operation failed.
 * `error_code` will be AWS_ERROR_SUCCESS if all data was written, or a code corresponding to the error.
 * `src_buffer` corresponds to the buffer passed into aws_pipe_write()
 * `user_data` corresponds to the `user_data` passed into aws_pipe_write().
 * This callback is always invoked on the write-end's event-loop thread.
 */
typedef void(aws_pipe_on_write_completed_fn)(
    struct aws_pipe_write_end *write_end,
    int error_code,
    struct aws_byte_cursor src_buffer,
    void *user_data);

AWS_EXTERN_C_BEGIN

/**
 * Opens an OS specific bidirectional pipe.
 * The read direction is stored in read_end. Write direction is stored in write_end.
 * Each end must be connected to an event-loop, and further calls to each end must happen on that event-loop's thread.
 */
AWS_IO_API
int aws_pipe_init(
    struct aws_pipe_read_end *read_end,
    struct aws_event_loop *read_end_event_loop,
    struct aws_pipe_write_end *write_end,
    struct aws_event_loop *write_end_event_loop,
    struct aws_allocator *allocator);

/**
 * Clean up the read-end of the pipe.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_clean_up_read_end(struct aws_pipe_read_end *read_end);

/**
 * Clean up the write-end of the pipe.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_clean_up_write_end(struct aws_pipe_write_end *write_end);

/**
 * Get the event-loop connected to the read-end of the pipe.
 * This may be called on any thread.
 */
AWS_IO_API
struct aws_event_loop *aws_pipe_get_read_end_event_loop(const struct aws_pipe_read_end *read_end);

/**
 * Get the event-loop connected to the write-end of the pipe.
 * This may be called on any thread.
 */
AWS_IO_API
struct aws_event_loop *aws_pipe_get_write_end_event_loop(const struct aws_pipe_write_end *write_end);

/**
 * Initiates an asynchrous write from the source buffer to the pipe.
 * The data referenced by `src_buffer` must remain in memory until the operation completes.
 * `on_complete` is called on the event-loop thread when the operation has either completed or failed.
 * The callback's pipe argument will be NULL if the callback is invoked after the pipe has been cleaned up.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_write(
    struct aws_pipe_write_end *write_end,
    struct aws_byte_cursor src_buffer,
    aws_pipe_on_write_completed_fn *on_completed,
    void *user_data);

/**
 * Read data from the pipe into the destination buffer.
 * Attempts to read enough to fill all remaining space in the buffer, from `dst_buffer->len` to `dst_buffer->capacity`.
 * `dst_buffer->len` is updated to reflect the buffer's new length.
 * `num_bytes_read` (optional) is set to the total number of bytes read.
 * This function never blocks. If no bytes could be read without blocking, then AWS_OP_ERR is returned and
 * aws_last_error() code will be AWS_IO_READ_WOULD_BLOCK.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_read(struct aws_pipe_read_end *read_end, struct aws_byte_buf *dst_buffer, size_t *num_bytes_read);

/**
 * Subscribe to be notified when the pipe becomes readable (edge-triggered), or an error occurs.
 * `on_readable` is invoked on the event-loop's thread when the pipe has data to read, or the pipe has an error.
 * `on_readable` is invoked again any time the user reads all data, and then more data arrives.
 * Note that it will not be invoked again if the pipe still has unread data when more data arrives.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_subscribe_to_readable_events(
    struct aws_pipe_read_end *read_end,
    aws_pipe_on_readable_fn *on_readable,
    void *user_data);

/**
 * Stop receiving notifications about events on the read-end of the pipe.
 * This must be called on the thread of the connected event-loop.
 */
AWS_IO_API
int aws_pipe_unsubscribe_from_readable_events(struct aws_pipe_read_end *read_end);

#if defined(_WIN32)
/**
 * Generate a unique pipe name.
 * The suggested dst_size is 256.
 */
AWS_IO_API
int aws_pipe_get_unique_name(char *dst, size_t dst_size);
#endif

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_PIPE_H */
