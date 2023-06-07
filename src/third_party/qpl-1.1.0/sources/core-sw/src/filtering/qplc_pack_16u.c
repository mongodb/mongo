/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing word integers to 9...16-bit integers
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_16u9u
 *          - @ref qplc_pack_16u10u
 *          - @ref qplc_pack_16u11u
 *          - @ref qplc_pack_16u12u
 *          - @ref qplc_pack_16u13u
 *          - @ref qplc_pack_16u14u
 *          - @ref qplc_pack_16u15u
 *          - @ref qplc_pack_16u16u
 *          - @ref qplc_pack_16u32u
 */
#include "own_qplc_defs.h"
#include "qplc_memop.h"

#if PLATFORM >= K0
#include "opt/qplc_pack_16u_k0.h"
#else

OWN_QPLC_INLINE(void, qplc_pack_16u_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t bit_width,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    int32_t  bits_in_buf  = (int32_t) (bit_width + start_bit);
    uint16_t *src_16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst_16u_ptr = (uint16_t *) dst_ptr;
    uint32_t src          = (uint32_t) (*dst_16u_ptr) & OWN_BIT_MASK(start_bit);

    src |= ((uint32_t) (*src_16u_ptr)) << start_bit;
    src_16u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_WORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_16u_ptr = (uint16_t) (src);
            dst_16u_ptr++;
            src = src >> OWN_WORD_WIDTH;
            bits_in_buf -= OWN_WORD_WIDTH;
        }
        src = src | (((uint32_t) (*src_16u_ptr)) << bits_in_buf);
        src_16u_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    dst_ptr               = (uint8_t *) dst_16u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t) (src);
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
    }
}
#endif

OWN_QPLC_FUN(void, qplc_pack_16u9u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u9u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 9u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u10u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u10u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 10u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u11u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u11u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 11u, dst_ptr, start_bit);
#endif
}


OWN_QPLC_FUN(void, qplc_pack_16u12u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u12u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 12u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u13u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u13u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 13u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u14u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u14u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 14u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u15u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u15u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_16u_nu(src_ptr, num_elements, 15u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements * sizeof(uint16_t));
}

OWN_QPLC_FUN(void, qplc_pack_16u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_16u32u)(src_ptr, num_elements, dst_ptr);
#else
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;
    uint16_t *src_16u_ptr = (uint16_t *) src_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst_32u_ptr[i] = src_16u_ptr[i];
    }
#endif
}
