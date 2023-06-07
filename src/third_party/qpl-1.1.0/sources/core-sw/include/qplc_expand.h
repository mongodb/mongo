/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 07/30/2020
 *
 * @defgroup SW_KERNELS_EXPAND_API Expand API
 * @ingroup  SW_KERNELS_PRIVATE_API
 * @{
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for `Expand` operation
 *
 * @details Core APIs implement the following functionalities:
 *      -   Expand analytics operation out-of-place kernels for 8u, 16u and 32u input/output data.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_EXPAND_H__
#define QPLC_EXPAND_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef qplc_status_t (*qplc_expand_t_ptr)(const uint8_t *src1_ptr,
                                           uint32_t length_1,
                                           const uint8_t *src2_ptr,
                                           uint32_t *length_2_ptr,
                                           uint8_t *dst_ptr);

/**
 * @name qplc_expand_<input bit-width>
 *
 * @brief Expand analytics operation out-of-place kernels for 8u, 16u and 32u input data
 *
 * @param[in]      src1_ptr      pointer to source vector #1
 * @param[in]      length_1      length of source #1 vector in elements
 * @param[in]      src2_ptr      pointer to source #2 vector (mask)
 * @param[in,out]  length_2_ptr  pointer to length of source #2 vector in elements
 * @param[out]     dst_ptr       pointer to destination vector
 *
 * @note Expand operation puts values from src_ptr to dst_ptr if corresponding mask defined by src2_ptr is not zero,
 *       otherwise it puts 0 to dst_ptr
 *
 * @return    - number of expanded elements.
 * @{
 */
OWN_QPLC_API(qplc_status_t, qplc_expand_8u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr))

OWN_QPLC_API(qplc_status_t, qplc_expand_16u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr))

OWN_QPLC_API(qplc_status_t, qplc_expand_32u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_EXPAND_H__
/** @} */
