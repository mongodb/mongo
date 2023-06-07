/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing dword integers to 17...32-bit integers in BE format
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_be_32u17u
 *          - @ref qplc_pack_be_32u18u
 *          - @ref qplc_pack_be_32u19u
 *          - @ref qplc_pack_be_32u20u
 *          - @ref qplc_pack_be_32u21u
 *          - @ref qplc_pack_be_32u22u
 *          - @ref qplc_pack_be_32u23u
 *          - @ref qplc_pack_be_32u24u
 *          - @ref qplc_pack_be_32u25u
 *          - @ref qplc_pack_be_32u26u
 *          - @ref qplc_pack_be_32u27u
 *          - @ref qplc_pack_be_32u28u
 *          - @ref qplc_pack_be_32u29u
 *          - @ref qplc_pack_be_32u30u
 *          - @ref qplc_pack_be_32u31u
 *          - @ref qplc_pack_be_32u32u
 *
 */
#include "own_qplc_defs.h"

OWN_QPLC_INLINE(void, qplc_pack_be_32u_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t bit_width,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    // For BE start_bit is bit index from the top of a dword
    int32_t  bits_in_buf  = (int32_t) (bit_width + start_bit);
    uint32_t *src_32u_ptr = (uint32_t *) src_ptr;
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;
    uint64_t src          = ((uint64_t) qplc_swap_bytes_32u(*dst_32u_ptr)) >> (OWN_DWORD_WIDTH - start_bit);

    src <<= (OWN_QWORD_WIDTH - start_bit);
    src |= ((uint64_t) (*src_32u_ptr)) << (OWN_QWORD_WIDTH - bits_in_buf);
    src_32u_ptr++;
    num_elements--;

    while (0u < num_elements) {
        if (OWN_DWORD_WIDTH <= (uint32_t)bits_in_buf) {
            *dst_32u_ptr = qplc_swap_bytes_32u((uint32_t) (src >> OWN_DWORD_WIDTH));
            dst_32u_ptr++;
            src = src << OWN_DWORD_WIDTH;
            bits_in_buf -= OWN_DWORD_WIDTH;
        }
        bits_in_buf += bit_width;
        src = src | (((uint64_t) (*src_32u_ptr)) << (OWN_QWORD_WIDTH - bits_in_buf));
        src_32u_ptr++;
        num_elements--;
    }
    dst_ptr               = (uint8_t *) dst_32u_ptr;
    while (0 < bits_in_buf) {
        *dst_ptr = (uint8_t) (src >> (OWN_QWORD_WIDTH - OWN_BYTE_WIDTH));
        bits_in_buf -= OWN_BYTE_WIDTH;
        dst_ptr++;
        src <<= OWN_BYTE_WIDTH;
    }
}

// ********************** 17u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u17u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 17u, dst_ptr, start_bit);
}

// ********************** 18u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u18u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 18u, dst_ptr, start_bit);
}

// ********************** 19u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u19u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 19u, dst_ptr, start_bit);
}

// ********************** 20u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u20u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 20u, dst_ptr, start_bit);
}

// ********************** 21u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u21u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 21u, dst_ptr, start_bit);
}

// ********************** 22u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u22u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 22u, dst_ptr, start_bit);
}

// ********************** 23u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u23u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 23u, dst_ptr, start_bit);
}

// ********************** 24u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u24u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 24u, dst_ptr, start_bit);
}

// ********************** 25u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u25u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 25u, dst_ptr, start_bit);
}

// ********************** 26u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u26u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 26u, dst_ptr, start_bit);
}

// ********************** 27u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u27u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 27u, dst_ptr, start_bit);
}

// ********************** 28u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u28u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 28u, dst_ptr, start_bit);
}

// ********************** 29u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u29u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 29u, dst_ptr, start_bit);
}

// ********************** 30u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u30u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 30u, dst_ptr, start_bit);
}

// ********************** 31u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u31u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    qplc_pack_be_32u_nu(src_ptr, num_elements, 31u, dst_ptr, start_bit);
}

// ********************** 32u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_be_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    uint32_t *src32u_ptr = (uint32_t *) src_ptr;
    uint32_t *dst32u_ptr = (uint32_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst32u_ptr[i] = qplc_swap_bytes_32u(src32u_ptr[i]);
    }
}
