/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of all functions for select analytics operation
 * @date 07/28/2020
 *
 * @details Function list:
 *          - @ref qplc_select_8u_i
 *          - @ref qplc_select_16u_i
 *          - @ref qplc_select_32u_i
 *          - @ref qplc_select_8u
 *          - @ref qplc_select_16u
 *          - @ref qplc_select_32u
 *
 */

#include "own_qplc_defs.h"

#if PLATFORM >= K0

#include "opt/qplc_select_k0.h"

#endif

OWN_QPLC_FUN(uint32_t, qplc_select_8u_i, (uint8_t * src_dst_ptr, const uint8_t *src2_ptr, uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_8u)((const uint8_t*)src_dst_ptr, src2_ptr, src_dst_ptr, length);
#else
    uint8_t  *src_ptr = src_dst_ptr;
    uint8_t  *dst_ptr = src_dst_ptr;
    uint32_t selected = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_ptr[selected++] = src_ptr[idx];
        }
    }
    return selected;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_select_16u_i, (uint8_t * src_dst_ptr, const uint8_t *src2_ptr, uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_16u)((const uint8_t*)src_dst_ptr, src2_ptr, src_dst_ptr, length);
#else
    uint16_t *src_ptr = (uint16_t *) src_dst_ptr;
    uint16_t *dst_ptr = (uint16_t *) src_dst_ptr;
    uint32_t selected = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_ptr[selected++] = src_ptr[idx];
        }
    }
    return selected;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_select_32u_i, (uint8_t * src_dst_ptr, const uint8_t *src2_ptr, uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_32u)((const uint8_t*)src_dst_ptr, src2_ptr, src_dst_ptr, length);
#else
    uint32_t *src_ptr = (uint32_t *) src_dst_ptr;
    uint32_t *dst_ptr = (uint32_t *) src_dst_ptr;
    uint32_t selected = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_ptr[selected++] = src_ptr[idx];
        }
    }
    return selected;
#endif
}

/******** out-of-place select functions ********/

OWN_QPLC_FUN(uint32_t, qplc_select_8u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_8u)(src_ptr, src2_ptr, dst_ptr, length);
#else
    uint32_t selected = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_ptr[selected++] = src_ptr[idx];
        }
    }
    return selected;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_select_16u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_16u)(src_ptr, src2_ptr, dst_ptr, length);
#else
    uint16_t *src_16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *) dst_ptr;
    uint32_t selected     = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_16u_ptr[selected++] = src_16u_ptr[idx];
        }
    }
    return selected;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_select_32u, (const uint8_t *src_ptr,
        const uint8_t *src2_ptr,
        uint8_t *dst_ptr,
        uint32_t length)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_select_32u)(src_ptr, src2_ptr, dst_ptr, length);
#else
    uint32_t *src_32u_ptr = (uint32_t *) src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;
    uint32_t selected     = 0u;

    for (uint32_t idx = 0u; idx < length; idx++) {
        if (src2_ptr[idx] != 0u) {
            dst_32u_ptr[selected++] = src_32u_ptr[idx];
        }
    }
    return selected;
#endif
}
