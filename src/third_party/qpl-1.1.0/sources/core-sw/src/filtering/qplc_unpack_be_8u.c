/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 1..8-bit BE data to bytes
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_be_1u8u
 *          - @ref qplc_unpack_be_2u8u
 *          - @ref qplc_unpack_be_3u8u
 *          - @ref qplc_unpack_be_4u8u
 *          - @ref qplc_unpack_be_5u8u
 *          - @ref qplc_unpack_be_6u8u
 *          - @ref qplc_unpack_be_7u8u
 *          - @ref qplc_unpack_be_8u8u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_memop.h"
#include "qplc_unpack.h"

#if PLATFORM >= K0

#include "opt/qplc_unpack_be_8u_k0.h"

#else

// For BE start_bit is bit index from the top of a byte
OWN_QPLC_INLINE(void, qplc_unpack_be_Nu8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint16_t next_byte;
    uint32_t bits_in_buf = OWN_BYTE_WIDTH - start_bit;
    uint32_t shift       = OWN_WORD_WIDTH - bit_width;
    uint16_t src         = ((uint16_t) (*src_ptr)) << (start_bit + OWN_BYTE_WIDTH);
    src_ptr++;

    while (0u < num_elements) {
        if (bit_width > bits_in_buf) {
            next_byte = (uint16_t) (*src_ptr);
            src_ptr++;
            next_byte = next_byte << (OWN_BYTE_WIDTH - bits_in_buf);
            src       = src | next_byte;
            bits_in_buf += OWN_BYTE_WIDTH;
        }
        *dst_ptr = (uint8_t) (src >> shift);
        src = src << bit_width;
        bits_in_buf -= bit_width;
        dst_ptr++;
        num_elements--;
    }
}

#endif

// ********************** 1u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_1u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_1u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 1u, dst_ptr);
#endif
}

// ********************** 2u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_2u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_2u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 2u, dst_ptr);
#endif
}

// ********************** 3u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_3u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_3u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 3u, dst_ptr);
#endif
}

// ********************** 4u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_4u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_4u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 4u, dst_ptr);
#endif
}

// ********************** 5u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_5u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_5u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 5u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_be_6u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_6u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 6u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_be_7u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_7u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu8u(src_ptr, num_elements, start_bit, 7u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_be_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t UNREFERENCED_PARAMETER(start_bit),
        uint8_t *dst_ptr)) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements);
}
