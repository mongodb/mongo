/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_aggregates.h -------*/
/**
 * @date 07/06/2020
 *
 * @brief Contains Intel® Query Processing Library (Intel® QPL) Core API for aggregates calculation
 *
 * @details Function list:
 *          - @ref qplc_bit_aggregates_8u
 */

/**
 * @defgroup SW_KERNELS_AGGREGATES_API Aggregates API
 * @ingroup SW_KERNELS_PRIVATE_API
 * @{
 */

#include "qplc_defines.h"

#ifndef QPLC_AGGREGATES_H_
#define QPLC_AGGREGATES_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qplc_aggregates_t_ptr)(const uint8_t *src_ptr,
                                      uint32_t length,
                                      uint32_t *min_value_ptr,
                                      uint32_t *max_value_ptr,
                                      uint32_t *sum_ptr,
                                      uint32_t *index_ptr);

/**
 * @name qplc_bit_aggregates_8u
 *
 * @brief Bit-aggregates function calculates minimum and maximum indexes for '1' elements and sum of all '1' elements.
 *
 * @param[in]      src_ptr        pointer to source vector
 * @param[in]      length         length of source vector in elements (bytes)
 * @param[in,out]  min_value_ptr  pointer to index of the first '1' in source vector
 * @param[in,out]  max_value_ptr  pointer to index of the last '1' in source vector
 * @param[in,out]  sum_ptr        pointer to the sum of all elements in source vector
 * @param[in,out]  index_ptr      pointer to the current element index
 *
 * @note The index incrementing can start from any initial value set in qpl_job_ptr structure.
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_bit_aggregates_8u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *index_ptr))
/** @} */

/**
 * @name qplc_aggregates_<input bit-width>
 *
 * @brief Array-aggregates function calculates minimum and maximum vector values and sum of all vector elements.
 *
 * @param[in]      src_ptr        pointer to source vector
 * @param[in]      length         length of source vector in elements (bytes)
 * @param[in,out]  min_value_ptr  pointer to min value over input vector
 * @param[in,out]  max_value_ptr  pointer to max value over input vector
 * @param[in,out]  sum_ptr        pointer to the sum of all elements in the source vector
 * @param[in,out]  index_ptr      is not used (unreferenced parameter)
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_aggregates_8u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *index_ptr))

OWN_QPLC_API(void, qplc_aggregates_16u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *index_ptr))

OWN_QPLC_API(void, qplc_aggregates_32u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *index_ptr))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_AGGREGATES_H_
/** @} */
