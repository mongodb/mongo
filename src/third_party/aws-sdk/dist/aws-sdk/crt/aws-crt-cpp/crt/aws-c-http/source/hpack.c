/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/http/private/hpack.h>

/* #TODO test empty strings */

/* #TODO remove all OOM error handling in HTTP/2 & HPACK. make functions void if possible */

/* RFC-7540 6.5.2 */
const size_t s_hpack_dynamic_table_initial_size = 4096;
const size_t s_hpack_dynamic_table_initial_elements = 512;
/* TODO: shouldn't be a hardcoded max_size, it should be driven by SETTINGS_HEADER_TABLE_SIZE */
const size_t s_hpack_dynamic_table_max_size = 16 * 1024 * 1024;

/* Used for growing the dynamic table buffer when it fills up */
const float s_hpack_dynamic_table_buffer_growth_rate = 1.5F;

struct aws_http_header s_static_header_table[] = {
#define HEADER(_index, _name)                                                                                          \
    [_index] = {                                                                                                       \
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),                                                          \
    },

#define HEADER_WITH_VALUE(_index, _name, _value)                                                                       \
    [_index] = {                                                                                                       \
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),                                                          \
        .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_value),                                                        \
    },

#include <aws/http/private/hpack_header_static_table.def>

#undef HEADER
#undef HEADER_WITH_VALUE
};
static const size_t s_static_header_table_size = AWS_ARRAY_SIZE(s_static_header_table);

struct aws_byte_cursor s_static_header_table_name_only[] = {
#define HEADER(_index, _name) [_index] = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),
#define HEADER_WITH_VALUE(_index, _name, _value) HEADER(_index, _name)

#include <aws/http/private/hpack_header_static_table.def>

#undef HEADER
#undef HEADER_WITH_VALUE
};

/* aws_http_header * -> size_t */
static struct aws_hash_table s_static_header_reverse_lookup;
/* aws_byte_cursor * -> size_t */
static struct aws_hash_table s_static_header_reverse_lookup_name_only;

static uint64_t s_header_hash(const void *key) {
    const struct aws_http_header *header = key;

    return aws_hash_combine(aws_hash_byte_cursor_ptr(&header->name), aws_hash_byte_cursor_ptr(&header->value));
}

static bool s_header_eq(const void *a, const void *b) {
    const struct aws_http_header *left = a;
    const struct aws_http_header *right = b;

    if (!aws_byte_cursor_eq(&left->name, &right->name)) {
        return false;
    }

    /* If the header stored in the table doesn't have a value, then it's a match */
    return aws_byte_cursor_eq(&left->value, &right->value);
}

void aws_hpack_static_table_init(struct aws_allocator *allocator) {

    int result = aws_hash_table_init(
        &s_static_header_reverse_lookup,
        allocator,
        s_static_header_table_size - 1,
        s_header_hash,
        s_header_eq,
        NULL,
        NULL);
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

    result = aws_hash_table_init(
        &s_static_header_reverse_lookup_name_only,
        allocator,
        s_static_header_table_size - 1,
        aws_hash_byte_cursor_ptr,
        (aws_hash_callback_eq_fn *)aws_byte_cursor_eq,
        NULL,
        NULL);
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

    /* Process in reverse so that name_only prefers lower indices */
    for (size_t i = s_static_header_table_size - 1; i > 0; --i) {
        /* the tables are created as 1-based indexing */
        result = aws_hash_table_put(&s_static_header_reverse_lookup, &s_static_header_table[i], (void *)i, NULL);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

        result = aws_hash_table_put(
            &s_static_header_reverse_lookup_name_only, &s_static_header_table_name_only[i], (void *)(i), NULL);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);
    }
}

void aws_hpack_static_table_clean_up(void) {
    aws_hash_table_clean_up(&s_static_header_reverse_lookup);
    aws_hash_table_clean_up(&s_static_header_reverse_lookup_name_only);
}

#define HPACK_LOGF(level, hpack, text, ...)                                                                            \
    AWS_LOGF_##level((hpack)->log_subject, "id=%p [HPACK]: " text, (hpack)->log_id, __VA_ARGS__)
#define HPACK_LOG(level, hpack, text) HPACK_LOGF(level, hpack, "%s", text)

void aws_hpack_context_init(
    struct aws_hpack_context *context,
    struct aws_allocator *allocator,
    enum aws_http_log_subject log_subject,
    const void *log_id) {

    AWS_ZERO_STRUCT(*context);
    context->allocator = allocator;
    context->log_subject = log_subject;
    context->log_id = log_id;

    /* Initialize dynamic table */
    context->dynamic_table.max_size = s_hpack_dynamic_table_initial_size;
    context->dynamic_table.buffer_capacity = s_hpack_dynamic_table_initial_elements;
    context->dynamic_table.buffer =
        aws_mem_calloc(allocator, context->dynamic_table.buffer_capacity, sizeof(struct aws_http_header));

    aws_hash_table_init(
        &context->dynamic_table.reverse_lookup,
        allocator,
        s_hpack_dynamic_table_initial_elements,
        s_header_hash,
        s_header_eq,
        NULL,
        NULL);

    aws_hash_table_init(
        &context->dynamic_table.reverse_lookup_name_only,
        allocator,
        s_hpack_dynamic_table_initial_elements,
        aws_hash_byte_cursor_ptr,
        (aws_hash_callback_eq_fn *)aws_byte_cursor_eq,
        NULL,
        NULL);
}

static struct aws_http_header *s_dynamic_table_get(const struct aws_hpack_context *context, size_t index);

static void s_clean_up_dynamic_table_buffer(struct aws_hpack_context *context) {
    while (context->dynamic_table.num_elements > 0) {
        struct aws_http_header *back = s_dynamic_table_get(context, context->dynamic_table.num_elements - 1);
        context->dynamic_table.num_elements -= 1;
        /* clean-up the memory we allocate for it */
        aws_mem_release(context->allocator, back->name.ptr);
    }
    aws_mem_release(context->allocator, context->dynamic_table.buffer);
}

void aws_hpack_context_clean_up(struct aws_hpack_context *context) {
    if (context->dynamic_table.buffer) {
        s_clean_up_dynamic_table_buffer(context);
    }
    aws_hash_table_clean_up(&context->dynamic_table.reverse_lookup);
    aws_hash_table_clean_up(&context->dynamic_table.reverse_lookup_name_only);
    AWS_ZERO_STRUCT(*context);
}

size_t aws_hpack_get_header_size(const struct aws_http_header *header) {
    return header->name.len + header->value.len + 32;
}

size_t aws_hpack_get_dynamic_table_num_elements(const struct aws_hpack_context *context) {
    return context->dynamic_table.num_elements;
}

size_t aws_hpack_get_dynamic_table_max_size(const struct aws_hpack_context *context) {
    return context->dynamic_table.max_size;
}

/*
 * Gets the header from the dynamic table.
 * NOTE: This function only bounds checks on the buffer size, not the number of elements.
 */
static struct aws_http_header *s_dynamic_table_get(const struct aws_hpack_context *context, size_t index) {

    AWS_ASSERT(index < context->dynamic_table.buffer_capacity);

    return &context->dynamic_table
                .buffer[(context->dynamic_table.index_0 + index) % context->dynamic_table.buffer_capacity];
}

const struct aws_http_header *aws_hpack_get_header(const struct aws_hpack_context *context, size_t index) {
    if (index == 0 || index >= s_static_header_table_size + context->dynamic_table.num_elements) {
        aws_raise_error(AWS_ERROR_INVALID_INDEX);
        return NULL;
    }

    /* Check static table */
    if (index < s_static_header_table_size) {
        return &s_static_header_table[index];
    }

    /* Check dynamic table */
    return s_dynamic_table_get(context, index - s_static_header_table_size);
}

/* TODO: remove `bool search_value`, this option has no reason to exist */
size_t aws_hpack_find_index(
    const struct aws_hpack_context *context,
    const struct aws_http_header *header,
    bool search_value,
    bool *found_value) {

    *found_value = false;

    struct aws_hash_element *elem = NULL;
    if (search_value) {
        /* Check name-and-value first in static table */
        aws_hash_table_find(&s_static_header_reverse_lookup, header, &elem);
        if (elem) {
            /* TODO: Maybe always set found_value to true? Who cares that the value is empty if they matched? */
            /* If an element was found, check if it has a value */
            *found_value = ((const struct aws_http_header *)elem->key)->value.len;
            return (size_t)elem->value;
        }
        /* Check name-and-value in dynamic table */
        aws_hash_table_find(&context->dynamic_table.reverse_lookup, header, &elem);
        if (elem) {
            /* TODO: Maybe always set found_value to true? Who cares that the value is empty if they matched? */
            *found_value = ((const struct aws_http_header *)elem->key)->value.len;
            goto trans_index_from_dynamic_table;
        }
    }
    /* Check the name-only table. Note, even if we search for value, when we fail in searching for name-and-value, we
     * should also check the name only table */
    aws_hash_table_find(&s_static_header_reverse_lookup_name_only, &header->name, &elem);
    if (elem) {
        return (size_t)elem->value;
    }
    aws_hash_table_find(&context->dynamic_table.reverse_lookup_name_only, &header->name, &elem);
    if (elem) {
        goto trans_index_from_dynamic_table;
    }
    return 0;

trans_index_from_dynamic_table:
    AWS_ASSERT(elem);
    size_t index;
    const size_t absolute_index = (size_t)elem->value;
    if (absolute_index >= context->dynamic_table.index_0) {
        index = absolute_index - context->dynamic_table.index_0;
    } else {
        index = (context->dynamic_table.buffer_capacity - context->dynamic_table.index_0) + absolute_index;
    }
    /* Need to add the static table size to re-base indicies */
    index += s_static_header_table_size;
    return index;
}

/* Remove elements from the dynamic table until it fits in max_size bytes */
static int s_dynamic_table_shrink(struct aws_hpack_context *context, size_t max_size) {
    while (context->dynamic_table.size > max_size && context->dynamic_table.num_elements > 0) {
        struct aws_http_header *back = s_dynamic_table_get(context, context->dynamic_table.num_elements - 1);

        /* "Remove" the header from the table */
        context->dynamic_table.size -= aws_hpack_get_header_size(back);
        context->dynamic_table.num_elements -= 1;

        /* Remove old header from hash tables */
        if (aws_hash_table_remove(&context->dynamic_table.reverse_lookup, back, NULL, NULL)) {
            HPACK_LOG(ERROR, context, "Failed to remove header from the reverse lookup table");
            goto error;
        }

        /* If the name-only lookup is pointing to the element we're removing, it needs to go.
         * If not, it's pointing to a younger, sexier element. */
        struct aws_hash_element *elem = NULL;
        aws_hash_table_find(&context->dynamic_table.reverse_lookup_name_only, &back->name, &elem);
        if (elem && elem->key == back) {
            if (aws_hash_table_remove_element(&context->dynamic_table.reverse_lookup_name_only, elem)) {
                HPACK_LOG(ERROR, context, "Failed to remove header from the reverse lookup (name-only) table");
                goto error;
            }
        }

        /* clean up the memory we allocated to hold the name and value string*/
        aws_mem_release(context->allocator, back->name.ptr);
    }

    return AWS_OP_SUCCESS;

error:
    return AWS_OP_ERR;
}

/*
 * Resizes the dynamic table storage buffer to new_max_elements.
 * Useful when inserting over capacity, or when downsizing.
 * Do shrink first, if you want to remove elements, or memory leak will happen.
 */
static int s_dynamic_table_resize_buffer(struct aws_hpack_context *context, size_t new_max_elements) {

    /* Clear the old hash tables */
    aws_hash_table_clear(&context->dynamic_table.reverse_lookup);
    aws_hash_table_clear(&context->dynamic_table.reverse_lookup_name_only);

    struct aws_http_header *new_buffer = NULL;

    if (AWS_UNLIKELY(new_max_elements == 0)) {
        /* If new buffer is of size 0, don't both initializing, just clean up the old one. */
        goto cleanup_old_buffer;
    }

    /* Allocate the new buffer */
    new_buffer = aws_mem_calloc(context->allocator, new_max_elements, sizeof(struct aws_http_header));
    if (!new_buffer) {
        return AWS_OP_ERR;
    }

    /* Don't bother copying data if old buffer was of size 0 */
    if (AWS_UNLIKELY(context->dynamic_table.num_elements == 0)) {
        goto reset_dyn_table_state;
    }

    /*
     * Take a buffer that looks like this:
     *
     *               Index 0
     *               ^
     * +---------------------------+
     * | Below Block | Above Block |
     * +---------------------------+
     * And make it look like this:
     *
     * Index 0
     * ^
     * +-------------+-------------+
     * | Above Block | Below Block |
     * +-------------+-------------+
     */

    /* Copy as much the above block as possible */
    size_t above_block_size = context->dynamic_table.buffer_capacity - context->dynamic_table.index_0;
    if (above_block_size > new_max_elements) {
        above_block_size = new_max_elements;
    }
    memcpy(
        new_buffer,
        context->dynamic_table.buffer + context->dynamic_table.index_0,
        above_block_size * sizeof(struct aws_http_header));

    /* Copy as much of below block as possible */
    const size_t free_blocks_available = new_max_elements - above_block_size;
    const size_t old_blocks_to_copy = context->dynamic_table.buffer_capacity - above_block_size;
    const size_t below_block_size = aws_min_size(free_blocks_available, old_blocks_to_copy);
    if (below_block_size) {
        memcpy(
            new_buffer + above_block_size,
            context->dynamic_table.buffer,
            below_block_size * sizeof(struct aws_http_header));
    }

    /* Free the old memory */
cleanup_old_buffer:
    aws_mem_release(context->allocator, context->dynamic_table.buffer);

    /* Reset state */
reset_dyn_table_state:
    if (context->dynamic_table.num_elements > new_max_elements) {
        context->dynamic_table.num_elements = new_max_elements;
    }
    context->dynamic_table.buffer_capacity = new_max_elements;
    context->dynamic_table.index_0 = 0;
    context->dynamic_table.buffer = new_buffer;

    /* Re-insert all of the reverse lookup elements */
    for (size_t i = 0; i < context->dynamic_table.num_elements; ++i) {
        if (aws_hash_table_put(
                &context->dynamic_table.reverse_lookup, &context->dynamic_table.buffer[i], (void *)i, NULL)) {
            return AWS_OP_ERR;
        }
        if (aws_hash_table_put(
                &context->dynamic_table.reverse_lookup_name_only,
                &context->dynamic_table.buffer[i].name,
                (void *)i,
                NULL)) {
            return AWS_OP_ERR;
        }
    }

    return AWS_OP_SUCCESS;
}

int aws_hpack_insert_header(struct aws_hpack_context *context, const struct aws_http_header *header) {

    /* Don't move forward if no elements allowed in the dynamic table */
    if (AWS_UNLIKELY(context->dynamic_table.max_size == 0)) {
        return AWS_OP_SUCCESS;
    }

    const size_t header_size = aws_hpack_get_header_size(header);

    /* If for whatever reason this new header is bigger than the total table size, burn everything to the ground. */
    if (AWS_UNLIKELY(header_size > context->dynamic_table.max_size)) {
        /* #TODO handle this. It's not an error. It should simply result in an empty table RFC-7541 4.4 */
        goto error;
    }

    /* Rotate out headers until there's room for the new header (this function will return immediately if nothing needs
     * to be evicted) */
    if (s_dynamic_table_shrink(context, context->dynamic_table.max_size - header_size)) {
        goto error;
    }

    /* If we're out of space in the buffer, grow it */
    if (context->dynamic_table.num_elements == context->dynamic_table.buffer_capacity) {
        /* If the buffer is currently of 0 size, reset it back to its initial size */
        const size_t new_size =
            context->dynamic_table.buffer_capacity
                ? (size_t)(context->dynamic_table.buffer_capacity * s_hpack_dynamic_table_buffer_growth_rate)
                : s_hpack_dynamic_table_initial_elements;

        if (s_dynamic_table_resize_buffer(context, new_size)) {
            goto error;
        }
    }

    /* Decrement index 0, wrapping if necessary */
    if (context->dynamic_table.index_0 == 0) {
        context->dynamic_table.index_0 = context->dynamic_table.buffer_capacity - 1;
    } else {
        context->dynamic_table.index_0--;
    }

    /* Increment num_elements */
    context->dynamic_table.num_elements++;
    /* Increment the size */
    context->dynamic_table.size += header_size;

    /* Put the header at the "front" of the table */
    struct aws_http_header *table_header = s_dynamic_table_get(context, 0);

    /* TODO:: We can optimize this with ring buffer. */
    /* allocate memory for the name and value, which will be deallocated whenever the entry is evicted from the table or
     * the table is cleaned up. We keep the pointer in the name pointer of each entry */
    const size_t buf_memory_size = header->name.len + header->value.len;

    if (buf_memory_size) {
        uint8_t *buf_memory = aws_mem_acquire(context->allocator, buf_memory_size);
        if (!buf_memory) {
            return AWS_OP_ERR;
        }
        struct aws_byte_buf buf = aws_byte_buf_from_empty_array(buf_memory, buf_memory_size);
        /* Copy header, then backup strings into our own allocation */
        *table_header = *header;
        aws_byte_buf_append_and_update(&buf, &table_header->name);
        aws_byte_buf_append_and_update(&buf, &table_header->value);
    } else {
        /* if buf_memory_size is 0, no memory needed, we will insert the empty header into dynamic table */
        *table_header = *header;
        table_header->name.ptr = NULL;
        table_header->value.ptr = NULL;
    }
    /* Write the new header to the look up tables */
    if (aws_hash_table_put(
            &context->dynamic_table.reverse_lookup, table_header, (void *)context->dynamic_table.index_0, NULL)) {
        goto error;
    }
    /* Note that we can just blindly put here, we want to overwrite any older entry so it isn't accidentally removed. */
    if (aws_hash_table_put(
            &context->dynamic_table.reverse_lookup_name_only,
            &table_header->name,
            (void *)context->dynamic_table.index_0,
            NULL)) {
        goto error;
    }

    return AWS_OP_SUCCESS;

error:
    /* Do not attempt to handle the error, if something goes wrong, close the connection */
    return AWS_OP_ERR;
}

int aws_hpack_resize_dynamic_table(struct aws_hpack_context *context, size_t new_max_size) {

    /* Nothing to see here! */
    if (new_max_size == context->dynamic_table.max_size) {
        return AWS_OP_SUCCESS;
    }

    if (new_max_size > s_hpack_dynamic_table_max_size) {

        HPACK_LOGF(
            ERROR,
            context,
            "New dynamic table max size %zu is greater than the supported max size (%zu)",
            new_max_size,
            s_hpack_dynamic_table_max_size);
        aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
        goto error;
    }

    /* If downsizing, remove elements until we're within the new size constraints */
    if (s_dynamic_table_shrink(context, new_max_size)) {
        goto error;
    }

    /* Resize the buffer to the current size */
    if (s_dynamic_table_resize_buffer(context, context->dynamic_table.num_elements)) {
        goto error;
    }

    /* Update the max size */
    context->dynamic_table.max_size = new_max_size;

    return AWS_OP_SUCCESS;

error:
    return AWS_OP_ERR;
}
