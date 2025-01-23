/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/stream.h>

#include <aws/common/file.h>
#include <aws/io/file_utils.h>
#include <aws/io/private/tracing.h>

#include <errno.h>

int aws_input_stream_seek(struct aws_input_stream *stream, int64_t offset, enum aws_stream_seek_basis basis) {
    AWS_ASSERT(stream && stream->vtable && stream->vtable->seek);

    return stream->vtable->seek(stream, offset, basis);
}

int aws_input_stream_read(struct aws_input_stream *stream, struct aws_byte_buf *dest) {
    AWS_ASSERT(stream && stream->vtable && stream->vtable->read);
    AWS_ASSERT(dest);
    AWS_ASSERT(dest->len <= dest->capacity);

    /* Deal with this edge case here, instead of relying on every implementation to do it right. */
    if (dest->capacity == dest->len) {
        return AWS_OP_SUCCESS;
    }

    /* Prevent implementations from accidentally overwriting existing data in the buffer.
     * Hand them a "safe" buffer that starts where the existing data ends. */
    const void *safe_buf_start = dest->buffer + dest->len;
    const size_t safe_buf_capacity = dest->capacity - dest->len;
    struct aws_byte_buf safe_buf = aws_byte_buf_from_empty_array(safe_buf_start, safe_buf_capacity);

    __itt_task_begin(io_tracing_domain, __itt_null, __itt_null, tracing_input_stream_read);
    int read_result = stream->vtable->read(stream, &safe_buf);
    __itt_task_end(io_tracing_domain);

    /* Ensure the implementation did not commit forbidden acts upon the buffer */
    AWS_FATAL_ASSERT(
        (safe_buf.buffer == safe_buf_start) && (safe_buf.capacity == safe_buf_capacity) &&
        (safe_buf.len <= safe_buf_capacity));

    if (read_result == AWS_OP_SUCCESS) {
        /* Update the actual buffer */
        dest->len += safe_buf.len;
    }

    return read_result;
}

int aws_input_stream_get_status(struct aws_input_stream *stream, struct aws_stream_status *status) {
    AWS_ASSERT(stream && stream->vtable && stream->vtable->get_status);

    return stream->vtable->get_status(stream, status);
}

int aws_input_stream_get_length(struct aws_input_stream *stream, int64_t *out_length) {
    AWS_ASSERT(stream && stream->vtable && stream->vtable->get_length);

    return stream->vtable->get_length(stream, out_length);
}

/*
 * cursor stream implementation
 */

struct aws_input_stream_byte_cursor_impl {
    struct aws_input_stream base;
    struct aws_allocator *allocator;
    struct aws_byte_cursor original_cursor;
    struct aws_byte_cursor current_cursor;
};

/*
 * This is an ugly function that, in the absence of better guidance, is designed to handle all possible combinations of
 * size_t (uint32_t, uint64_t).  If size_t ever exceeds 64 bits this function will fail badly.
 *
 *  Safety and invariant assumptions are sprinkled via comments.  The overall strategy is to cast up to 64 bits and
 * perform all arithmetic there, being careful with signed vs. unsigned to prevent bad operations.
 *
 *  Assumption #1: size_t resolves to an unsigned integer 64 bits or smaller
 */

AWS_STATIC_ASSERT(sizeof(size_t) <= 8);

static int s_aws_input_stream_byte_cursor_seek(
    struct aws_input_stream *stream,
    int64_t offset,
    enum aws_stream_seek_basis basis) {
    struct aws_input_stream_byte_cursor_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_input_stream_byte_cursor_impl, base);

    uint64_t final_offset = 0;

    switch (basis) {
        case AWS_SSB_BEGIN:
            /*
             * (uint64_t)offset -- safe by virtue of the earlier is-negative check
             * (uint64_t)impl->original_cursor.len -- safe via assumption 1
             */
            if (offset < 0 || (uint64_t)offset > (uint64_t)impl->original_cursor.len) {
                return aws_raise_error(AWS_IO_STREAM_INVALID_SEEK_POSITION);
            }

            /* safe because negative offsets were turned into an error */
            final_offset = (uint64_t)offset;
            break;

        case AWS_SSB_END:
            /*
             * -offset -- safe as long offset is not INT64_MIN which was previously checked
             * (uint64_t)(-offset) -- safe because (-offset) is positive (and < INT64_MAX < UINT64_MAX)
             */
            if (offset > 0 || offset == INT64_MIN || (uint64_t)(-offset) > (uint64_t)impl->original_cursor.len) {
                return aws_raise_error(AWS_IO_STREAM_INVALID_SEEK_POSITION);
            }

            /* cases that would make this unsafe became errors with previous conditional */
            final_offset = (uint64_t)impl->original_cursor.len - (uint64_t)(-offset);
            break;

        default:
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* true because we already validated against (impl->original_cursor.len) which is <= SIZE_MAX */
    AWS_ASSERT(final_offset <= SIZE_MAX);

    /* safe via previous assert */
    size_t final_offset_sz = (size_t)final_offset;

    /* sanity */
    AWS_ASSERT(final_offset_sz <= impl->original_cursor.len);

    /* reset current_cursor to new position */
    impl->current_cursor = impl->original_cursor;
    impl->current_cursor.ptr += final_offset_sz;
    impl->current_cursor.len -= final_offset_sz;

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_byte_cursor_read(struct aws_input_stream *stream, struct aws_byte_buf *dest) {
    struct aws_input_stream_byte_cursor_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_input_stream_byte_cursor_impl, base);

    size_t actually_read = dest->capacity - dest->len;
    if (actually_read > impl->current_cursor.len) {
        actually_read = impl->current_cursor.len;
    }

    if (!aws_byte_buf_write(dest, impl->current_cursor.ptr, actually_read)) {
        return aws_raise_error(AWS_IO_STREAM_READ_FAILED);
    }

    aws_byte_cursor_advance(&impl->current_cursor, actually_read);

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_byte_cursor_get_status(
    struct aws_input_stream *stream,
    struct aws_stream_status *status) {
    struct aws_input_stream_byte_cursor_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_input_stream_byte_cursor_impl, base);

    status->is_end_of_stream = impl->current_cursor.len == 0;
    status->is_valid = true;

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_byte_cursor_get_length(struct aws_input_stream *stream, int64_t *out_length) {
    struct aws_input_stream_byte_cursor_impl *impl =
        AWS_CONTAINER_OF(stream, struct aws_input_stream_byte_cursor_impl, base);

#if SIZE_MAX > INT64_MAX
    size_t length = impl->original_cursor.len;
    if (length > INT64_MAX) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
#endif

    *out_length = (int64_t)impl->original_cursor.len;

    return AWS_OP_SUCCESS;
}

static void s_aws_input_stream_byte_cursor_destroy(struct aws_input_stream_byte_cursor_impl *impl) {
    aws_mem_release(impl->allocator, impl);
}

static struct aws_input_stream_vtable s_aws_input_stream_byte_cursor_vtable = {
    .seek = s_aws_input_stream_byte_cursor_seek,
    .read = s_aws_input_stream_byte_cursor_read,
    .get_status = s_aws_input_stream_byte_cursor_get_status,
    .get_length = s_aws_input_stream_byte_cursor_get_length,
};

struct aws_input_stream *aws_input_stream_new_from_cursor(
    struct aws_allocator *allocator,
    const struct aws_byte_cursor *cursor) {

    struct aws_input_stream_byte_cursor_impl *impl =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_input_stream_byte_cursor_impl));

    impl->allocator = allocator;
    impl->original_cursor = *cursor;
    impl->current_cursor = *cursor;
    impl->base.vtable = &s_aws_input_stream_byte_cursor_vtable;
    aws_ref_count_init(
        &impl->base.ref_count, impl, (aws_simple_completion_callback *)s_aws_input_stream_byte_cursor_destroy);

    return &impl->base;
}

/*
 * file-based input stream
 */
struct aws_input_stream_file_impl {
    struct aws_input_stream base;
    struct aws_allocator *allocator;
    FILE *file;
    bool close_on_clean_up;
};

static int s_aws_input_stream_file_seek(
    struct aws_input_stream *stream,
    int64_t offset,
    enum aws_stream_seek_basis basis) {
    struct aws_input_stream_file_impl *impl = AWS_CONTAINER_OF(stream, struct aws_input_stream_file_impl, base);

    int whence = (basis == AWS_SSB_BEGIN) ? SEEK_SET : SEEK_END;
    if (aws_fseek(impl->file, offset, whence)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_file_read(struct aws_input_stream *stream, struct aws_byte_buf *dest) {
    struct aws_input_stream_file_impl *impl = AWS_CONTAINER_OF(stream, struct aws_input_stream_file_impl, base);

    size_t max_read = dest->capacity - dest->len;
    size_t actually_read = fread(dest->buffer + dest->len, 1, max_read, impl->file);
    if (actually_read == 0) {
        if (ferror(impl->file)) {
            return aws_raise_error(AWS_IO_STREAM_READ_FAILED);
        }
    }

    dest->len += actually_read;

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_file_get_status(struct aws_input_stream *stream, struct aws_stream_status *status) {
    struct aws_input_stream_file_impl *impl = AWS_CONTAINER_OF(stream, struct aws_input_stream_file_impl, base);

    status->is_end_of_stream = feof(impl->file) != 0;
    status->is_valid = ferror(impl->file) == 0;

    return AWS_OP_SUCCESS;
}

static int s_aws_input_stream_file_get_length(struct aws_input_stream *stream, int64_t *length) {
    struct aws_input_stream_file_impl *impl = AWS_CONTAINER_OF(stream, struct aws_input_stream_file_impl, base);

    return aws_file_get_length(impl->file, length);
}

static void s_aws_input_stream_file_destroy(struct aws_input_stream_file_impl *impl) {

    if (impl->close_on_clean_up && impl->file) {
        fclose(impl->file);
    }
    aws_mem_release(impl->allocator, impl);
}

static struct aws_input_stream_vtable s_aws_input_stream_file_vtable = {
    .seek = s_aws_input_stream_file_seek,
    .read = s_aws_input_stream_file_read,
    .get_status = s_aws_input_stream_file_get_status,
    .get_length = s_aws_input_stream_file_get_length,
};

struct aws_input_stream *aws_input_stream_new_from_file(struct aws_allocator *allocator, const char *file_name) {

    struct aws_input_stream_file_impl *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_input_stream_file_impl));

    impl->file = aws_fopen(file_name, "rb");
    if (impl->file == NULL) {
        goto on_error;
    }

    impl->close_on_clean_up = true;
    impl->allocator = allocator;
    impl->base.vtable = &s_aws_input_stream_file_vtable;
    aws_ref_count_init(&impl->base.ref_count, impl, (aws_simple_completion_callback *)s_aws_input_stream_file_destroy);

    return &impl->base;

on_error:
    aws_mem_release(allocator, impl);
    return NULL;
}

struct aws_input_stream *aws_input_stream_new_from_open_file(struct aws_allocator *allocator, FILE *file) {
    struct aws_input_stream_file_impl *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_input_stream_file_impl));

    impl->file = file;
    impl->close_on_clean_up = false;
    impl->allocator = allocator;

    impl->base.vtable = &s_aws_input_stream_file_vtable;
    aws_ref_count_init(&impl->base.ref_count, impl, (aws_simple_completion_callback *)s_aws_input_stream_file_destroy);
    return &impl->base;
}

struct aws_input_stream *aws_input_stream_acquire(struct aws_input_stream *stream) {
    if (stream != NULL) {
        if (stream->vtable->acquire) {
            stream->vtable->acquire(stream);
        } else {
            aws_ref_count_acquire(&stream->ref_count);
        }
    }
    return stream;
}

struct aws_input_stream *aws_input_stream_release(struct aws_input_stream *stream) {
    if (stream != NULL) {
        if (stream->vtable->release) {
            stream->vtable->release(stream);
        } else {
            aws_ref_count_release(&stream->ref_count);
        }
    }
    return NULL;
}

void aws_input_stream_destroy(struct aws_input_stream *stream) {
    aws_input_stream_release(stream);
}
