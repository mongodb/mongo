/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 07/28/2020
 *
 * @defgroup SW_KERNELS_SELECT_API Select API
 * @ingroup  SW_KERNELS_PRIVATE_API
 * @{
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for `Select` operation
 *
 * @details Core APIs implement the following functionalities:
 *      -   Select analytics operation in-place kernels for 8u, 16u and 32u input/output data.
 *      -   Select analytics operation out-of-place kernels for 8u, 16u and 32u input/output data.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_SELECT_H__
#define QPLC_SELECT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef qplc_status_t (*qplc_select_i_t_ptr)(uint8_t *src_dst_ptr,
                                             const uint8_t *src2_ptr,
                                             uint32_t length);

typedef qplc_status_t (*qplc_select_t_ptr)(const uint8_t *src_ptr,
                                           const uint8_t *src2_ptr,
                                           uint8_t *dst_ptr,
                                           uint32_t length);
/**
 * @name qplc_select_<input bit-width>_i
 *
 * @brief Select analytics operation in-place kernels for 8u, 16u and 32u input data
 *
 * @param[in,out]  src_dst_ptr  pointer to source and destination vector (in-place operation)
 * @param[in]      src2_ptr     pointer to the source #2 vector (mask)
 * @param[in]      length       length of source vectors in elements
 *
 * @note Select operation puts values from src_ptr to dst_ptr if corresponding mask defined by src2_ptr is not zero
 *
 * @return
 *      - number of selected elements.
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_select_8u_i, (uint8_t * src_dst_ptr,
        const uint8_t *src2_ptr,
        uint32_t      length))

OWN_QPLC_API(qplc_status_t, qplc_select_16u_i, (uint8_t * src_dst_ptr,
        const uint8_t *src2_ptr,
        uint32_t      length))

OWN_QPLC_API(qplc_status_t, qplc_select_32u_i, (uint8_t * src_dst_ptr,
        const uint8_t *src2_ptr,
        uint32_t      length))
/** @} */

/**
 * @name qplc_select_<input bit-width>
 *
 * @brief Select analytics operation out-of-place kernels for 8u, 16u and 32u input data
 *
 * @param[in]   src_ptr   pointer to source vector
 * @param[in]   src2_ptr  pointer to the source #2 vector (mask)
 * @param[out]  dst_ptr   pointer to destination vector
 * @param[in]   length    length of source and destination vector in elements
 *
 * @note Select operation puts values from src_ptr to dst_ptr if corresponding mask defined by src2_ptr is not zero
 *
 * @return
 *      - number of selected elements.
 * @{
 */

OWN_QPLC_API(qplc_status_t, qplc_select_8u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length))

OWN_QPLC_API(qplc_status_t, qplc_select_16u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length))

OWN_QPLC_API(qplc_status_t, qplc_select_32u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_SELECT_H__
/** @} */
