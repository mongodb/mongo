/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file own_deflate_hash_table.c
 * @brief contains implementation of @link own_deflate_hash_table @endlink service functions
 */

/* ------ Includes ------ */

#include "qplc_memop.h"
#include "deflate_hash_table.h"
#include "own_qplc_defs.h"

/**
 * Values of uninitialized index in hash table
 */
#define OWN_UNINITIALIZED_INDEX 0xFFFFFFFFu
#define OWN_UNINITIALIZED_INDEX_32u 0x80000000u

/**
 * Size of hash table that is used during match searching
 */

/* ------ Own functions implementation ------ */

OWN_QPLC_FUN(void, deflate_hash_table_reset,(deflate_hash_table_t *const hash_table_ptr)) {
    CALL_CORE_FUN(qplc_set_32u)((uint32_t) OWN_UNINITIALIZED_INDEX_32u,
                                (uint32_t *) hash_table_ptr->hash_table_ptr,
                                OWN_HIGH_HASH_TABLE_SIZE);

    CALL_CORE_FUN(qplc_zero_8u)((uint8_t *) hash_table_ptr->hash_story_ptr,
                                OWN_HIGH_HASH_TABLE_SIZE * 4u);
}

#if PLATFORM < K0

void own_deflate_hash_table_update(deflate_hash_table_t *const hash_table_ptr,
                                   const uint32_t new_index,
                                   const uint32_t hash_value) {
    // Variables
    const uint32_t current_index = hash_table_ptr->hash_table_ptr[hash_value];

    // Update hash table
    hash_table_ptr->hash_table_ptr[hash_value] = new_index;

    hash_table_ptr->hash_story_ptr[new_index & hash_table_ptr->hash_mask] = current_index;
}

#endif
