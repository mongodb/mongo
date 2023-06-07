/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contain implementation of functions for calculating aggregates for nominal bit vector and nominal array
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_bit_aggregates_8u
 *          - @ref qplc_aggregates_8u
 *          - @ref qplc_aggregates_16u
 *          - @ref qplc_aggregates_32u
 */

#include "own_qplc_defs.h"

#if PLATFORM >= K0

#include "opt/qplc_aggregates_k0.h"

#endif


OWN_QPLC_FUN(void, qplc_bit_aggregates_8u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *index_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_bit_aggregates_8u)(src_ptr, length, min_value_ptr, max_value_ptr, sum_ptr, index_ptr);
#else
    for (uint32_t idx = 0u; idx < length; idx++) {
        *sum_ptr += src_ptr[idx];
        if (OWN_MAX_32U == *min_value_ptr) {
            *min_value_ptr = (0u == src_ptr[idx]) ? *min_value_ptr : idx + *index_ptr;
        }
        *max_value_ptr = (0u == src_ptr[idx]) ? *max_value_ptr : idx + *index_ptr;
    }
    *index_ptr += length;
#endif
}

OWN_QPLC_FUN(void, qplc_aggregates_8u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *UNREFERENCED_PARAMETER(index_ptr))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_aggregates_8u)(src_ptr, length, min_value_ptr, max_value_ptr, sum_ptr);
#else
    for (uint32_t idx = 0u; idx < length; idx++) {
        *sum_ptr += src_ptr[idx];
        *min_value_ptr = (src_ptr[idx] < *min_value_ptr) ? src_ptr[idx] : *min_value_ptr;
        *max_value_ptr = (src_ptr[idx] > *max_value_ptr) ? src_ptr[idx] : *max_value_ptr;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_aggregates_16u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *UNREFERENCED_PARAMETER(index_ptr))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_aggregates_16u)(src_ptr, length, min_value_ptr, max_value_ptr, sum_ptr);
#else
    const uint16_t *src_16u_ptr = (uint16_t *) src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++) {
        *sum_ptr += src_16u_ptr[idx];
        *min_value_ptr = (src_16u_ptr[idx] < *min_value_ptr) ? src_16u_ptr[idx] : *min_value_ptr;
        *max_value_ptr = (src_16u_ptr[idx] > *max_value_ptr) ? src_16u_ptr[idx] : *max_value_ptr;
    }
#endif
}

OWN_QPLC_FUN(void, qplc_aggregates_32u, (const uint8_t *src_ptr,
        uint32_t length,
        uint32_t *min_value_ptr,
        uint32_t *max_value_ptr,
        uint32_t *sum_ptr,
        uint32_t *UNREFERENCED_PARAMETER(index_ptr))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_aggregates_32u)(src_ptr, length, min_value_ptr, max_value_ptr, sum_ptr);
#else
    const uint32_t *src_32u_ptr = (uint32_t *) src_ptr;

    for (uint32_t idx = 0u; idx < length; idx++) {
        *sum_ptr += src_32u_ptr[idx];
        *min_value_ptr = (src_32u_ptr[idx] < *min_value_ptr) ? src_32u_ptr[idx] : *min_value_ptr;
        *max_value_ptr = (src_32u_ptr[idx] > *max_value_ptr) ? src_32u_ptr[idx] : *max_value_ptr;
    }
#endif
}
