/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Own Includes
 */

#ifndef OWN_ANALITICS_H_
#define OWN_ANALITICS_H_

#ifdef __cplusplus
extern "C" {
#endif

/*------- own_analytics.h -------*/

/**
 * @date 07/14/2020
 *
 * @defgroup SW_QPL_ANALYTICS_PRIVATE_API Analytics API
 * @ingroup  SW_QPL_PRIVATE_API
 * @{
 * @brief Contains data structure definitions and macro for analytics operations.
 *
 * @details We need 3 buffers: for inflate, for PRLE & data unpacking,
 *          and for "set" analytics operations (set membership & find unique)
 *          Also several in-line helpers for all analytics functions
 *
 */

#include "stdint.h"

/* ------ Macro ------ */
/**
 * @brief Maximum buffer sizes for analytics operations
 */

/**
 * Maximum number of unpacked elements
 */
#define OWN_MAX_ELEMENTS 4096u

/**
 * Unpack buffer size, +1u
 */
#define OWN_UNPACK_BUF_SIZE ((OWN_MAX_ELEMENTS + 1u) * sizeof(uint32_t))

/**
 * Inflate buffer size
 */
#define OWN_INFLATE_BUF_SIZE 32768u

/**
 * Max supported bit_width for set operations
 */
#define OWN_MAX_SET_SIZE 15u

/**
 * Find unique/Set membership buffer size - 2^OWN_MAX_SET_SIZE
 */
#define OWN_SET_BUF_SIZE (1u << OWN_MAX_SET_SIZE)

/**
 * Expand src2 unpack buffer size - MAX_ELEMENTS in bytes
 */
#define OWN_SRC2_BUF_SIZE (OWN_MAX_ELEMENTS * sizeof(uint32_t))

/**
 * @brief Interal structure for analytics buffers manipulations
 */
typedef struct {
    uint32_t inflate_buf_size;    /**< Size of buffer for inflate operation */
    uint32_t unpack_buf_size;     /**< Size of buffer for unpack operation */
    uint32_t set_buf_size;        /**< Size of buffer used in select and expand */
    uint32_t src2_buf_size;       /**< Size of buffer for src2 unpacking in expand operation */
    uint8_t  *inflate_buf_ptr;    /**< Pointer to inflate buffer */
    uint8_t  *unpack_buf_ptr;     /**< Pointer to unpack buffer */
    uint8_t  *set_buf_ptr;        /**< Pointer to buffer used in select and expand */
    uint8_t  *src2_buf_ptr;       /**< Pointer to src2 buffer for expand */
} own_analytics_state_t;

#ifdef __cplusplus
}
#endif

#endif // OWN_ANALITICS_H_
/** @} */
