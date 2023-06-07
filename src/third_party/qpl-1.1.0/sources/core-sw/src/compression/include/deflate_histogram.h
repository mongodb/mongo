/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Core API (private C++ API)
 */

#ifndef QPL_DEFLATE_HISTOGRAM_H_
#define QPL_DEFLATE_HISTOGRAM_H_

#include "deflate_defs.h"
#include "qplc_compression_consts.h"
#include "deflate_hash_table.h"
#include "own_qplc_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct isal_huff_histogram isal_histogram;

/**
 * @brief Internal structure that is used for statistics gathering in deflate process
 */
typedef struct {
    uint32_t             literals_matches[QPLC_DEFLATE_LL_TABLE_SIZE]; /**< Literals and matches statistics */
    uint32_t             offsets[QPLC_DEFLATE_D_TABLE_SIZE];           /**< Offsets statistics */
    deflate_hash_table_t table;                                        /**< Hash-table for match searching */
} deflate_histogram_t;

typedef void (*qplc_deflate_histogram_reset_ptr) (deflate_histogram_t *const histogram_ptr);

/**
 * @brief Resets @link own_deflate_histogram @endlink fields values to default ones
 *
 * @param[out]  histogram_ptr  pointer to @link own_deflate_histogram @endlink that should be reset
 */
OWN_QPLC_API(void, deflate_histogram_reset, (deflate_histogram_t *const histogram_ptr));

/**
 * @brief Updates statistics for given match
 *
 * @param[in,out]  histogram_ptr  pointer to @link own_deflate_histogram @endlink where statistics should be stored
 * @param[in]      match          information about found match
 */
void deflate_histogram_update_match(deflate_histogram_t *const histogram_ptr, const deflate_match_t match);

/**
 * @brief Fills deflate statistics histogram from given @link own_deflate_histogram @endlink
 *
 * @note According to rfc1951 literals/lengths and offsets histograms should have 286 and 30 elements.
 *
 * @param[out]   literal_length_histogram_ptr  Pointer to literals/lengths histogram
 * @param[out]   offsets_histogram_ptr        Pointer to offsets histogram
 * @param[in]    deflate_histogram_ptr        Pointer to filled deflate histogram
 *
 * @return this function doesn't return anything
 */
void deflate_histogram_get_statistics(const deflate_histogram_t *deflate_histogram_ptr,
                                      uint32_t *literal_length_histogram_ptr,
                                      uint32_t *offsets_histogram_ptr);

/**
 * @brief Fills @link own_deflate_histogram @endlink from given deflate statistics
 *
 * @note According to rfc1951 literals/lengths and offsets histograms should have 286 and 30 elements.
 *
 * @param[out]  deflate_histogram_ptr         Pointer to deflate histogram
 * @param[in]   literal_length_histogram_ptr  Pointer to filled literals/lengths histogram
 * @param[in]   offsets_histogram_ptr         Pointer to filled offsets histogram
 *
 * @return this function doesn't return anything
 */
void deflate_histogram_set_statistics(deflate_histogram_t *deflate_histogram_ptr,
                                      const uint32_t *literal_length_histogram_ptr,
                                      const uint32_t *offsets_histogram_ptr);

#ifdef __cplusplus
}
#endif

#endif //QPL_DEFLATE_HISTOGRAM_H_
