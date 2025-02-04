#ifndef AWS_COMMON_CACHE_H
#define AWS_COMMON_CACHE_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/linked_hash_table.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_cache;

struct aws_cache_vtable {
    void (*destroy)(struct aws_cache *cache);
    int (*find)(struct aws_cache *cache, const void *key, void **p_value);
    int (*put)(struct aws_cache *cache, const void *key, void *p_value);
    int (*remove)(struct aws_cache *cache, const void *key);
    void (*clear)(struct aws_cache *cache);
    size_t (*get_element_count)(const struct aws_cache *cache);
};

/**
 * Base stucture for caches, used the linked hash table implementation.
 */
struct aws_cache {
    struct aws_allocator *allocator;
    const struct aws_cache_vtable *vtable;
    struct aws_linked_hash_table table;
    size_t max_items;

    void *impl;
};

/* Default implementations */
void aws_cache_base_default_destroy(struct aws_cache *cache);
int aws_cache_base_default_find(struct aws_cache *cache, const void *key, void **p_value);
int aws_cache_base_default_remove(struct aws_cache *cache, const void *key);
void aws_cache_base_default_clear(struct aws_cache *cache);
size_t aws_cache_base_default_get_element_count(const struct aws_cache *cache);

AWS_EXTERN_C_BEGIN
/**
 * Cleans up the cache. Elements in the cache will be evicted and cleanup
 * callbacks will be invoked.
 */
AWS_COMMON_API
void aws_cache_destroy(struct aws_cache *cache);

/**
 * Finds element in the cache by key. If found, *p_value will hold the stored value, and AWS_OP_SUCCESS will be
 * returned. If not found, AWS_OP_SUCCESS will be returned and *p_value will be NULL.
 *
 * If any errors occur AWS_OP_ERR will be returned.
 */
AWS_COMMON_API
int aws_cache_find(struct aws_cache *cache, const void *key, void **p_value);

/**
 * Puts `p_value` at `key`. If an element is already stored at `key` it will be replaced. If the cache is already full,
 * an item will be removed based on the cache policy.
 */
AWS_COMMON_API
int aws_cache_put(struct aws_cache *cache, const void *key, void *p_value);

/**
 * Removes item at `key` from the cache.
 */
AWS_COMMON_API
int aws_cache_remove(struct aws_cache *cache, const void *key);

/**
 * Clears all items from the cache.
 */
AWS_COMMON_API
void aws_cache_clear(struct aws_cache *cache);

/**
 * Returns the number of elements in the cache.
 */
AWS_COMMON_API
size_t aws_cache_get_element_count(const struct aws_cache *cache);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_CACHE_H */
