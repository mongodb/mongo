/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file own_deflate_hash_table.h
 * @brief service functions for @link own_deflate_hash_table @endlink internal structure
 */

#ifndef QPL_PROJECT_OWN_DEFLATE_HASH_TABLE_H
#define QPL_PROJECT_OWN_DEFLATE_HASH_TABLE_H

#include "stdint.h"
#include "qplc_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OWN_HIGH_HASH_TABLE_SIZE 4096u

/* ------ Internal types ------ */

/**
 * @brief Internal structure to organise work with hash-table during deflate process
 */
typedef struct {
    uint32_t *hash_table_ptr;    /**< Pointer to the main hash-table */
    uint32_t *hash_story_ptr;    /**< Pointer to sub-hash-table that stores history of matches */
    uint32_t hash_mask;          /**< Bit-mask that is used to cropping hash-values to not overflow the table */
    uint32_t attempts;           /**< Number of attempts to find a better match in sub-hash-table */
    uint32_t good_match;         /**< Length of the match that stops searching when reached */
    uint32_t nice_match;         /**< Stop searching when match length is longer or equal to this */
    uint32_t lazy_match;         /**< Continue search until new match length is less or equal to the previous*/
} deflate_hash_table_t;

/* ------ Own functions API ------ */

typedef void (*qplc_deflate_hash_table_reset_ptr) (deflate_hash_table_t *const histogram_ptr);

/**
 * @brief Sets @link own_deflate_hash_table @endlink into initial state where nothing was processed yet
 *
 * @param[in,out]  hash_table_ptr  pointer to @link own_deflate_hash_table @endlink that should be reset
 */
OWN_QPLC_API(void, deflate_hash_table_reset, (deflate_hash_table_t *const hash_table_ptr))

/**
 * @brief Updates hash-table with using given index and hash value
 *
 * @param[in,out]  hash_table_ptr  pointer to @link own_deflate_hash_table @endlink that should be updated
 * @param[in]      new_index       index of the bytes with given hash value
 * @param[in]      hash_value      hash value of some bytes
 */
void own_deflate_hash_table_update(deflate_hash_table_t *const hash_table_ptr,
                                   const uint32_t new_index,
                                   const uint32_t hash_value);

#ifdef __cplusplus
}
#endif

#endif // QPL_PROJECT_OWN_DEFLATE_HASH_TABLE_H
