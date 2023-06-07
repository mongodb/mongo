/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_extract.h -------*/

/**
 * @date 07/06/2020
 *
 * @defgroup SW_KERNELS_EXTRACT_API Extract API
 * @ingroup  SW_KERNELS_PRIVATE_API
 * @{
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for `Extract` operation
 *
 * @details Core APIs implement the following functionalities:
 *      -   Extract analytics operation in-place kernels for 8u, 16u and 32u input data and 8u output.
 *      -   Extract analytics operation out-of-place kernels for 8u, 16u and 32u input data and 8u output.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_EXTRACT_H__
#define QPLC_EXTRACT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef qplc_status_t (*qplc_extract_i_t_ptr)(uint8_t *src_dst_ptr,
                                              uint32_t length,
                                              uint32_t *index_ptr,
                                              uint32_t low_value,
                                              uint32_t high_value);

typedef qplc_status_t (*qplc_extract_t_ptr)(const uint8_t *src_ptr,
                                            uint8_t *dst_ptr,
                                            uint32_t length,
                                            uint32_t *index_ptr,
                                            uint32_t low_value,
                                            uint32_t high_value);

/**
 * @name qplc_extract_<input bit-width><output bit-width>_i
 *
 * @brief Extract analytics operation in-place kernels for 8u, 16u and 32u input data
 *
 * @param[in,out]  src_dst_ptr  pointer to source and destination vector (in-place operation)
 * @param[in]      length       length of source and destination vector in elements
 * @param[in,out]  index_ptr    pointer to the current source index (operation by chunks)
 * @param[in]      low_value    low index value for extract operation
 * @param[in]      high_value   high index value for extract operation
 *
 * @note Extract operation extracts values from src_ptr with indexes in range low_value - high_value (inclusive)
 *
 * @return
 *      - number of extracted elements.
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_extract_8u_i, (uint8_t *src_dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(qplc_status_t, qplc_extract_16u_i, (uint8_t *src_dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(qplc_status_t, qplc_extract_32u_i, (uint8_t *src_dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))
/** @} */

/**
 * @name qplc_extract_<input bit-width><output bit-width>
 *
 * @brief Extract analytics operation out-of-place kernels for 8u, 16u and 32u input data
 *
 * @param[in]      src_ptr     pointer to source vector
 * @param[out]     dst_ptr     pointer to destination vector
 * @param[in]      length      length of source and destination vector in elements
 * @param[in,out]  index_ptr   pointer to the current source index (operation by chunks)
 * @param[in]      low_value   low index value for extract operation
 * @param[in]      high_value  high index value for extract operation
 *
 * @note Extract operation extracts values from src_ptr with indexes in range low_value - high_value (inclusive)
 *
 * @return
 *      - number of extracted elements.
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_extract_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(qplc_status_t, qplc_extract_16u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(qplc_status_t, qplc_extract_32u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_EXTRACT_H__
/** @} */
