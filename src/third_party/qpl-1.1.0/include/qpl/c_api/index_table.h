/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_INDEX_TABLE_H_
#define QPL_INDEX_TABLE_H_

#include "qpl/c_api/status.h"
#include "qpl/c_api/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup JOB_API_DEFINITIONS
 * @{
 */

/**
 * @brief Structure for indices representation
 */
typedef struct {
    uint32_t bit_offset;    /**< Starting bit of mini-block */
    uint32_t crc;           /**< Cumulative calculated CRC */
} qpl_index;

/**
 * @brief Structure for indices table representation
 */
typedef struct {
    uint32_t  block_count;              /**< Number of deflate blocks in the table */
    uint32_t  mini_block_count;         /**< Number of mini-blocks in the table */
    uint32_t  mini_blocks_per_block;    /**< Number of mini-blocks in one deflate block */
    qpl_index *indices_ptr;             /**< Array with indices for mini-blocks */
} qpl_index_table;

/** @} */

/**
 * @addtogroup JOB_API_FUNCTIONS
 * @{
 */

/**
 * @brief Gets the number of bytes required for indexing table
 *
 * @param mini_block_count      Number of mini-blocks in the table
 * @param mini_blocks_per_block Number of mini-blocks in one deflate block
 * @param size_ptr              The resulted number of bytes
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_get_index_table_size, (uint32_t mini_block_count,
                                               uint32_t mini_blocks_per_block,
                                               size_t * size_ptr))

/**
 * @brief Sets source pointer to required position for further mini-block decompression
 *
 * @param start_bit             Starting bit in the stream
 * @param last_bit              Final bit in the stream
 * @param source_pptr           Source pointer to modify
 * @param first_bit_offset_ptr  Is set to the first meaningful bit in the byte
 * @param last_bit_offset_ptr   Is set to the last meaningful bit in the byte
 * @param compressed_size_ptr   Is set to the number of bytes used to keep the mini-block compressed
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_set_mini_block_location, (uint32_t start_bit,
                                                  uint32_t last_bit,
                                                  uint8_t * *source_pptr,
                                                  uint32_t * first_bit_offset_ptr,
                                                  uint32_t * last_bit_offset_ptr,
                                                  uint32_t * compressed_size_ptr))

/**
 * @brief Sets appropriate deflate block index
 *
 * @param table_ptr         Pointer to the table with indices
 * @param mini_block_number Index of mini-block that should be decompressed
 * @param block_index_ptr   Is set to an appropriate deflate block index
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_find_header_block_index, (qpl_index_table * table_ptr,
                                                  uint32_t mini_block_number,
                                                  uint32_t * block_index_ptr))

/**
 * @brief Sets appropriate mini-block index
 *
 * @param table_ptr         Pointer to the table with indices
 * @param mini_block_number Index of mini-block that should be decompressed
 * @param block_index_ptr   Is set to an appropriate mini-block index
 *
 * @return
 *     - @ref QPL_STS_OK;
 *     - @ref QPL_STS_NULL_PTR_ERR.
 */
QPL_API(qpl_status, qpl_find_mini_block_index, (qpl_index_table * table_ptr,
                                                uint32_t mini_block_number,
                                                uint32_t * block_index_ptr))

/** @} */

#ifdef __cplusplus
}
#endif

#endif //QPL_INDEX_TABLE_H_
