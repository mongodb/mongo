/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 17..32-bit BE data to dwords
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_be_17u32u
 *          - @ref qplc_unpack_be_18u32u
 *          - @ref qplc_unpack_be_19u32u
 *          - @ref qplc_unpack_be_20u32u
 *          - @ref qplc_unpack_be_21u32u
 *          - @ref qplc_unpack_be_22u32u
 *          - @ref qplc_unpack_be_23u32u
 *          - @ref qplc_unpack_be_24u32u
 *          - @ref qplc_unpack_be_25u32u
 *          - @ref qplc_unpack_be_26u32u
 *          - @ref qplc_unpack_be_27u32u
 *          - @ref qplc_unpack_be_28u32u
 *          - @ref qplc_unpack_be_29u32u
 *          - @ref qplc_unpack_be_30u32u
 *          - @ref qplc_unpack_be_31u32u
 *          - @ref qplc_unpack_be_32u32u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_unpack.h"

#if PLATFORM >= K0

#include "opt/qplc_unpack_be_32u_k0.h"

#else

// For BE start_bit is bit index from the top of a byte
OWN_QPLC_INLINE(void, qplc_unpack_be_Nu32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint32_t *src32u_ptr = (uint32_t *) src_ptr;
    uint8_t  *src8u_ptr  = (uint8_t *) src_ptr;
    uint32_t *dst32u_ptr = (uint32_t *) dst_ptr;
    uint32_t shift       = OWN_QWORD_WIDTH - bit_width;
    uint32_t bits_in_buf = 0u;
    uint64_t src         = 0u;
    uint64_t next_dword;
    uint32_t bytes_to_read = OWN_BITS_2_BYTE(num_elements * bit_width + start_bit);

    if (sizeof(uint32_t) <= bytes_to_read) {
        bits_in_buf = OWN_DWORD_WIDTH - start_bit;
        src         = ((uint64_t) qplc_swap_bytes_32u(*src32u_ptr)) << (OWN_QWORD_WIDTH - bits_in_buf);
        
        src32u_ptr++;

        while (2u < num_elements) {
            if (bit_width > bits_in_buf) {
                next_dword = (uint64_t) qplc_swap_bytes_32u(*src32u_ptr);
                src32u_ptr++;
                next_dword = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
                src        = src | next_dword;
                bits_in_buf += OWN_DWORD_WIDTH;
            }
            *dst32u_ptr = (uint32_t) (src >> shift);
            src = src << bit_width;
            bits_in_buf -= bit_width;
            dst32u_ptr++;
            num_elements--;
        }

        bytes_to_read = OWN_BITS_2_BYTE(num_elements * bit_width > bits_in_buf ?
                                        num_elements * bit_width - bits_in_buf : 0u);

        if (bytes_to_read > 3u) {
            next_dword = (uint64_t) qplc_swap_bytes_32u(*src32u_ptr);
            src32u_ptr++;
            next_dword = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
            src        = src | next_dword;
            bits_in_buf += OWN_DWORD_WIDTH;
            bytes_to_read -= 4u;
        }

        src8u_ptr = (uint8_t *) src32u_ptr;
    } else {
        next_dword    = 0u;
        for (uint32_t byte_to_read = 0u; byte_to_read < bytes_to_read; byte_to_read++) {
            next_dword |= ((uint64_t) (*src8u_ptr)) << (byte_to_read * OWN_BYTE_WIDTH);
            src8u_ptr++;
        }
        next_dword   = (uint64_t) qplc_swap_bytes_32u((uint32_t) next_dword);
        bits_in_buf  = OWN_DWORD_WIDTH - start_bit;
        next_dword   = next_dword << (OWN_QWORD_WIDTH - bits_in_buf);
        src          = next_dword;
        *dst32u_ptr  = (uint32_t) (src >> shift);
        return;
    }

    while (0u < num_elements) {
        if (bit_width > bits_in_buf) {
            next_dword = 0u;
            for (uint32_t byte_to_read = 0u; byte_to_read < bytes_to_read; byte_to_read++) {
                next_dword |= ((uint64_t) (*src8u_ptr)) << (byte_to_read * OWN_BYTE_WIDTH);
                src8u_ptr++;
            }
            next_dword  = (uint64_t) qplc_swap_bytes_32u((uint32_t) next_dword);
            next_dword  = next_dword << (OWN_DWORD_WIDTH - bits_in_buf);
            src         = src | next_dword;
            bits_in_buf += OWN_DWORD_WIDTH;
        }
        *dst32u_ptr = (uint32_t) (src >> shift);
        src = src << bit_width;
        bits_in_buf -= bit_width;
        dst32u_ptr++;
        num_elements--;
    }
}

#endif

// ********************** 17u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_17u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_17u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 17u, dst_ptr);
#endif
}

// ********************** 18u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_18u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_18u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 18u, dst_ptr);
#endif
}

// ********************** 19u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_19u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_19u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 19u, dst_ptr);
#endif
}

// ********************** 20u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_20u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_20u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 20u, dst_ptr);
#endif
}

// ********************** 21u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_21u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_21u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 21u, dst_ptr);
#endif
}

// ********************** 22u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_22u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_22u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 22u, dst_ptr);
#endif
}

// ********************** 23u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_23u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_23u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 23u, dst_ptr);
#endif
}

// ********************** 24u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_24u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_24u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 24u, dst_ptr);
#endif
}

// ********************** 25u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_25u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_25u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 25u, dst_ptr);
#endif
}

// ********************** 26u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_26u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_26u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 26u, dst_ptr);
#endif
}

// ********************** 27u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_27u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_27u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 27u, dst_ptr);
#endif
}

// ********************** 28u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_28u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_28u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 28u, dst_ptr);
#endif
}

// ********************** 29u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_29u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_29u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 29u, dst_ptr);
#endif
}

// ********************** 30u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_30u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_30u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 30u, dst_ptr);
#endif
}

// ********************** 31u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_31u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_31u32u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_be_Nu32u(src_ptr, num_elements, start_bit, 31u, dst_ptr);
#endif
}

// ********************** 32u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_be_32u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t UNREFERENCED_PARAMETER(start_bit),
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_be_32u32u)(src_ptr, num_elements, dst_ptr);
#else
    uint32_t *src32u_ptr = (uint32_t *)src_ptr;
    uint32_t *dst32u_ptr = (uint32_t *)dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++)
    {
        dst32u_ptr[i] = qplc_swap_bytes_32u(src32u_ptr[i]);
    }
#endif
}
