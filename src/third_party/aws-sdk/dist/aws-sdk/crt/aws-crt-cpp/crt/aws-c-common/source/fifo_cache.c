/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/fifo_cache.h>

static int s_fifo_cache_put(struct aws_cache *cache, const void *key, void *p_value);

static struct aws_cache_vtable s_fifo_cache_vtable = {
    .destroy = aws_cache_base_default_destroy,
    .find = aws_cache_base_default_find,
    .put = s_fifo_cache_put,
    .remove = aws_cache_base_default_remove,
    .clear = aws_cache_base_default_clear,
    .get_element_count = aws_cache_base_default_get_element_count,
};

struct aws_cache *aws_cache_new_fifo(
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_key_fn,
    aws_hash_callback_destroy_fn *destroy_value_fn,
    size_t max_items) {
    AWS_ASSERT(allocator);
    AWS_ASSERT(max_items);

    struct aws_cache *fifo_cache = aws_mem_calloc(allocator, 1, sizeof(struct aws_cache));
    if (!fifo_cache) {
        return NULL;
    }
    fifo_cache->allocator = allocator;
    fifo_cache->max_items = max_items;
    fifo_cache->vtable = &s_fifo_cache_vtable;
    if (aws_linked_hash_table_init(
            &fifo_cache->table, allocator, hash_fn, equals_fn, destroy_key_fn, destroy_value_fn, max_items)) {
        return NULL;
    }
    return fifo_cache;
}

/* fifo cache put implementation */
static int s_fifo_cache_put(struct aws_cache *cache, const void *key, void *p_value) {
    if (aws_linked_hash_table_put(&cache->table, key, p_value)) {
        return AWS_OP_ERR;
    }

    /* Manage the space if we actually added a new element and the cache is full. */
    if (aws_linked_hash_table_get_element_count(&cache->table) > cache->max_items) {
        /* we're over the cache size limit. Remove whatever is in the front of
         * the linked_hash_table, which is the oldest element */
        const struct aws_linked_list *list = aws_linked_hash_table_get_iteration_list(&cache->table);
        struct aws_linked_list_node *node = aws_linked_list_front(list);
        struct aws_linked_hash_table_node *table_node = AWS_CONTAINER_OF(node, struct aws_linked_hash_table_node, node);
        return aws_linked_hash_table_remove(&cache->table, table_node->key);
    }

    return AWS_OP_SUCCESS;
}
