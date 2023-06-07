/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_unpack.h -------*/

/**
 * @date 07/06/2020
 *
* @defgroup SW_KERNELS_UNPACK_API Unpack API
* @ingroup  SW_KERNELS_PRIVATE_API
* @{
 * @brief Contains Contains Intel® Query Processing Library (Intel® QPL) Core API for unpack functionality from different
 * filter operation input formats to integers of byte,
 *        word and dword size
 *
 * @details Core unpack APIs implement the following functionalities:
 *      -   Unpacking n-bit integers' vector to 8u, 16u or 32u integers;
 *      -   Unpacking input data in PRLE format to 8u, 16u or 32u integers;
 *      -   Unpacking n-bit integers' vector in BE format to 8u, 16u or 32u integers.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_UNPACK_API_H_
#define QPLC_UNPACK_API_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qplc_unpack_bits_t_ptr)(const uint8_t *src_ptr,
                                       uint32_t num_elements,
                                       uint32_t start_bit,
                                       uint8_t *dst_ptr);

typedef qplc_status_t (*qplc_unpack_prle_t_ptr)(uint8_t **pp_src,
                                                uint32_t src_length,
                                                uint32_t bit_width,
                                                uint8_t **pp_dst,
                                                uint32_t dst_length,
                                                int32_t *count_ptr,
                                                uint32_t *value_ptr);

/**
 * @name qplc_unpack_<input bit-width><output bit-width>
 *
 * @brief Unpacking input data in format of any-bit-width, LE or BE, to vector of 8u, 16u or 32u integers.
 *
 * @param[in]   src_ptr       pointer to source vector in packed any-bit-width integers format
 * @param[in]   num_elements  number of n-bit integers to unpack
 * @param[in]   start_bit     bit position in the first byte to start from
 * @param[out]  dst_ptr       pointer to unpacked data in 8u, 16u or 32u format (depends on bit width)
 *
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_unpack_1u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_2u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_3u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_4u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_5u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_6u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_7u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_9u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_10u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_11u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_12u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_13u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_14u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_15u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_17u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_18u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_19u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_20u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_21u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_22u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_23u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_24u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_25u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_26u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_27u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_28u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_29u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_30u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_31u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_1u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_2u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_3u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_4u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_5u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_6u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_7u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_9u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_10u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_11u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_12u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_13u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_14u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_15u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_17u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_18u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_19u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_20u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_21u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_22u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_23u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_24u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_25u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_26u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_27u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_28u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_29u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_30u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_31u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))

OWN_QPLC_API(void, qplc_unpack_be_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr))
/** @} */

/**
 * @name qplc_unpack_prle<output bit-width>
 *
 * @brief Unpacking input data in PRLE format to vector of 8u, 16u or 32u integers.
 *
 * @param[in,out] pp_src      double pointer to source vector in PRLE format with +1 shift (bit width is extracted)
 * @param[in]     src_length  length of source vector in bytes
 * @param[in]     bit_width   bit width of packed input data
 * @param[out]    pp_dst      double pointer to unpacked data in 8u, 16u or 32u format (depends on bit width)
 * @param[in]     dst_length  length of destination vector in bytes
 * @param[in,out] count_ptr     pointer to the number of remaining iteration for RLE or literal-octa-groups:
 *                            if > 0 - number of RLE packed values to unpack at the next iteration,
 *                            if dst_length was not enough at the previous one;
 *                            if < 0 - abs(*count_ptr) means a number of literal octets to copy at the next iteration,
 *                            if dst_length was not enough at the previous one;
 *                            if == 0 - common PRLE operation will start at the next iteration.
 * @param[in,out] -value_ptr    pointer to value for non-finished RLE unpacking due to not-enough dst_length.
 *
 * @return
 *      - @ref QPLC_STS_OK;
 *      - @ref QPLC_STS_MORE_OUTPUT_NEEDED;
 *      - @ref QPLC_STS_SRC_IS_SHORT_ERR.
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_unpack_prle_8u, (uint8_t **pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t **pp_dst,
        uint32_t dst_length,
        int32_t *count_ptr,
        uint32_t *value_ptr))

OWN_QPLC_API(qplc_status_t, qplc_unpack_prle_16u, (uint8_t **pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t **pp_dst,
        uint32_t dst_length,
        int32_t *count_ptr,
        uint32_t *value_ptr))

OWN_QPLC_API(qplc_status_t, qplc_unpack_prle_32u, (uint8_t **pp_src,
        uint32_t src_length,
        uint32_t bit_width,
        uint8_t **pp_dst,
        uint32_t dst_length,
        int32_t *count_ptr,
        uint32_t *value_ptr))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_UNPACK_API_H_
/** @} */
