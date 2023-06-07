/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_mmemop.h -------*/

/**
 * @date 07/06/2020
 *
 * @defgroup SW_KERNELS_MEMOP_API Memory operations API
 * @ingroup  SW_KERNELS_PRIVATE_API
 * @{
 * @brief Contains Contains Intel® Query Processing Library (Intel® QPL) Core API for `Memory Group`
 *
 * @details Memory operations list:
 *      -   Memory copy
 *      -   memory move,
 *      -   memory set - fill with zeroes for 8u, 16u and 32u data types
 *
 */

#include "qplc_defines.h"

#ifndef QPLC_MEMOP_API_H_
#define QPLC_MEMOP_API_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*qplc_copy_t_ptr)(const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length);

typedef void (*qplc_zero_t_ptr)(uint8_t *dst_ptr, uint32_t length);

typedef void (*qplc_move_t_ptr)(const uint8_t* src_ptr, uint8_t* dst_ptr, uint32_t length);


/**
 * @name qplc_set_<input-output bit-width>
 *
 * @brief Set operation kernels for 8u, 16u and 32u data (memset analogue).
 *
 * @param[in]   value    value to set
 * @param[out]  dst_ptr  pointer to destination vector
 * @param[in]   length   number of value elements to set
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_set_8u, (uint8_t value, uint8_t * dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_set_16u, (uint16_t value, uint16_t * dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_set_32u, (uint32_t value, uint32_t * dst_ptr, uint32_t length))
/** @} */

/**
 * @name qplc_copy_<input-output bit-width>
 *
 * @brief Copy operation kernels for 8u data (memcpy analogue).
 *
 * @param[in]   src_ptr  pointer to source vector
 * @param[out]  dst_ptr  pointer to destination vector
 * @param[in]   length   number of elements to copy
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_copy_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_copy_16u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_copy_32u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length))
/** @} */

/**
 * @name qplc_zero_<output bit-width>
 *
 * @brief Zero operation kernels for 8u data (fill dst with zeroes).
 *
 * @param[out]  dst_ptr  pointer to destination vector
 * @param[in]   length   number of elements to copy
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_zero_8u, (uint8_t* dst_ptr, uint32_t length))
/** @} */

/**
 * @name qplc_move_<input-output bit-width>
 *
 * @brief Move operation kernels for 8u data (memmove analogue).
 *
 * @param[in]   src_ptr  pointer to source vector
 * @param[out]  dst_ptr  pointer to destination vector
 * @param[in]   length   number of elements to copy
 *
 * @return
 *      - n/a (void).
 * @{
 */
OWN_QPLC_API(void, qplc_move_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_move_16u, (const uint16_t *src_ptr, uint16_t *dst_ptr, uint32_t length))

OWN_QPLC_API(void, qplc_move_32u, (const uint32_t *src_ptr, uint32_t *dst_ptr, uint32_t length))
/** @} */

#ifdef __cplusplus
}
#endif

#endif // QPLC_MEMOP_API_H_
/** @} */
