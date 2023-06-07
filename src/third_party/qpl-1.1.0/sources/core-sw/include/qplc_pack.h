/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_pack.h -------*/

/**
 * @date 07/06/2020
 *
 * @defgroup SW_KERNELS_PACK_API Pack API
 * @ingroup SW_KERNELS_PRIVATE_API
 * @{
 *
 * @brief Contains Contains Intel® Query Processing Library (Intel® QPL) Core API for packing results of filter operation
 * to required output format - nominal bit array, array of integers, or to indexes.
 *
 * @details Core pack APIs implement the following functionalities:
 *      -   Packing kernels for 8u, 16u and 32u input data and 1..32u output data;
 *      -   Packing kernels for 8u, 16u and 32u input data and 1..32u output data in BE format;
 *      -   Packing kernels for 8u input data and index output data in 8u, 16u or 32u representation;
 *      -   Packing kernels for 8u input data and index output data in 8u, 16u or 32u representation in BE format.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_PACK_API_H_
#define QPLC_PACK_API_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qplc_pack_bits_t_ptr)(const uint8_t *src_ptr,
                                     uint32_t num_elements,
                                     uint8_t *dst_ptr,
                                     uint32_t start_bit);

typedef qplc_status_t (*qplc_pack_vector_t_ptr)(const uint8_t *src_ptr,
                                                uint32_t num_elements,
                                                uint8_t **dst_ptr,
                                                uint32_t start_bit_or_dst_length,
                                                uint32_t *index_ptr);

typedef qplc_status_t (*qplc_pack_index_t_ptr)(const uint8_t *src_ptr,
                                               uint32_t num_elements,
                                               uint8_t **pp_dst,
                                               uint32_t dst_length,
                                               uint32_t *index_ptr);


/**
 * @name qplc_pack_<byte order><input bit-width><output bit-width>
 *
 * @brief Packing input data in 8u, 16u or 32u integers format to integers of any-bit-width, LE or BE.
 *
 * @param[in]     src_ptr        pointer to source vector in 8u, 16u or 32u integers format
 * @param[in]     num_elements number of source integers to pack
 * @param[out]    dst_ptr        pointer to packed data in any-bit-width format (LE or BE)
 * @param[in]     start_bit    bit position in the first byte of destination to start from
 *
 * @note Pack function table contains 70 (2 * 35) entries - starts from 1-32 bit-width for LE + [8u16u|8u32u|16u32u],
 *       then the same for BE output
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_pack_8u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u2u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u3u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u4u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u5u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u6u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u7u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u9u, (const uint8_t *src_ptr,
        uint32_t num_elements, uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u10u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u11u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u12u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u13u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u14u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u15u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u17u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u18u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u19u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u20u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u21u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u22u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u23u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u24u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u25u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u26u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u27u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u28u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u29u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u30u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u31u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_16u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u2u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u3u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u4u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u5u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u6u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u7u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u9u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u10u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u11u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u12u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u13u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u14u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u15u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u17u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u18u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u19u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u20u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u21u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u22u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u23u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u24u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u25u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u26u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u27u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u28u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u29u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u30u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u31u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))

OWN_QPLC_API(void, qplc_pack_be_16u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit))
/** @} */

/**
 * @name qplc_pack_index_<byte order><input bit-width><output bit-width>
 * @brief Packing input data in 8u format to nominal bit vector or indexes of 8u, 16u or 32u size, LE or BE.
 *
 * @param[in]   src_ptr       pointer to source 8u vector
 * @param[in]   num_elements  number of 8u source bytes to pack
 * @param[out]  dst_ptr       pointer to packed data in nominal bit vector or index format
 * @param[in]   start_bit     bit position in the first destination byte to start from (if nominal bit vector), or
 *              dst_length    number of bytes available for destination
 * @param[out]  index_ptr     pointer to index to start from for index format; not used for nominal bit vector output.
 *
 * @note Parameters are a bit different for nominal bit vector and index output
 *
 * @return
 *      - @ref QPLC_STS_OK;
 *      - @ref QPLC_STS_OUTPUT_OVERFLOW_ERR;
 *      - @ref QPLC_STS_DST_IS_SHORT_ERR.
 *
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_pack_bits_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **dst_ptr,
        uint32_t start_bit,
        uint32_t *pack_index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_index_8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_index_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_index_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_bits_be_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **dst_ptr,
        uint32_t start_bit,
        uint32_t *pack_index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_index_be_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr))

OWN_QPLC_API(qplc_status_t, qplc_pack_index_be_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t **pp_dst,
        uint32_t dst_length,
        uint32_t *index_ptr))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_PACK_API_H_
/** @}*/
