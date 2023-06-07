/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing byte integers to 1...8-bit integers in BE format
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_be_8u1u
 *          - @ref qplc_pack_be_8u2u
 *          - @ref qplc_pack_be_8u3u
 *          - @ref qplc_pack_be_8u4u
 *          - @ref qplc_pack_be_8u5u
 *          - @ref qplc_pack_be_8u6u
 *          - @ref qplc_pack_be_8u7u
 *          - @ref qplc_pack_be_8u8u
 *          - @ref qplc_pack_be_8u16u
 *          - @ref qplc_pack_be_8u32u
 */
#include "own_qplc_defs.h"
#include "qplc_memop.h"

OWN_QPLC_INLINE(void, qplc_pack_be_8u_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t bit_width,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    // For BE start_bit is bit index from the top of a byte
    uint32_t bits_in_buf = bit_width + start_bit;
    uint16_t src         = ((uint16_t) (*dst_ptr)) >> (OWN_BYTE_WIDTH - start_bit);

    src <<= (OWN_WORD_WIDTH - start_bit);
    src |= ((uint16_t) (*src_ptr)) << (OWN_WORD_WIDTH - bits_in_buf);
    src_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_BYTE_WIDTH <= bits_in_buf) {
            *dst_ptr = (uint8_t) (src >> OWN_BYTE_WIDTH);
            dst_ptr++;
            src = src << OWN_BYTE_WIDTH;
            bits_in_buf -= OWN_BYTE_WIDTH;
        }
        bits_in_buf += bit_width;
        src = src | (((uint16_t) (*src_ptr)) << (OWN_WORD_WIDTH - bits_in_buf));
        src_ptr++;
        num_elements--;
    }
    if (0u < bits_in_buf) {
        *dst_ptr = (uint8_t) (src >> OWN_BYTE_WIDTH);
        if (OWN_BYTE_WIDTH < bits_in_buf) {
            dst_ptr++;
            *dst_ptr = (uint8_t) (src);
        }
    }
}

// ********************** 1u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_8u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 1u, dst_ptr, start_bit);
}

// ********************** 2u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_8u2u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 2u, dst_ptr, start_bit);
}

// ********************** 3u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_8u3u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 3u, dst_ptr, start_bit);
}

// ********************** 4u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_8u4u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 4u, dst_ptr, start_bit);
}

// ********************** 5u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_8u5u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 5u, dst_ptr, start_bit);
}

OWN_QPLC_FUN(void, qplc_pack_be_8u6u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 6u, dst_ptr, start_bit);
}

OWN_QPLC_FUN(void, qplc_pack_be_8u7u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_8u_nu(src_ptr, num_elements, 7u, dst_ptr, start_bit);
}

OWN_QPLC_FUN(void, qplc_pack_be_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements);
}

OWN_QPLC_FUN(void, qplc_pack_be_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    uint16_t *dst_16u_ptr = (uint16_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst_16u_ptr[i] = qplc_swap_bytes_16u((uint16_t) src_ptr[i]);
    }
}

OWN_QPLC_FUN(void, qplc_pack_be_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst_32u_ptr[i] = qplc_swap_bytes_32u((uint32_t) src_ptr[i]);
    }
}
