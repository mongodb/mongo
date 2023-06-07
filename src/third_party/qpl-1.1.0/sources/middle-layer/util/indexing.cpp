/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "qpl/qpl.h"
#include "../c_api/own_defs.h"

qpl_status qpl_get_index_table_size(uint32_t mini_block_count,
                                    uint32_t mini_blocks_per_block,
                                    size_t *size_ptr) {
    QPL_BAD_PTR_RET(size_ptr);
    if (mini_blocks_per_block == 0) {
        QPL_ERROR_RET(QPL_STS_SIZE_ERR);
    }

    uint32_t block_count = (mini_block_count + mini_blocks_per_block - 1u) / mini_blocks_per_block;
    *size_ptr = (block_count * 2u + mini_block_count + 1u) * sizeof(uint64_t);

    return QPL_STS_OK;
}

qpl_status qpl_set_mini_block_location(uint32_t start_bit,
                                       uint32_t last_bit,
                                       uint8_t **source_pptr,
                                       uint32_t *first_bit_offset_ptr,
                                       uint32_t *last_bit_offset_ptr,
                                       uint32_t *compressed_size_ptr) {
    QPL_BAD_PTR2_RET(source_pptr, *source_pptr);
    QPL_BAD_PTR_RET(first_bit_offset_ptr);
    QPL_BAD_PTR_RET(last_bit_offset_ptr);
    QPL_BAD_PTR_RET(compressed_size_ptr);
    if (start_bit > last_bit) {
        QPL_ERROR_RET(QPL_STS_INVALID_PARAM_ERR);
    }

    *first_bit_offset_ptr = start_bit & 7;
    *last_bit_offset_ptr = 7 & (0 - last_bit);
    *compressed_size_ptr = ((last_bit + 7) / 8) - (start_bit / 8);
    *source_pptr += start_bit / 8;

    return QPL_STS_OK;
}

qpl_status qpl_find_header_block_index(qpl_index_table *table_ptr,
                                       uint32_t mini_block_number,
                                       uint32_t *block_index_ptr) {
    QPL_BAD_PTR_RET(table_ptr);
    QPL_BAD_PTR_RET(block_index_ptr);
    OWN_RETURN_ERROR(mini_block_number >= table_ptr->mini_block_count, QPL_STS_SIZE_ERR);
    if (table_ptr->mini_blocks_per_block == 0) {
        QPL_ERROR_RET(QPL_STS_SIZE_ERR);
    }

    uint32_t block_number = mini_block_number / table_ptr->mini_blocks_per_block;

    *block_index_ptr = block_number * (table_ptr->mini_blocks_per_block + 2u);

    return QPL_STS_OK;
}

qpl_status qpl_find_mini_block_index(qpl_index_table *table_ptr,
                                     uint32_t mini_block_number,
                                     uint32_t *block_index_ptr) {
    QPL_BAD_PTR_RET(table_ptr);
    QPL_BAD_PTR_RET(block_index_ptr);
    OWN_RETURN_ERROR(mini_block_number >= table_ptr->mini_block_count, QPL_STS_SIZE_ERR);
    if (table_ptr->mini_blocks_per_block == 0) {
        QPL_ERROR_RET(QPL_STS_SIZE_ERR);
    }

    uint32_t current_header_index = 0u;

    qpl_status status =
        qpl_find_header_block_index(table_ptr, mini_block_number, &current_header_index);

    if (status) {
        return status;
    }

    uint32_t block_number = mini_block_number / table_ptr->mini_blocks_per_block;
    uint32_t mini_block_number_in_block =
        mini_block_number - block_number * table_ptr->mini_blocks_per_block;

    *block_index_ptr = current_header_index + 1u + mini_block_number_in_block;

    return QPL_STS_OK;
}
