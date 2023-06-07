/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of all functions for expand analytics operation
 * @date 07/30/2020
 *
 * @details Function list:
 *          - @ref qplc_expand_8u
 *          - @ref qplc_expand_16u
 *          - @ref qplc_expand_32u
 */

#include "own_qplc_defs.h"

#if PLATFORM >= K0

#include "opt/qplc_expand_k0.h"

#endif


/******** out-of-place expand functions ********/

OWN_QPLC_FUN(uint32_t, qplc_expand_8u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_qplc_expand_8u)(src1_ptr, length_1, src2_ptr, length_2_ptr, dst_ptr);
#else

    uint32_t expanded = 0u;
    uint32_t idx;

    for (idx = 0u; idx < *length_2_ptr; idx++) {
        if (src2_ptr[idx]) {
            OWN_CONDITION_BREAK(expanded >= length_1);
            dst_ptr[idx] = src1_ptr[expanded++];
        } else {
            dst_ptr[idx] = 0u;
        }
    }
    *length_2_ptr -= idx;
    return expanded;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_expand_16u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_qplc_expand_16u)(src1_ptr, length_1, src2_ptr, length_2_ptr, dst_ptr);
#else

    uint16_t *src_16u_ptr = (uint16_t *) src1_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *) dst_ptr;
    uint32_t expanded     = 0u;
    uint32_t idx;

    for (idx = 0u; idx < *length_2_ptr; idx++) {
        if (src2_ptr[idx]) {
            OWN_CONDITION_BREAK(expanded >= length_1);
            dst_16u_ptr[idx] = src_16u_ptr[expanded++];
        } else {
            dst_16u_ptr[idx] = 0u;
        }
    }
    *length_2_ptr -= idx;
    return expanded;
#endif
}

OWN_QPLC_FUN(uint32_t, qplc_expand_32u, (const uint8_t *src1_ptr,
        uint32_t length_1,
        const uint8_t *src2_ptr,
        uint32_t *length_2_ptr,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_qplc_qplc_expand_32u)(src1_ptr, length_1, src2_ptr, length_2_ptr, dst_ptr);
#else

    uint32_t *src_32u_ptr = (uint32_t *) src1_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;
    uint32_t expanded     = 0u;
    uint32_t idx;

    for (idx = 0u; idx < *length_2_ptr; idx++) {
        if (src2_ptr[idx]) {
            OWN_CONDITION_BREAK(expanded >= length_1);
            dst_32u_ptr[idx] = src_32u_ptr[expanded++];
        } else {
            dst_32u_ptr[idx] = 0u;
        }
    }
    *length_2_ptr -= idx;
    return expanded;
#endif
}
