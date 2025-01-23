#ifndef AWS_COMMON_PRIVATE_HASH_TABLE_IMPL_H
#define AWS_COMMON_PRIVATE_HASH_TABLE_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/hash_table.h>
#include <aws/common/math.h>

struct hash_table_entry {
    struct aws_hash_element element;
    uint64_t hash_code; /* hash code (0 signals empty) */
};

/* Using a flexible array member is the C99 compliant way to have the hash_table_entries
 * immediately follow the struct.
 *
 * MSVC doesn't know this for some reason so we need to use a pragma to make
 * it happy.
 */
#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4200)
#endif
struct hash_table_state {
    aws_hash_fn *hash_fn;
    aws_hash_callback_eq_fn *equals_fn;
    aws_hash_callback_destroy_fn *destroy_key_fn;
    aws_hash_callback_destroy_fn *destroy_value_fn;
    struct aws_allocator *alloc;

    size_t size, entry_count;
    size_t max_load;
    /* We AND a hash value with mask to get the slot index */
    size_t mask;
    double max_load_factor;
    /* actually variable length */
    struct hash_table_entry slots[];
};
#ifdef _MSC_VER
#    pragma warning(pop)
#endif

/**
 * Best-effort check of hash_table_state data-structure invariants
 * Some invariants, such as that the number of entries is actually the
 * same as the entry_count field, would require a loop to check
 */
bool hash_table_state_is_valid(const struct hash_table_state *map);

/**
 * Determine the total number of bytes needed for a hash-table with
 * "size" slots. If the result would overflow a size_t, return
 * AWS_OP_ERR; otherwise, return AWS_OP_SUCCESS with the result in
 * "required_bytes".
 */
int hash_table_state_required_bytes(size_t size, size_t *required_bytes);

#endif /* AWS_COMMON_PRIVATE_HASH_TABLE_IMPL_H */
