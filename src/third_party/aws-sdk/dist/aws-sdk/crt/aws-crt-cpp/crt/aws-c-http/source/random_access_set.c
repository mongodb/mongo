
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/allocator.h>
#include <aws/common/device_random.h>
#include <aws/http/private/random_access_set.h>

struct aws_random_access_set_impl {
    struct aws_allocator *allocator;
    struct aws_array_list list; /* Always store the pointer of the element. */
    struct aws_hash_table map;  /* Map from the element to the index in the array. */
    aws_hash_callback_destroy_fn *destroy_element_fn;
};

static void s_impl_destroy(struct aws_random_access_set_impl *impl) {
    if (!impl) {
        return;
    }
    aws_array_list_clean_up(&impl->list);
    aws_hash_table_clean_up(&impl->map);
    aws_mem_release(impl->allocator, impl);
}

static struct aws_random_access_set_impl *s_impl_new(
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_element_fn,
    size_t initial_item_allocation) {
    struct aws_random_access_set_impl *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_random_access_set_impl));
    impl->allocator = allocator;
    /* Will always store the pointer of the element. */
    if (aws_array_list_init_dynamic(&impl->list, allocator, initial_item_allocation, sizeof(void *))) {
        s_impl_destroy(impl);
        return NULL;
    }

    if (aws_hash_table_init(
            &impl->map, allocator, initial_item_allocation, hash_fn, equals_fn, destroy_element_fn, NULL)) {
        s_impl_destroy(impl);
        return NULL;
    }
    impl->destroy_element_fn = destroy_element_fn;
    return impl;
}

int aws_random_access_set_init(
    struct aws_random_access_set *set,
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_element_fn,
    size_t initial_item_allocation) {
    AWS_FATAL_PRECONDITION(set);
    AWS_FATAL_PRECONDITION(allocator);
    AWS_FATAL_PRECONDITION(hash_fn);
    AWS_FATAL_PRECONDITION(equals_fn);

    struct aws_random_access_set_impl *impl =
        s_impl_new(allocator, hash_fn, equals_fn, destroy_element_fn, initial_item_allocation);
    if (!impl) {
        return AWS_OP_ERR;
    }
    set->impl = impl;
    return AWS_OP_SUCCESS;
}

void aws_random_access_set_clean_up(struct aws_random_access_set *set) {
    if (!set) {
        return;
    }
    s_impl_destroy(set->impl);
}

int aws_random_access_set_add(struct aws_random_access_set *set, const void *element, bool *added) {
    AWS_PRECONDITION(set);
    AWS_PRECONDITION(element);
    AWS_PRECONDITION(added);
    bool exist = false;
    if (aws_random_access_set_exist(set, element, &exist) || exist) {
        *added = false;
        return AWS_OP_SUCCESS;
    }
    /* deep copy the pointer of element to store at the array list */
    if (aws_array_list_push_back(&set->impl->list, (void *)&element)) {
        goto list_push_error;
    }
    if (aws_hash_table_put(&set->impl->map, element, (void *)(aws_array_list_length(&set->impl->list) - 1), NULL)) {
        goto error;
    }
    *added = true;
    return AWS_OP_SUCCESS;
error:
    aws_array_list_pop_back(&set->impl->list);
list_push_error:
    *added = false;
    return AWS_OP_ERR;
}

int aws_random_access_set_remove(struct aws_random_access_set *set, const void *element) {
    AWS_PRECONDITION(set);
    AWS_PRECONDITION(element);
    size_t current_length = aws_array_list_length(&set->impl->list);
    if (current_length == 0) {
        /* Nothing to remove */
        return AWS_OP_SUCCESS;
    }
    struct aws_hash_element *find = NULL;
    /* find and remove the element from table */
    if (aws_hash_table_find(&set->impl->map, element, &find)) {
        return AWS_OP_ERR;
    }
    if (!find) {
        /* It's removed already */
        return AWS_OP_SUCCESS;
    }

    size_t index_to_remove = (size_t)find->value;
    if (aws_hash_table_remove_element(&set->impl->map, find)) {
        return AWS_OP_ERR;
    }
    /* If assert code failed, we won't be recovered from the failure */
    int assert_re = AWS_OP_SUCCESS;
    (void)assert_re;
    /* Nothing else can fail after here. */
    if (index_to_remove != current_length - 1) {
        /* It's not the last element, we need to swap it with the end of the list and remove the last element */
        void *last_element = NULL;
        /* The last element is a pointer of pointer of element. */
        assert_re = aws_array_list_get_at_ptr(&set->impl->list, &last_element, current_length - 1);
        AWS_ASSERT(assert_re == AWS_OP_SUCCESS);
        /* Update the last element index in the table */
        struct aws_hash_element *element_to_update = NULL;
        assert_re = aws_hash_table_find(&set->impl->map, *(void **)last_element, &element_to_update);
        AWS_ASSERT(assert_re == AWS_OP_SUCCESS);
        AWS_ASSERT(element_to_update != NULL);
        element_to_update->value = (void *)index_to_remove;
        /* Swap the last element with the element to remove in the list */
        aws_array_list_swap(&set->impl->list, index_to_remove, current_length - 1);
    }
    /* Remove the current last element from the list */
    assert_re = aws_array_list_pop_back(&set->impl->list);
    AWS_ASSERT(assert_re == AWS_OP_SUCCESS);
    if (set->impl->destroy_element_fn) {
        set->impl->destroy_element_fn((void *)element);
    }
    return AWS_OP_SUCCESS;
}

int aws_random_access_set_random_get_ptr(const struct aws_random_access_set *set, void **out) {
    AWS_PRECONDITION(set);
    AWS_PRECONDITION(out != NULL);
    size_t length = aws_array_list_length(&set->impl->list);
    if (length == 0) {
        return aws_raise_error(AWS_ERROR_LIST_EMPTY);
    }

    uint64_t random_64_bit_num = 0;
    aws_device_random_u64(&random_64_bit_num);

    size_t index = (size_t)random_64_bit_num % length;
    /* The array list stores the pointer of the element. */
    return aws_array_list_get_at(&set->impl->list, (void *)out, index);
}

size_t aws_random_access_set_get_size(const struct aws_random_access_set *set) {
    return aws_array_list_length(&set->impl->list);
}

int aws_random_access_set_exist(const struct aws_random_access_set *set, const void *element, bool *exist) {
    AWS_PRECONDITION(set);
    AWS_PRECONDITION(element);
    AWS_PRECONDITION(exist);
    struct aws_hash_element *find = NULL;
    int re = aws_hash_table_find(&set->impl->map, element, &find);
    *exist = find != NULL;
    return re;
}

int aws_random_access_set_random_get_ptr_index(const struct aws_random_access_set *set, void **out, size_t index) {
    AWS_PRECONDITION(set);
    AWS_PRECONDITION(out != NULL);
    return aws_array_list_get_at(&set->impl->list, (void *)out, index);
}
