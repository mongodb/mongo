/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_STATISTICS_HPP_
#define QPL_STATISTICS_HPP_

#include "stdint.h"
#include "qpl/c_api/status.h"
#include "qpl/c_api/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup JOB_API_DEFINITIONS
 * @{
 */

#define QPL_LITERALS_MATCHES_TABLE_SIZE    286u  /**< Size of Huffman table with codes for literals and match lengths */
#define QPL_DEFAULT_OFFSETS_NUMBER         30u   /**< Default number of possible offsets in a Huffman table */
#define QPL_DEFAULT_LITERALS_NUMBER        257u  /**< Default number of literals in a Huffman table */

/**
 * @brief Represents mode in which @ref qpl_op_compress operation should be performed
 */
typedef enum {
    qpl_compression_mode = 0,    /**< Perform @ref qpl_op_compress operation in default compression mode */
    qpl_gathering_mode   = 1     /**< Perform @ref qpl_op_compress operation in statistic gathering mode */
} qpl_statistics_mode;

/**
 * @struct qpl_histogram
 * @brief Structure that represents histogram of literals, lengths and offsets symbols
 */
typedef struct {
    uint32_t literal_lengths[QPL_LITERALS_MATCHES_TABLE_SIZE]; /**< Combined histogram for literals and match lengths tokens */
    uint32_t reserved_literal_lengths[2u];                     /**< Reserved match lengths tokens */
    uint32_t distances[QPL_DEFAULT_OFFSETS_NUMBER];            /**< Histogram for distance tokens */
    uint32_t reserved_distances[2u];                           /**< Reserved distance tokens */
} qpl_histogram;

/** @} */

/**
 * @addtogroup JOB_API_FUNCTIONS
 * @{
 */

/**
 * @brief Gathers deflate statistics (literals/lengths and offsets histogram)
 *
 * @param[in]   source_ptr     Pointer to source vector that should be processed
 * @param[in]   source_length  Source vector length
 * @param[out]  histogram_ptr  Pointer to histogram to be updated
 * @param[in]   level          Level of compression algorithm
 * @param[in]   path           Execution path
 *
 * @return One of statuses presented in the @ref qpl_status
 */
QPL_API(qpl_status, qpl_gather_deflate_statistics, (uint8_t * source_ptr,
                                                    const uint32_t source_length,
                                                    qpl_histogram *histogram_ptr,
                                                    const qpl_compression_levels level,
                                                    const qpl_path_t path))

/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_STATISTICS_HPP_
