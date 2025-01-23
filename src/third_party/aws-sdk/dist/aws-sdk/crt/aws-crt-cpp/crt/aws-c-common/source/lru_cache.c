/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/lru_cache.h>
static int s_lru_cache_put(struct aws_cache *cache, const void *key, void *p_value);
static int s_lru_cache_find(struct aws_cache *cache, const void *key, void **p_value);
static void *s_lru_cache_use_lru_element(struct aws_cache *cache);
static void *s_lru_cache_get_mru_element(const struct aws_cache *cache);

struct lru_cache_impl_vtable {
    void *(*use_lru_element)(struct aws_cache *cache);
    void *(*get_mru_element)(const struct aws_cache *cache);
};

static struct aws_cache_vtable s_lru_cache_vtable = {
    .destroy = aws_cache_base_default_destroy,
    .find = s_lru_cache_find,
    .put = s_lru_cache_put,
    .remove = aws_cache_base_default_remove,
    .clear = aws_cache_base_default_clear,
    .get_element_count = aws_cache_base_default_get_element_count,
};

struct aws_cache *aws_cache_new_lru(
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_key_fn,
    aws_hash_callback_destroy_fn *destroy_value_fn,
    size_t max_items) {
    AWS_ASSERT(allocator);
    AWS_ASSERT(max_items);
    struct aws_cache *lru_cache = NULL;
    struct lru_cache_impl_vtable *impl = NULL;

    if (!aws_mem_acquire_many(
            allocator, 2, &lru_cache, sizeof(struct aws_cache), &impl, sizeof(struct lru_cache_impl_vtable))) {
        return NULL;
    }
    impl->use_lru_element = s_lru_cache_use_lru_element;
    impl->get_mru_element = s_lru_cache_get_mru_element;
    lru_cache->allocator = allocator;
    lru_cache->max_items = max_items;
    lru_cache->vtable = &s_lru_cache_vtable;
    lru_cache->impl = impl;
    if (aws_linked_hash_table_init(
            &lru_cache->table, allocator, hash_fn, equals_fn, destroy_key_fn, destroy_value_fn, max_items)) {
        return NULL;
    }
    return lru_cache;
}

/* implementation for lru cache put */
static int s_lru_cache_put(struct aws_cache *cache, const void *key, void *p_value) {

    if (aws_linked_hash_table_put(&cache->table, key, p_value)) {
        return AWS_OP_ERR;
    }

    /* Manage the space if we actually added a new element and the cache is full. */
    if (aws_linked_hash_table_get_element_count(&cache->table) > cache->max_items) {
        /* we're over the cache size limit. Remove whatever is in the front of
         * the linked_hash_table, which is the LRU element */
        const struct aws_linked_list *list = aws_linked_hash_table_get_iteration_list(&cache->table);
        struct aws_linked_list_node *node = aws_linked_list_front(list);
        struct aws_linked_hash_table_node *table_node = AWS_CONTAINER_OF(node, struct aws_linked_hash_table_node, node);
        return aws_linked_hash_table_remove(&cache->table, table_node->key);
    }

    return AWS_OP_SUCCESS;
}
/* implementation for lru cache find */
static int s_lru_cache_find(struct aws_cache *cache, const void *key, void **p_value) {
    return (aws_linked_hash_table_find_and_move_to_back(&cache->table, key, p_value));
}

static void *s_lru_cache_use_lru_element(struct aws_cache *cache) {
    const struct aws_linked_list *list = aws_linked_hash_table_get_iteration_list(&cache->table);
    if (aws_linked_list_empty(list)) {
        return NULL;
    }
    struct aws_linked_list_node *node = aws_linked_list_front(list);
    struct aws_linked_hash_table_node *lru_node = AWS_CONTAINER_OF(node, struct aws_linked_hash_table_node, node);

    aws_linked_hash_table_move_node_to_end_of_list(&cache->table, lru_node);
    return lru_node->value;
}
static void *s_lru_cache_get_mru_element(const struct aws_cache *cache) {
    const struct aws_linked_list *list = aws_linked_hash_table_get_iteration_list(&cache->table);
    if (aws_linked_list_empty(list)) {
        return NULL;
    }
    struct aws_linked_list_node *node = aws_linked_list_back(list);
    struct aws_linked_hash_table_node *mru_node = AWS_CONTAINER_OF(node, struct aws_linked_hash_table_node, node);
    return mru_node->value;
}

void *aws_lru_cache_use_lru_element(struct aws_cache *cache) {
    AWS_PRECONDITION(cache);
    AWS_PRECONDITION(cache->impl);
    struct lru_cache_impl_vtable *impl_vtable = cache->impl;
    return impl_vtable->use_lru_element(cache);
}

void *aws_lru_cache_get_mru_element(const struct aws_cache *cache) {
    AWS_PRECONDITION(cache);
    AWS_PRECONDITION(cache->impl);
    struct lru_cache_impl_vtable *impl_vtable = cache->impl;
    return impl_vtable->get_mru_element(cache);
}
