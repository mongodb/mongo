/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing byte integers to 1-bit integers or indexes in BE format
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_bits_be_nu
 *          - @ref qplc_pack_index_be_8u16u
 *          - @ref qplc_pack_index_be_8u32u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_api.h"

OWN_QPLC_FUN(qplc_status_t, qplc_pack_bits_be_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t start_bit,
        uint32_t *pack_bits_index_ptr)) {
    uint8_t  *dst_ptr  = (0u != start_bit) ? *pp_dst - 1u : *pp_dst;
    uint32_t bit_width = own_get_bit_width_from_index(*pack_bits_index_ptr - OWN_QPLC_PACK_BE_INDEX_SHIFT);

    (*qplc_pack_bits_array[*pack_bits_index_ptr])(src_ptr, num_elements, dst_ptr, start_bit);
    *pp_dst += OWN_BITS_2_BYTE(num_elements * bit_width - ((OWN_BYTE_WIDTH - start_bit) & OWN_BYTE_BIT_MASK));

    return QPLC_STS_OK;
}

OWN_QPLC_FUN(qplc_status_t, qplc_pack_index_be_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr)) {
    uint32_t      index    = *index_ptr;
    qplc_status_t status   = QPLC_STS_OK;
    uint16_t      *dst_ptr = (uint16_t *) *pp_dst;
    uint16_t      *end_ptr = (uint16_t *) ((uint8_t *) dst_ptr + dst_length);

    for (uint32_t i = 0u; i < num_elements; i++) {
        if (0u < src_ptr[i]) {
            if (OWN_MAX_16U < index) {
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                return status;
            }
            if (dst_ptr >= end_ptr) {
                status = QPLC_STS_DST_IS_SHORT_ERR;
                return status;
            }
            *dst_ptr = qplc_swap_bytes_16u((uint16_t) index);
            dst_ptr++;
        }
        index++;
    }
    *pp_dst         = (uint8_t *) dst_ptr;
    *index_ptr      = index;
    return status;
}

OWN_QPLC_FUN(qplc_status_t, qplc_pack_index_be_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr)) {
    uint64_t      index    = (uint64_t) *index_ptr;
    qplc_status_t status   = QPLC_STS_OK;
    uint32_t      *dst_ptr = (uint32_t *) *pp_dst;
    uint32_t      *end_ptr = (uint32_t *) ((uint8_t *) dst_ptr + dst_length);

    for (uint32_t i = 0u; i < num_elements; i++) {
        if (0u < src_ptr[i]) {
            if (OWN_MAX_32U < index) {
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                return status;
            }
            if (dst_ptr >= end_ptr) {
                status = QPLC_STS_DST_IS_SHORT_ERR;
                return status;
            }
            *dst_ptr = qplc_swap_bytes_32u((uint32_t) index);
            dst_ptr++;
        }
        index++;
    }
    *pp_dst         = (uint8_t *) dst_ptr;
    *index_ptr      = (uint32_t) index;
    return status;
}
