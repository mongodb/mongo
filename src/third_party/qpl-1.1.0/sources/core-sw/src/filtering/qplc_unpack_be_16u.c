/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 9..16-bit BE data to words
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_be_9u16u
 *          - @ref qplc_unpack_be_10u16u
 *          - @ref qplc_unpack_be_11u16u
 *          - @ref qplc_unpack_be_12u16u
 *          - @ref qplc_unpack_be_13u16u
 *          - @ref qplc_unpack_be_14u16u
 *          - @ref qplc_unpack_be_15u16u
 *          - @ref qplc_unpack_be_16u16u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_unpack.h"

#if PLATFORM >= K0

#include "opt/qplc_unpack_be_16u_k0.h"

#else

// For BE start_bit is bit index from the top of a byte
OWN_QPLC_INLINE(void, qplc_unpack_be_Nu16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint16_t *src16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst16u_ptr = (uint16_t *) dst_ptr;
    uint32_t bits_in_buf = OWN_WORD_WIDTH - start_bit;
    uint32_t shift       = OWN_DWORD_WIDTH - bit_width;
    uint32_t src         = ((uint32_t) qplc_swap_bytes_16u(*src16u_ptr)) << (OWN_DWORD_WIDTH - bits_in_buf);
    uint32_t next_word;

    src16u_ptr++;

    while (1u < num_elements) {
        if (bit_width > bits_in_buf) {
            next_word = (uint32_t) qplc_swap_bytes_16u(*src16u_ptr);
            src16u_ptr++;
            next_word = next_word << (OWN_WORD_WIDTH - bits_in_buf);
            src       = src | next_word;
            bits_in_buf += OWN_WORD_WIDTH;
        }
        *dst16u_ptr = (uint16_t) (src >> shift);
        src = src << bit_width;
        bits_in_buf -= bit_width;
        dst16u_ptr++;
        num_elements--;
    }

    uint8_t *src8u_ptr = (uint8_t *) src16u_ptr;
    if (bit_width > bits_in_buf) {
        uint32_t bytes_to_read = OWN_BITS_2_BYTE(bit_width - bits_in_buf);
        if (bytes_to_read == 2u) {
            next_word = *((uint16_t *) src8u_ptr);
            src8u_ptr += 2;
        } else {
            next_word = *src8u_ptr;
            src8u_ptr++;
        }
        next_word              = (uint32_t) qplc_swap_bytes_16u(next_word);
        next_word              = next_word << (OWN_WORD_WIDTH - bits_in_buf);
        src                    = src | next_word;
        bits_in_buf += OWN_WORD_WIDTH;
    }
    *dst16u_ptr = (uint16_t) (src >> shift);
    src = src << bit_width;
    bits_in_buf -= bit_width;
    dst16u_ptr++;
    num_elements--;
}

#endif

// ********************** 9u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_9u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_9u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 9u, dst_ptr);
#endif
}

// ********************** 10u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_10u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_10u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 10u, dst_ptr);
#endif
}

// ********************** 11u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_11u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_11u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 11u, dst_ptr);
#endif
}

// ********************** 12u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_12u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_12u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 12u, dst_ptr);
#endif
}

// ********************** 13u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_13u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_13u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 13u, dst_ptr);
#endif
}

// ********************** 14u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_14u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_14u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 14u, dst_ptr);
#endif
}

// ********************** 15u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_15u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_15u16u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu16u(src_ptr, num_elements, start_bit, 15u, dst_ptr);
#endif
}

// ********************** 16u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_16u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t UNREFERENCED_PARAMETER(start_bit),
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_16u16u)(src_ptr, num_elements, dst_ptr);
#else
    uint16_t *src16u_ptr = (uint16_t *)src_ptr;
    uint16_t *dst16u_ptr = (uint16_t *)dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++)
    {
        dst16u_ptr[i] = qplc_swap_bytes_16u(src16u_ptr[i]);
    }
#endif
}
