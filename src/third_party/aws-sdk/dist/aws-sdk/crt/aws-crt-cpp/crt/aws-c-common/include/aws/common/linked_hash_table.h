#ifndef AWS_COMMON_LINKED_HASH_TABLE_H
#define AWS_COMMON_LINKED_HASH_TABLE_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/hash_table.h>
#include <aws/common/linked_list.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Simple linked hash table. Preserves insertion order, and can be iterated in insertion order.
 *
 * You can also change the order safely without altering the shape of the underlying hash table.
 */
struct aws_linked_hash_table {
    struct aws_allocator *allocator;
    struct aws_linked_list list;
    struct aws_hash_table table;
    aws_hash_callback_destroy_fn *user_on_value_destroy;
    aws_hash_callback_destroy_fn *user_on_key_destroy;
};

/**
 * Linked-List node stored in the table. This is the node type that will be returned in
 * aws_linked_hash_table_get_iteration_list().
 */
struct aws_linked_hash_table_node {
    struct aws_linked_list_node node;
    struct aws_linked_hash_table *table;
    const void *key;
    void *value;
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes the table. Sets up the underlying hash table and linked list.
 * For the other parameters, see aws/common/hash_table.h. Hash table
 * semantics of these arguments are preserved.
 */
AWS_COMMON_API
int aws_linked_hash_table_init(
    struct aws_linked_hash_table *table,
    struct aws_allocator *allocator,
    aws_hash_fn *hash_fn,
    aws_hash_callback_eq_fn *equals_fn,
    aws_hash_callback_destroy_fn *destroy_key_fn,
    aws_hash_callback_destroy_fn *destroy_value_fn,
    size_t initial_item_count);

/**
 * Cleans up the table. Elements in the table will be evicted and cleanup
 * callbacks will be invoked.
 */
AWS_COMMON_API
void aws_linked_hash_table_clean_up(struct aws_linked_hash_table *table);

/**
 * Finds element in the table by key. If found, AWS_OP_SUCCESS will be
 * returned. If not found, AWS_OP_SUCCESS will be returned and *p_value will be
 * NULL.
 *
 * If any errors occur AWS_OP_ERR will be returned.
 */
AWS_COMMON_API
int aws_linked_hash_table_find(struct aws_linked_hash_table *table, const void *key, void **p_value);

/**
 * Finds element in the table by key. If found, AWS_OP_SUCCESS will be returned and the item will be moved to the back
 * of the list.
 * If not found, AWS_OP_SUCCESS will be returned and *p_value will be NULL.
 *
 * Note: this will change the order of elements
 */
AWS_COMMON_API
int aws_linked_hash_table_find_and_move_to_back(struct aws_linked_hash_table *table, const void *key, void **p_value);

/**
 * Puts `p_value` at `key`. If an element is already stored at `key` it will be replaced.
 */
AWS_COMMON_API
int aws_linked_hash_table_put(struct aws_linked_hash_table *table, const void *key, void *p_value);

/**
 * Removes item at `key` from the table.
 */
AWS_COMMON_API
int aws_linked_hash_table_remove(struct aws_linked_hash_table *table, const void *key);

/**
 * Clears all items from the table.
 */
AWS_COMMON_API
void aws_linked_hash_table_clear(struct aws_linked_hash_table *table);

/**
 * returns number of elements in the table.
 */
AWS_COMMON_API
size_t aws_linked_hash_table_get_element_count(const struct aws_linked_hash_table *table);

/**
 * Move the aws_linked_hash_table_node to the end of the list.
 *
 * Note: this will change the order of elements
 */
AWS_COMMON_API
void aws_linked_hash_table_move_node_to_end_of_list(
    struct aws_linked_hash_table *table,
    struct aws_linked_hash_table_node *node);

/**
 * returns the underlying linked list for iteration.
 *
 * The returned list has nodes of the type: aws_linked_hash_table_node. Use AWS_CONTAINER_OF for access to the element.
 */
AWS_COMMON_API
const struct aws_linked_list *aws_linked_hash_table_get_iteration_list(const struct aws_linked_hash_table *table);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_LINKED_HASH_TABLE_H */
