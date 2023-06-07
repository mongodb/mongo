/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_scan.h -------*/

/**
 * @date 07/06/2020
 *
 * @defgroup SW_KERNELS_SCAN_API Scan API
 * @ingroup  SW_KERNELS_PRIVATE_API
 * @{
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for `Scan` operation
 *
 * @details Scan Core APIs implement the following functionalities:
 *      -   Scan analytics operation in-place kernels for 8u, 16u and 32u input data and 8u output.
 *      -   Scan analytics operation out-of-place kernels for 8u, 16u and 32u input data and 8u output.
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_SCAN_H__
#define QPLC_SCAN_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qplc_scan_i_t_ptr)(uint8_t *src_dst_ptr,
                                  uint32_t length,
                                  uint32_t low_value,
                                  uint32_t high_value);

typedef void (*qplc_scan_t_ptr)(const uint8_t *src_ptr,
                                uint8_t *dst_ptr,
                                uint32_t length,
                                uint32_t low_value,
                                uint32_t high_value);

/**
 * @name qplc_scan_<comparison type><input bit-width><output bit-width>_i
 *
 * @brief Scan analytics operation in-place kernels for 8u, 16u and 32u input data and 8u output.
 *
 * @param[in,out]  src_dst_ptr  pointer to source and destination vector (in-place operation)
 * @param[in]      length       length of source and destination vector in elements
 * @param[in]      low_value    low value for scan operation
 * @param[in]      high_value   high value for scan operation
 *
 * @note Scan operations are lt, le, gt, ge, eq, ne, range, not range
 * @note Source-destination vector always contains result data in 8u format: 1 - condition is met,
 *       0 - condition is not met
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_scan_eq_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_eq_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_eq_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_16u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_32u8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))
/** @} */

/**
 * @name qplc_scan_<comparison type><input bit-width><output bit-width>
 *
 * @brief Scan analytics operation out-of-place kernels for 8u, 16u and 32u input data and 8u output.
 *
 * @param[in]     src_ptr       pointer to source vector
 * @param[out]    dst_ptr       pointer to destination vector
 * @param[in]     length      length of source and destination vector in elements
 * @param[in]     low_value   low value for scan operation
 * @param[in]     high_value  high value for scan operation
 *
 * @note Scan operations are lt, le, gt, ge, eq, ne, range, not range
 * @note Destination vector always contains result data in 8u format: 1 - condition is met, 0 - condition is not met
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_scan_eq_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_eq_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_eq_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ne_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_lt_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t param_low,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_le_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_gt_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_ge_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_range_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_16u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))

OWN_QPLC_API(void, qplc_scan_not_range_32u8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t low_value,
        uint32_t high_value))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_SCAN_H__
/** @} */
