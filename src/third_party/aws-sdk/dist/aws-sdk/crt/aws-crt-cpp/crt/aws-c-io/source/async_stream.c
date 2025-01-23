/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/async_stream.h>

#include <aws/common/byte_buf.h>
#include <aws/io/future.h>
#include <aws/io/stream.h>

void aws_async_input_stream_init_base(
    struct aws_async_input_stream *stream,
    struct aws_allocator *alloc,
    const struct aws_async_input_stream_vtable *vtable,
    void *impl) {

    AWS_PRECONDITION(stream);
    AWS_PRECONDITION(alloc);
    AWS_PRECONDITION(vtable);
    AWS_PRECONDITION(vtable->read);
    AWS_PRECONDITION(vtable->destroy);

    AWS_ZERO_STRUCT(*stream);
    stream->alloc = alloc;
    stream->vtable = vtable;
    stream->impl = impl;
    aws_ref_count_init(&stream->ref_count, stream, (aws_simple_completion_callback *)vtable->destroy);
}

struct aws_async_input_stream *aws_async_input_stream_acquire(struct aws_async_input_stream *stream) {
    if (stream != NULL) {
        aws_ref_count_acquire(&stream->ref_count);
    }
    return stream;
}

struct aws_async_input_stream *aws_async_input_stream_release(struct aws_async_input_stream *stream) {
    if (stream) {
        aws_ref_count_release(&stream->ref_count);
    }
    return NULL;
}

struct aws_future_bool *aws_async_input_stream_read(struct aws_async_input_stream *stream, struct aws_byte_buf *dest) {
    AWS_PRECONDITION(stream);
    AWS_PRECONDITION(dest);

    /* Ensure the buffer has space available */
    if (dest->len == dest->capacity) {
        struct aws_future_bool *future = aws_future_bool_new(stream->alloc);
        aws_future_bool_set_error(future, AWS_ERROR_SHORT_BUFFER);
        return future;
    }

    struct aws_future_bool *future = stream->vtable->read(stream, dest);
    AWS_POSTCONDITION(future != NULL);
    return future;
}

/* Data to perform the aws_async_input_stream_read_to_fill() job */
struct aws_async_input_stream_fill_job {
    struct aws_allocator *alloc;
    struct aws_async_input_stream *stream;
    struct aws_byte_buf *dest;
    /* Future for each read() step */
    struct aws_future_bool *read_step_future;
    /* Future to set when this fill job completes */
    struct aws_future_bool *on_complete_future;
};

static void s_async_stream_fill_job_complete(
    struct aws_async_input_stream_fill_job *fill_job,
    bool eof,
    int error_code) {

    if (error_code) {
        aws_future_bool_set_error(fill_job->on_complete_future, error_code);
    } else {
        aws_future_bool_set_result(fill_job->on_complete_future, eof);
    }
    aws_future_bool_release(fill_job->on_complete_future);
    aws_async_input_stream_release(fill_job->stream);
    aws_mem_release(fill_job->alloc, fill_job);
}

/* Call read() in a loop.
 * It would be simpler to set a completion callback for each read() call,
 * but this risks our call stack growing large if there are many small, synchronous, reads.
 * So be complicated and loop until a read() ) call is actually async,
 * and only then set the completion callback (which is this same function, where we resume looping). */
static void s_async_stream_fill_job_loop(void *user_data) {
    struct aws_async_input_stream_fill_job *fill_job = user_data;

    while (true) {
        /* Process read_step_future from previous iteration of loop.
         * It's NULL the first time the job ever enters the loop.
         * But it's set in subsequent runs of the loop,
         * and when this is a read_step_future completion callback. */
        if (fill_job->read_step_future) {
            if (aws_future_bool_register_callback_if_not_done(
                    fill_job->read_step_future, s_async_stream_fill_job_loop, fill_job)) {

                /* not done, we'll resume this loop when callback fires */
                return;
            }

            /* read_step_future is done */
            int error_code = aws_future_bool_get_error(fill_job->read_step_future);
            bool eof = error_code ? false : aws_future_bool_get_result(fill_job->read_step_future);
            bool reached_capacity = fill_job->dest->len == fill_job->dest->capacity;
            fill_job->read_step_future = aws_future_bool_release(fill_job->read_step_future); /* release and NULL */

            if (error_code || eof || reached_capacity) {
                /* job complete! */
                s_async_stream_fill_job_complete(fill_job, eof, error_code);
                return;
            }
        }

        /* Kick off a read, which may or may not complete async */
        fill_job->read_step_future = aws_async_input_stream_read(fill_job->stream, fill_job->dest);
    }
}

struct aws_future_bool *aws_async_input_stream_read_to_fill(
    struct aws_async_input_stream *stream,
    struct aws_byte_buf *dest) {

    AWS_PRECONDITION(stream);
    AWS_PRECONDITION(dest);

    struct aws_future_bool *future = aws_future_bool_new(stream->alloc);

    /* Ensure the buffer has space available */
    if (dest->len == dest->capacity) {
        aws_future_bool_set_error(future, AWS_ERROR_SHORT_BUFFER);
        return future;
    }

    /* Prepare for async job */
    struct aws_async_input_stream_fill_job *fill_job =
        aws_mem_calloc(stream->alloc, 1, sizeof(struct aws_async_input_stream_fill_job));
    fill_job->alloc = stream->alloc;
    fill_job->stream = aws_async_input_stream_acquire(stream);
    fill_job->dest = dest;
    fill_job->on_complete_future = aws_future_bool_acquire(future);

    /* Kick off work */
    s_async_stream_fill_job_loop(fill_job);

    return future;
}
