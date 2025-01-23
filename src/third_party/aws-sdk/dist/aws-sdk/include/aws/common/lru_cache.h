#ifndef AWS_COMMON_LRU_CACHE_H
#define AWS_COMMON_LRU_CACHE_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/cache.h>

AWS_PUSH_SANE_WARNING_LEVEL
AWS_EXTERN_C_BEGIN

/**
 * Initializes the Least-recently-used cache. Sets up the underlying linked hash table.
 * Once `max_items` elements have been added, the least recently used item will be removed. For the other parameters,
 * see aws/common/hash_table.h. Hash table semantics of these arguments are preserved.(Yes the one that was the answer
 * to that interview question that one time).
 */
AWS_COMMON_API
struct aws_cache *aws_cache_new_lru(
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_key_fn,
    aws_hash_callback_destroy_fn *destroy_value_fn,
    size_t max_items);

/**
 * Accesses the least-recently-used element, sets it to most-recently-used
 * element, and returns the value.
 */
AWS_COMMON_API
void *aws_lru_cache_use_lru_element(struct aws_cache *cache);

/**
 * Accesses the most-recently-used element and returns its value.
 */
AWS_COMMON_API
void *aws_lru_cache_get_mru_element(const struct aws_cache *cache);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_LRU_CACHE_H */
