/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 9..16-bit data to words
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_9u16u
 *          - @ref qplc_unpack_10u16u
 *          - @ref qplc_unpack_11u16u
 *          - @ref qplc_unpack_12u16u
 *          - @ref qplc_unpack_13u16u
 *          - @ref qplc_unpack_14u16u
 *          - @ref qplc_unpack_15u16u
 *          - @ref qplc_unpack_16u16u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_memop.h"
#include "qplc_unpack.h"

#if PLATFORM >= K0

#include "opt/qplc_unpack_16u_k0.h"

#else

OWN_QPLC_INLINE(void, qplc_unpack_Nu16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    if (0u == num_elements) {
        return;
    }
    {
        uint32_t mask = OWN_BIT_MASK(bit_width);
        uint32_t next_word;
        uint32_t bits_in_buf = OWN_WORD_WIDTH - start_bit;
        uint16_t* src16u_ptr = (uint16_t*)src_ptr;
        uint16_t* dst16u_ptr = (uint16_t*)dst_ptr;
        uint32_t src = (uint32_t)((*src16u_ptr) >> start_bit);
        src16u_ptr++;

        while (1u < num_elements) {
            if (bit_width > bits_in_buf) {
                next_word = (uint32_t)(*src16u_ptr);
                src16u_ptr++;
                next_word = next_word << bits_in_buf;
                src = src | next_word;
                bits_in_buf += OWN_WORD_WIDTH;
            }
            *dst16u_ptr = (uint16_t)(src & mask);
            src = src >> bit_width;
            bits_in_buf -= bit_width;
            dst16u_ptr++;
            num_elements--;
        }

        if (bit_width > bits_in_buf) {
            next_word = (uint32_t)(bit_width - bits_in_buf > 8u ? *src16u_ptr : *((uint8_t*)src16u_ptr));
            next_word = next_word << bits_in_buf;
            src = src | next_word;
        }
        *dst16u_ptr = (uint16_t)(src & mask);
    }
}

#endif

OWN_QPLC_FUN(void, qplc_unpack_9u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_9u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 9u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_10u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_10u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 10u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_11u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_11u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 11u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_12u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_12u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 12u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_13u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_13u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 13u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_14u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_14u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 14u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_15u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_15u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu16u(src_ptr, num_elements, start_bit, 15u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t UNREFERENCED_PARAMETER(start_bit),
        uint8_t *dst_ptr)) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements * sizeof(uint16_t));
}
