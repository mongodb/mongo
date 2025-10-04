/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/file.h>
#include <aws/common/linked_list.h>
#include <aws/common/logging.h>
#include <aws/common/string.h>

#include <errno.h>

/* For "special files", the OS often lies about size.
 * For example, on Amazon Linux 2:
 * /proc/cpuinfo: size is 0, but contents are several KB of data.
 * /sys/devices/virtual/dmi/id/product_name: size is 4096, but contents are "c5.2xlarge"
 *
 * Therefore, we may need to grow the buffer as we read until EOF.
 * This is the min/max step size for growth. */
#define MIN_BUFFER_GROWTH_READING_FILES 32
#define MAX_BUFFER_GROWTH_READING_FILES 4096

FILE *aws_fopen(const char *file_path, const char *mode) {
    if (!file_path || strlen(file_path) == 0) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_IO, "static: Failed to open file. path is empty");
        aws_raise_error(AWS_ERROR_FILE_INVALID_PATH);
        return NULL;
    }

    if (!mode || strlen(mode) == 0) {
        AWS_LOGF_ERROR(AWS_LS_COMMON_IO, "static: Failed to open file. mode is empty");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_string *file_path_str = aws_string_new_from_c_str(aws_default_allocator(), file_path);
    struct aws_string *mode_str = aws_string_new_from_c_str(aws_default_allocator(), mode);

    FILE *file = aws_fopen_safe(file_path_str, mode_str);
    aws_string_destroy(mode_str);
    aws_string_destroy(file_path_str);

    return file;
}

/* Helper function used by aws_byte_buf_init_from_file() and aws_byte_buf_init_from_file_with_size_hint() */
static int s_byte_buf_init_from_file_impl(
    struct aws_byte_buf *out_buf,
    struct aws_allocator *alloc,
    const char *filename,
    bool use_file_size_as_hint,
    size_t size_hint) {

    AWS_ZERO_STRUCT(*out_buf);
    FILE *fp = aws_fopen(filename, "rb");
    if (fp == NULL) {
        goto error;
    }

    if (use_file_size_as_hint) {
        int64_t len64 = 0;
        if (aws_file_get_length(fp, &len64)) {
            AWS_LOGF_ERROR(
                AWS_LS_COMMON_IO,
                "static: Failed to get file length. file:'%s' error:%s",
                filename,
                aws_error_name(aws_last_error()));
            goto error;
        }

        if (len64 >= SIZE_MAX) {
            aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
            AWS_LOGF_ERROR(
                AWS_LS_COMMON_IO,
                "static: File too large to read into memory. file:'%s' error:%s",
                filename,
                aws_error_name(aws_last_error()));
            goto error;
        }

        /* Leave space for null terminator at end of buffer */
        size_hint = (size_t)len64 + 1;
    }

    aws_byte_buf_init(out_buf, alloc, size_hint);

    /* Read in a loop until we hit EOF */
    while (true) {
        /* Expand buffer if necessary (at a reasonable rate) */
        if (out_buf->len == out_buf->capacity) {
            size_t additional_capacity = out_buf->capacity;
            additional_capacity = aws_max_size(MIN_BUFFER_GROWTH_READING_FILES, additional_capacity);
            additional_capacity = aws_min_size(MAX_BUFFER_GROWTH_READING_FILES, additional_capacity);
            if (aws_byte_buf_reserve_relative(out_buf, additional_capacity)) {
                AWS_LOGF_ERROR(AWS_LS_COMMON_IO, "static: Failed to grow buffer for file:'%s'", filename);
                goto error;
            }
        }

        size_t space_available = out_buf->capacity - out_buf->len;
        size_t bytes_read = fread(out_buf->buffer + out_buf->len, 1, space_available, fp);
        out_buf->len += bytes_read;

        /* If EOF, we're done! */
        if (feof(fp)) {
            break;
        }

        /* If no EOF but we read 0 bytes, there's been an error or at least we need
         * to treat it like one because we can't just infinitely loop. */
        if (bytes_read == 0) {
            int errno_value = ferror(fp) ? errno : 0; /* Always cache errno before potential side-effect */
            aws_translate_and_raise_io_error_or(errno_value, AWS_ERROR_FILE_READ_FAILURE);
            AWS_LOGF_ERROR(
                AWS_LS_COMMON_IO,
                "static: Failed reading file:'%s' errno:%d aws-error:%s",
                filename,
                errno_value,
                aws_error_name(aws_last_error()));
            goto error;
        }
    }

    /* A null terminator is appended, but is not included as part of the length field. */
    if (out_buf->len == out_buf->capacity) {
        if (aws_byte_buf_reserve_relative(out_buf, 1)) {
            AWS_LOGF_ERROR(AWS_LS_COMMON_IO, "static: Failed to grow buffer for file:'%s'", filename);
            goto error;
        }
    }
    out_buf->buffer[out_buf->len] = 0;

    fclose(fp);
    return AWS_OP_SUCCESS;

error:
    if (fp) {
        fclose(fp);
    }
    aws_byte_buf_clean_up_secure(out_buf);
    return AWS_OP_ERR;
}

int aws_byte_buf_init_from_file(struct aws_byte_buf *out_buf, struct aws_allocator *alloc, const char *filename) {
    return s_byte_buf_init_from_file_impl(out_buf, alloc, filename, true /*use_file_size_as_hint*/, 0 /*size_hint*/);
}

int aws_byte_buf_init_from_file_with_size_hint(
    struct aws_byte_buf *out_buf,
    struct aws_allocator *alloc,
    const char *filename,
    size_t size_hint) {

    return s_byte_buf_init_from_file_impl(out_buf, alloc, filename, false /*use_file_size_as_hint*/, size_hint);
}

bool aws_is_any_directory_separator(char value) {
    return value == '\\' || value == '/';
}

void aws_normalize_directory_separator(struct aws_byte_buf *path) {
    AWS_PRECONDITION(aws_byte_buf_is_valid(path));

    const char local_platform_separator = aws_get_platform_directory_separator();
    for (size_t i = 0; i < path->len; ++i) {
        if (aws_is_any_directory_separator((char)path->buffer[i])) {
            path->buffer[i] = local_platform_separator;
        }
    }

    AWS_POSTCONDITION(aws_byte_buf_is_valid(path));
}

struct aws_directory_iterator {
    struct aws_linked_list list_data;
    struct aws_allocator *allocator;
    struct aws_linked_list_node *current_node;
};

struct directory_entry_value {
    struct aws_directory_entry entry;
    struct aws_byte_buf path;
    struct aws_byte_buf relative_path;
    struct aws_linked_list_node node;
};

static bool s_directory_iterator_directory_entry(const struct aws_directory_entry *entry, void *user_data) {
    struct aws_directory_iterator *iterator = user_data;
    struct directory_entry_value *value = aws_mem_calloc(iterator->allocator, 1, sizeof(struct directory_entry_value));

    value->entry = *entry;
    aws_byte_buf_init_copy_from_cursor(&value->path, iterator->allocator, entry->path);
    value->entry.path = aws_byte_cursor_from_buf(&value->path);
    aws_byte_buf_init_copy_from_cursor(&value->relative_path, iterator->allocator, entry->relative_path);
    value->entry.relative_path = aws_byte_cursor_from_buf(&value->relative_path);
    aws_linked_list_push_back(&iterator->list_data, &value->node);

    return true;
}

struct aws_directory_iterator *aws_directory_entry_iterator_new(
    struct aws_allocator *allocator,
    const struct aws_string *path) {
    struct aws_directory_iterator *iterator = aws_mem_acquire(allocator, sizeof(struct aws_directory_iterator));
    iterator->allocator = allocator;
    aws_linked_list_init(&iterator->list_data);

    /* the whole point of this iterator is to avoid recursion, so let's do that by passing recurse as false. */
    if (AWS_OP_SUCCESS ==
        aws_directory_traverse(allocator, path, false, s_directory_iterator_directory_entry, iterator)) {
        if (!aws_linked_list_empty(&iterator->list_data)) {
            iterator->current_node = aws_linked_list_front(&iterator->list_data);
        }
        return iterator;
    }

    aws_mem_release(allocator, iterator);
    return NULL;
}

int aws_directory_entry_iterator_next(struct aws_directory_iterator *iterator) {
    struct aws_linked_list_node *node = iterator->current_node;

    if (!node || node->next == aws_linked_list_end(&iterator->list_data)) {
        return aws_raise_error(AWS_ERROR_LIST_EMPTY);
    }

    iterator->current_node = aws_linked_list_next(node);

    return AWS_OP_SUCCESS;
}

int aws_directory_entry_iterator_previous(struct aws_directory_iterator *iterator) {
    struct aws_linked_list_node *node = iterator->current_node;

    if (!node || node == aws_linked_list_begin(&iterator->list_data)) {
        return aws_raise_error(AWS_ERROR_LIST_EMPTY);
    }

    iterator->current_node = aws_linked_list_prev(node);

    return AWS_OP_SUCCESS;
}

void aws_directory_entry_iterator_destroy(struct aws_directory_iterator *iterator) {
    while (!aws_linked_list_empty(&iterator->list_data)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_front(&iterator->list_data);
        struct directory_entry_value *value = AWS_CONTAINER_OF(node, struct directory_entry_value, node);

        aws_byte_buf_clean_up(&value->path);
        aws_byte_buf_clean_up(&value->relative_path);

        aws_mem_release(iterator->allocator, value);
    }

    aws_mem_release(iterator->allocator, iterator);
}

const struct aws_directory_entry *aws_directory_entry_iterator_get_value(
    const struct aws_directory_iterator *iterator) {
    struct aws_linked_list_node *node = iterator->current_node;

    if (!iterator->current_node) {
        return NULL;
    }

    struct directory_entry_value *value = AWS_CONTAINER_OF(node, struct directory_entry_value, node);
    return &value->entry;
}
