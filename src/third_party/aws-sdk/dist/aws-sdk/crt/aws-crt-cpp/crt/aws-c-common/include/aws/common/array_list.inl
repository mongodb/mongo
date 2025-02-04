#ifndef AWS_COMMON_ARRAY_LIST_INL
#define AWS_COMMON_ARRAY_LIST_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* This is implicitly included, but helps with editor highlighting */
#include <aws/common/array_list.h>
/*
 * Do not add system headers here; add them to array_list.h. This file is included under extern "C" guards,
 * which might break system headers.
 */
AWS_EXTERN_C_BEGIN

AWS_STATIC_IMPL
int aws_array_list_init_dynamic(
    struct aws_array_list *AWS_RESTRICT list,
    struct aws_allocator *alloc,
    size_t initial_item_allocation,
    size_t item_size) {

    AWS_FATAL_PRECONDITION(list != NULL);
    AWS_FATAL_PRECONDITION(alloc != NULL);
    AWS_FATAL_PRECONDITION(item_size > 0);

    AWS_ZERO_STRUCT(*list);

    size_t allocation_size = 0;
    if (aws_mul_size_checked(initial_item_allocation, item_size, &allocation_size)) {
        goto error;
    }

    if (allocation_size > 0) {
        list->data = aws_mem_acquire(alloc, allocation_size);
        if (!list->data) {
            goto error;
        }
#ifdef DEBUG_BUILD
        memset(list->data, AWS_ARRAY_LIST_DEBUG_FILL, allocation_size);

#endif
        list->current_size = allocation_size;
    }
    list->item_size = item_size;
    list->alloc = alloc;

    AWS_FATAL_POSTCONDITION(list->current_size == 0 || list->data);
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return AWS_OP_SUCCESS;

error:
    AWS_POSTCONDITION(AWS_IS_ZEROED(*list));
    return AWS_OP_ERR;
}

AWS_STATIC_IMPL
void aws_array_list_init_static(
    struct aws_array_list *AWS_RESTRICT list,
    void *raw_array,
    size_t item_count,
    size_t item_size) {

    AWS_FATAL_PRECONDITION(list != NULL);
    AWS_FATAL_PRECONDITION(raw_array != NULL);
    AWS_FATAL_PRECONDITION(item_count > 0);
    AWS_FATAL_PRECONDITION(item_size > 0);

    AWS_ZERO_STRUCT(*list);
    list->alloc = NULL;

    size_t current_size = 0;
    int no_overflow = !aws_mul_size_checked(item_count, item_size, &current_size);
    AWS_FATAL_PRECONDITION(no_overflow);
    list->current_size = current_size;

    list->item_size = item_size;
    list->length = 0;
    list->data = raw_array;
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
}

AWS_STATIC_IMPL
void aws_array_list_init_static_from_initialized(
    struct aws_array_list *AWS_RESTRICT list,
    void *raw_array,
    size_t item_count,
    size_t item_size) {

    aws_array_list_init_static(list, raw_array, item_count, item_size);
    list->length = item_count;

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
}

AWS_STATIC_IMPL
bool aws_array_list_is_valid(const struct aws_array_list *AWS_RESTRICT list) {
    if (!list) {
        return false;
    }
    size_t required_size = 0;
    bool required_size_is_valid =
        (aws_mul_size_checked(list->length, list->item_size, &required_size) == AWS_OP_SUCCESS);
    bool current_size_is_valid = (list->current_size >= required_size);
    bool data_is_valid = AWS_IMPLIES(list->current_size == 0, list->data == NULL) &&
                         AWS_IMPLIES(list->current_size != 0, AWS_MEM_IS_WRITABLE(list->data, list->current_size));
    bool item_size_is_valid = (list->item_size != 0);
    return required_size_is_valid && current_size_is_valid && data_is_valid && item_size_is_valid;
}

AWS_STATIC_IMPL
void aws_array_list_clean_up(struct aws_array_list *AWS_RESTRICT list) {
    AWS_PRECONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
    if (list->alloc && list->data) {
        aws_mem_release(list->alloc, list->data);
    }

    AWS_ZERO_STRUCT(*list);
}

AWS_STATIC_IMPL
void aws_array_list_clean_up_secure(struct aws_array_list *AWS_RESTRICT list) {
    AWS_PRECONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
    if (list->alloc && list->data) {
        aws_secure_zero(list->data, list->current_size);
        aws_mem_release(list->alloc, list->data);
    }

    AWS_ZERO_STRUCT(*list);
}

AWS_STATIC_IMPL
int aws_array_list_push_back(struct aws_array_list *AWS_RESTRICT list, const void *val) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_READABLE(val, list->item_size),
        "Input pointer [val] must point writable memory of [list->item_size] bytes.");

    int err_code = aws_array_list_set_at(list, val, aws_array_list_length(list));

    if (err_code && aws_last_error() == AWS_ERROR_INVALID_INDEX && !list->alloc) {
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return aws_raise_error(AWS_ERROR_LIST_EXCEEDS_MAX_SIZE);
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return err_code;
}

AWS_STATIC_IMPL
int aws_array_list_front(const struct aws_array_list *AWS_RESTRICT list, void *val) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_WRITABLE(val, list->item_size),
        "Input pointer [val] must point writable memory of [list->item_size] bytes.");
    if (aws_array_list_length(list) > 0) {
        memcpy(val, list->data, list->item_size);
        AWS_POSTCONDITION(AWS_BYTES_EQ(val, list->data, list->item_size));
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_LIST_EMPTY);
}

AWS_STATIC_IMPL
int aws_array_list_push_front(struct aws_array_list *AWS_RESTRICT list, const void *val) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_READABLE(val, list->item_size),
        "Input pointer [val] must point writable memory of [list->item_size] bytes.");
    size_t orig_len = aws_array_list_length(list);
    int err_code = aws_array_list_ensure_capacity(list, orig_len);

    if (err_code && aws_last_error() == AWS_ERROR_INVALID_INDEX && !list->alloc) {
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return aws_raise_error(AWS_ERROR_LIST_EXCEEDS_MAX_SIZE);
    } else if (err_code) {
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return err_code;
    }
    if (orig_len) {
        memmove((uint8_t *)list->data + list->item_size, list->data, orig_len * list->item_size);
    }
    ++list->length;
    memcpy(list->data, val, list->item_size);

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return err_code;
}

AWS_STATIC_IMPL
int aws_array_list_pop_front(struct aws_array_list *AWS_RESTRICT list) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    if (aws_array_list_length(list) > 0) {
        aws_array_list_pop_front_n(list, 1);
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_LIST_EMPTY);
}

AWS_STATIC_IMPL
void aws_array_list_pop_front_n(struct aws_array_list *AWS_RESTRICT list, size_t n) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    if (n >= aws_array_list_length(list)) {
        aws_array_list_clear(list);
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return;
    }

    if (n > 0) {
        size_t popping_bytes = list->item_size * n;
        size_t remaining_items = aws_array_list_length(list) - n;
        size_t remaining_bytes = remaining_items * list->item_size;
        memmove(list->data, (uint8_t *)list->data + popping_bytes, remaining_bytes);
        list->length = remaining_items;
#ifdef DEBUG_BUILD
        memset((uint8_t *)list->data + remaining_bytes, AWS_ARRAY_LIST_DEBUG_FILL, popping_bytes);
#endif
    }
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
}

int aws_array_list_erase(struct aws_array_list *AWS_RESTRICT list, size_t index) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));

    const size_t length = aws_array_list_length(list);

    if (index >= length) {
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return aws_raise_error(AWS_ERROR_INVALID_INDEX);
    }

    if (index == 0) {
        /* Removing front element */
        aws_array_list_pop_front(list);
    } else if (index == (length - 1)) {
        /* Removing back element */
        aws_array_list_pop_back(list);
    } else {
        /* Removing middle element */
        uint8_t *item_ptr = (uint8_t *)list->data + (index * list->item_size);
        uint8_t *next_item_ptr = item_ptr + list->item_size;
        size_t trailing_items = (length - index) - 1;
        size_t trailing_bytes = trailing_items * list->item_size;
        memmove(item_ptr, next_item_ptr, trailing_bytes);

        aws_array_list_pop_back(list);
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return AWS_OP_SUCCESS;
}

AWS_STATIC_IMPL
int aws_array_list_back(const struct aws_array_list *AWS_RESTRICT list, void *val) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_WRITABLE(val, list->item_size),
        "Input pointer [val] must point writable memory of [list->item_size] bytes.");
    if (aws_array_list_length(list) > 0) {
        size_t last_item_offset = list->item_size * (aws_array_list_length(list) - 1);

        memcpy(val, (void *)((uint8_t *)list->data + last_item_offset), list->item_size);
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_LIST_EMPTY);
}

AWS_STATIC_IMPL
int aws_array_list_pop_back(struct aws_array_list *AWS_RESTRICT list) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    if (aws_array_list_length(list) > 0) {

        AWS_FATAL_PRECONDITION(list->data);

        size_t last_item_offset = list->item_size * (aws_array_list_length(list) - 1);

        memset((void *)((uint8_t *)list->data + last_item_offset), 0, list->item_size);
        list->length--;
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_LIST_EMPTY);
}

AWS_STATIC_IMPL
void aws_array_list_clear(struct aws_array_list *AWS_RESTRICT list) {
    AWS_PRECONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
    if (list->data) {
#ifdef DEBUG_BUILD
        memset(list->data, AWS_ARRAY_LIST_DEBUG_FILL, list->current_size);
#endif
        list->length = 0;
    }
    AWS_POSTCONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
}

AWS_STATIC_IMPL
void aws_array_list_swap_contents(
    struct aws_array_list *AWS_RESTRICT list_a,
    struct aws_array_list *AWS_RESTRICT list_b) {
    AWS_FATAL_PRECONDITION(list_a->alloc);
    AWS_FATAL_PRECONDITION(list_a->alloc == list_b->alloc);
    AWS_FATAL_PRECONDITION(list_a->item_size == list_b->item_size);
    AWS_FATAL_PRECONDITION(list_a != list_b);
    AWS_PRECONDITION(aws_array_list_is_valid(list_a));
    AWS_PRECONDITION(aws_array_list_is_valid(list_b));

    struct aws_array_list tmp = *list_a;
    *list_a = *list_b;
    *list_b = tmp;
    AWS_POSTCONDITION(aws_array_list_is_valid(list_a));
    AWS_POSTCONDITION(aws_array_list_is_valid(list_b));
}

AWS_STATIC_IMPL
size_t aws_array_list_capacity(const struct aws_array_list *AWS_RESTRICT list) {
    AWS_FATAL_PRECONDITION(list->item_size);
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    size_t capacity = list->current_size / list->item_size;
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return capacity;
}

AWS_STATIC_IMPL
size_t aws_array_list_length(const struct aws_array_list *AWS_RESTRICT list) {
    /*
     * This assert teaches clang-tidy and friends that list->data cannot be null in a non-empty
     * list.
     */
    AWS_FATAL_PRECONDITION(!list->length || list->data);
    AWS_PRECONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
    size_t len = list->length;
    AWS_POSTCONDITION(AWS_IS_ZEROED(*list) || aws_array_list_is_valid(list));
    return len;
}

AWS_STATIC_IMPL
int aws_array_list_get_at(const struct aws_array_list *AWS_RESTRICT list, void *val, size_t index) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_WRITABLE(val, list->item_size),
        "Input pointer [val] must point writable memory of [list->item_size] bytes.");
    if (aws_array_list_length(list) > index) {
        memcpy(val, (void *)((uint8_t *)list->data + (list->item_size * index)), list->item_size);
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_INVALID_INDEX);
}

AWS_STATIC_IMPL
int aws_array_list_get_at_ptr(const struct aws_array_list *AWS_RESTRICT list, void **val, size_t index) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(val != NULL);
    if (aws_array_list_length(list) > index) {
        *val = (void *)((uint8_t *)list->data + (list->item_size * index));
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_SUCCESS;
    }
    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return aws_raise_error(AWS_ERROR_INVALID_INDEX);
}

AWS_STATIC_IMPL
int aws_array_list_set_at(struct aws_array_list *AWS_RESTRICT list, const void *val, size_t index) {
    AWS_PRECONDITION(aws_array_list_is_valid(list));
    AWS_PRECONDITION(
        val && AWS_MEM_IS_READABLE(val, list->item_size),
        "Input pointer [val] must point readable memory of [list->item_size] bytes.");

    if (aws_array_list_ensure_capacity(list, index)) {
        AWS_POSTCONDITION(aws_array_list_is_valid(list));
        return AWS_OP_ERR;
    }

    AWS_FATAL_PRECONDITION(list->data);

    memcpy((void *)((uint8_t *)list->data + (list->item_size * index)), val, list->item_size);

    /*
     * This isn't perfect, but its the best I can come up with for detecting
     * length changes.
     */
    if (index >= aws_array_list_length(list)) {
        if (aws_add_size_checked(index, 1, &list->length)) {
            AWS_POSTCONDITION(aws_array_list_is_valid(list));
            return AWS_OP_ERR;
        }
    }

    AWS_POSTCONDITION(aws_array_list_is_valid(list));
    return AWS_OP_SUCCESS;
}

AWS_EXTERN_C_END

#endif /*  AWS_COMMON_ARRAY_LIST_INL */
