/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for vector packing byte integers to 1...8-bit integers
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_pack_8u1u
 *          - @ref qplc_pack_8u2u
 *          - @ref qplc_pack_8u3u
 *          - @ref qplc_pack_8u4u
 *          - @ref qplc_pack_8u5u
 *          - @ref qplc_pack_8u6u
 *          - @ref qplc_pack_8u7u
 *          - @ref qplc_pack_8u8u
 *          - @ref qplc_pack_8u16u
 *          - @ref qplc_pack_8u32u
 */
#include "own_qplc_defs.h"
#include "qplc_memop.h"

#if PLATFORM >= K0
#include "opt/qplc_pack_8u_k0.h"
#endif

// ********************** 1u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_8u1u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {

#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u1u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    uint32_t i;

    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u != start_bit) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit++;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
        if (0 == num_elements) {
            return;
        }
    }
    while (num_elements > 64u) {
        uint64_t bit_buf  = 0LLu;
        uint64_t *tmp_dst = (uint64_t *) dst_ptr;
        uint64_t bit_mask;

        for (i = 0; i < 64u; i++) {
            bit_mask = OWN_1_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << i;
            bit_buf |= bit_mask;
        }
        src_ptr += 64;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint64_t);
        num_elements -= 64u;
    }
    if (num_elements > 32u) {
        uint32_t bit_buf  = 0u;
        uint32_t *tmp_dst = (uint32_t *) dst_ptr;
        uint32_t bit_mask;

        for (i = 0; i < 32u; i++) {
            bit_mask = OWN_1_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << i;
            bit_buf |= bit_mask;
        }
        src_ptr += 32u;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint32_t);
        num_elements -= 32u;
    }
    if (num_elements > 16u) {
        uint16_t bit_buf  = 0u;
        uint16_t *tmp_dst = (uint16_t *) dst_ptr;
        uint16_t bit_mask;

        for (i = 0; i < 16; i++) {
            bit_mask = OWN_1_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << i;
            bit_buf |= bit_mask;
        }
        src_ptr += 16u;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint16_t);
        num_elements -= 16u;
    }
    if (0u < num_elements) {
        uint16_t bit_buf  = 0u;
        uint16_t bit_mask;
        uint16_t *tmp_dst = (uint16_t *) dst_ptr;
        for (i            = 0u; i < num_elements; i++) {
            bit_mask = OWN_1_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << i;
            bit_buf |= bit_mask;
        }
        if (OWN_BYTE_WIDTH >= i) {
            *dst_ptr = (uint8_t) bit_buf;
        } else {
            *tmp_dst = bit_buf;
        }
    }
#endif
}

// ********************** 2u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_8u2u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u2u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    uint32_t i;

    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u != start_bit) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit += 2u;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
        if (0u == num_elements) {
            return;
        }
    }
    while (num_elements > 32u) {
        uint64_t bit_buf  = 0LLu;
        uint64_t *tmp_dst = (uint64_t *) dst_ptr;
        uint64_t bit_mask;

        for (i = 0u; i < 32u; i++) {
            bit_mask = OWN_2_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << 2u * i;
            bit_buf |= bit_mask;
        }
        src_ptr += 32u;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint64_t);
        num_elements -= 32u;
    }
    if (num_elements > 16u) {
        uint32_t bit_buf  = 0u;
        uint32_t *tmp_dst = (uint32_t *) dst_ptr;
        uint32_t bit_mask;

        for (i = 0u; i < 16u; i++) {
            bit_mask = OWN_2_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << 2u * i;
            bit_buf |= bit_mask;
        }
        src_ptr += 16u;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint32_t);
        num_elements -= 16u;
    }
    if (num_elements > 8u) {
        uint16_t bit_buf  = 0u;
        uint16_t *tmp_dst = (uint16_t *) dst_ptr;
        uint16_t bit_mask;

        for (i = 0u; i < 8u; i++) {
            bit_mask = OWN_2_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << 2u * i;
            bit_buf |= bit_mask;
        }
        src_ptr += 8u;
        *tmp_dst = bit_buf;
        dst_ptr += sizeof(uint16_t);
        num_elements -= 8u;
    }
    if (0u < num_elements) {
        uint16_t bit_buf  = 0u;
        uint16_t bit_mask;
        uint16_t *tmp_dst = (uint16_t *) dst_ptr;
        for (i            = 0u; i < num_elements; i++) {
            bit_mask = OWN_2_BIT_MASK & src_ptr[i];
            bit_mask = bit_mask << 2u * i;
            bit_buf |= bit_mask;
        }
        if (4u >= i) {
            *dst_ptr = (uint8_t) bit_buf;
        } else {
            *tmp_dst = bit_buf;
        }
    }
#endif
}

// ********************** 3u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_8u3u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u3u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u != start_bit) {
        *dst_ptr |= *src_ptr << start_bit;
        start_bit += 3u;
        if (OWN_BYTE_WIDTH < start_bit) {
            start_bit -= OWN_BYTE_WIDTH;
            dst_ptr++;
            *dst_ptr = *src_ptr >> (3u - start_bit);
        }
        num_elements--;
        src_ptr++;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
        if (0u == num_elements) {
            return;
        }
    }
    while (num_elements > 32u) {
        uint64_t bit_buf64;
        uint32_t bit_buf32;
        uint64_t *dst64_ptr = (uint64_t *) dst_ptr;
        uint32_t *dst32_ptr = (uint32_t *) (dst_ptr + 8u);

        bit_buf64 = (uint64_t) src_ptr[0];
        bit_buf64 |= ((uint64_t) (src_ptr[1])) << 3u;
        bit_buf64 |= ((uint64_t) (src_ptr[2])) << 6u;
        bit_buf64 |= ((uint64_t) (src_ptr[3])) << 9u;
        bit_buf64 |= ((uint64_t) (src_ptr[4])) << 12u;
        bit_buf64 |= ((uint64_t) (src_ptr[5])) << 15u;
        bit_buf64 |= ((uint64_t) (src_ptr[6])) << 18u;
        bit_buf64 |= ((uint64_t) (src_ptr[7])) << 21u;
        bit_buf64 |= ((uint64_t) (src_ptr[8])) << 24u;
        bit_buf64 |= ((uint64_t) (src_ptr[9])) << 27u;
        bit_buf64 |= ((uint64_t) (src_ptr[10])) << 30u;
        bit_buf64 |= ((uint64_t) (src_ptr[11])) << 33u;
        bit_buf64 |= ((uint64_t) (src_ptr[12])) << 36u;
        bit_buf64 |= ((uint64_t) (src_ptr[13])) << 39u;
        bit_buf64 |= ((uint64_t) (src_ptr[14])) << 42u;
        bit_buf64 |= ((uint64_t) (src_ptr[15])) << 45u;
        bit_buf64 |= ((uint64_t) (src_ptr[16])) << 48u;
        bit_buf64 |= ((uint64_t) (src_ptr[17])) << 51u;
        bit_buf64 |= ((uint64_t) (src_ptr[18])) << 54u;
        bit_buf64 |= ((uint64_t) (src_ptr[19])) << 57u;
        bit_buf64 |= ((uint64_t) (src_ptr[20])) << 60u;
        bit_buf64 |= ((uint64_t) (src_ptr[21])) << 63u;
        bit_buf32 = ((uint32_t) (src_ptr[21])) >> 1u;
        bit_buf32 |= ((uint32_t) (src_ptr[22])) << 2u;
        bit_buf32 |= ((uint32_t) (src_ptr[23])) << 5u;
        bit_buf32 |= ((uint32_t) (src_ptr[24])) << 8u;
        bit_buf32 |= ((uint32_t) (src_ptr[25])) << 11u;
        bit_buf32 |= ((uint32_t) (src_ptr[26])) << 14u;
        bit_buf32 |= ((uint32_t) (src_ptr[27])) << 17u;
        bit_buf32 |= ((uint32_t) (src_ptr[28])) << 20u;
        bit_buf32 |= ((uint32_t) (src_ptr[29])) << 23u;
        bit_buf32 |= ((uint32_t) (src_ptr[30])) << 26u;
        bit_buf32 |= ((uint32_t) (src_ptr[31])) << 29u;
        *dst64_ptr = bit_buf64;
        *dst32_ptr = bit_buf32;
        src_ptr += 32u;
        dst_ptr += 12u;
        num_elements -= 32u;
    }
    while (num_elements > 16u) {
        uint16_t bit_buf16;
        uint32_t bit_buf32;
        uint16_t *dst16_ptr = (uint16_t *) (dst_ptr + 4u);
        uint32_t *dst32_ptr = (uint32_t *) (dst_ptr);

        bit_buf32 = (uint32_t) src_ptr[0];
        bit_buf32 |= ((uint32_t) (src_ptr[1])) << 3u;
        bit_buf32 |= ((uint32_t) (src_ptr[2])) << 6u;
        bit_buf32 |= ((uint32_t) (src_ptr[3])) << 9u;
        bit_buf32 |= ((uint32_t) (src_ptr[4])) << 12u;
        bit_buf32 |= ((uint32_t) (src_ptr[5])) << 15u;
        bit_buf32 |= ((uint32_t) (src_ptr[6])) << 18u;
        bit_buf32 |= ((uint32_t) (src_ptr[7])) << 21u;
        bit_buf32 |= ((uint32_t) (src_ptr[8])) << 24u;
        bit_buf32 |= ((uint32_t) (src_ptr[9])) << 27u;
        bit_buf32 |= ((uint32_t) (src_ptr[10])) << 30u;
        bit_buf16 = ((uint16_t) (src_ptr[10])) >> 2u;
        bit_buf16 |= ((uint16_t) (src_ptr[11])) << 1u;
        bit_buf16 |= ((uint16_t) (src_ptr[12])) << 4u;
        bit_buf16 |= ((uint16_t) (src_ptr[13])) << 7u;
        bit_buf16 |= ((uint16_t) (src_ptr[14])) << 10u;
        bit_buf16 |= ((uint16_t) (src_ptr[15])) << 13u;
        *dst32_ptr = bit_buf32;
        *dst16_ptr = bit_buf16;
        src_ptr += 16u;
        dst_ptr += 6u;
        num_elements -= 16u;
    }
    while (num_elements > 8u) {
        uint16_t bit_buf16;
        uint8_t  bit_buf8;
        uint16_t *dst16_ptr = (uint16_t *) (dst_ptr);

        bit_buf16 = (uint16_t) src_ptr[0];
        bit_buf16 |= ((uint16_t) (src_ptr[1])) << 3u;
        bit_buf16 |= ((uint16_t) (src_ptr[2])) << 6u;
        bit_buf16 |= ((uint16_t) (src_ptr[3])) << 9u;
        bit_buf16 |= ((uint16_t) (src_ptr[4])) << 12u;
        bit_buf16 |= ((uint16_t) (src_ptr[5])) << 15u;
        bit_buf8  = src_ptr[5] >> 1u;
        bit_buf8 |= src_ptr[6] << 2u;
        bit_buf8 |= src_ptr[7] << 5u;
        *dst16_ptr = bit_buf16;
        dst_ptr += 2u;
        *dst_ptr = bit_buf8;
        src_ptr += 8u;
        dst_ptr++;
        num_elements -= 8u;
    }
    if (0u < num_elements) {
        uint32_t               bits_in_buf = 0u;
        qplc_bit_byte_pool32_t bit_byte_pool32;
        bit_byte_pool32.bit_buf = 0u;

        while (0u != num_elements) {
            bit_byte_pool32.bit_buf |= ((uint32_t) (*src_ptr)) << bits_in_buf;
            src_ptr++;
            bits_in_buf += 3u;
            num_elements--;
        }
        dst_ptr[0] = bit_byte_pool32.byte_buf[0];
        if (bits_in_buf > OWN_BYTE_WIDTH) {
            dst_ptr[1] = bit_byte_pool32.byte_buf[1];
        }
        if (bits_in_buf > OWN_WORD_WIDTH) {
            dst_ptr[2] = bit_byte_pool32.byte_buf[2];
        }
    }
#endif
}

// ********************** 4u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_8u4u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u4u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    uint32_t i;

    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u != start_bit) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit += 4u;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
        if (0u == num_elements) {
            return;
        }
    }
    while (num_elements > 32u) {
        uint64_t bit_buf0 = 0LLu;
        uint64_t bit_buf1 = 0LLu;
        uint64_t *tmp_dst = (uint64_t *) dst_ptr;
        uint64_t bit_mask0;
        uint64_t bit_mask1;

        for (i = 0u; i < 16u; i++) {
            bit_mask0 = ((uint64_t) src_ptr[i]) << (4u * i);
            bit_buf0 |= bit_mask0;
            bit_mask1 = ((uint64_t) src_ptr[i + 16u]) << (4u * i);
            bit_buf1 |= bit_mask1;
        }
        src_ptr += 32u;
        *tmp_dst = bit_buf0;
        tmp_dst++;
        *tmp_dst = bit_buf1;
        dst_ptr += 2u * sizeof(uint64_t);
        num_elements -= 32u;
    }
    if (num_elements > 16u) {
        uint64_t bit_buf0 = 0LLu;
        uint64_t *tmp_dst = (uint64_t *) dst_ptr;
        uint64_t bit_mask0;

        for (i = 0u; i < 16u; i++) {
            bit_mask0 = ((uint64_t) src_ptr[i]) << (4u * i);
            bit_buf0 |= bit_mask0;
        }
        src_ptr += 16u;
        *tmp_dst = bit_buf0;
        dst_ptr += sizeof(uint64_t);
        num_elements -= 16u;
    }
    if (num_elements > 8u) {
        uint32_t bit_buf0 = 0u;
        uint32_t *tmp_dst = (uint32_t *) dst_ptr;
        uint32_t bit_mask0;

        for (i = 0u; i < 8u; i++) {
            bit_mask0 = ((uint32_t) src_ptr[i]) << (4u * i);
            bit_buf0 |= bit_mask0;
        }
        src_ptr += 8u;
        *tmp_dst = bit_buf0;
        dst_ptr += sizeof(uint32_t);
        num_elements -= 8u;
    }
    uint8_t bit_buf = 0u;
    for (i = 0u; i < num_elements; i++) {
        if (OWN_1_BIT_MASK & i) {
            bit_buf |= src_ptr[i] << 4u;
            *dst_ptr = bit_buf;
            dst_ptr++;
        } else {
            bit_buf = src_ptr[i];
            *dst_ptr = bit_buf;
        }
    }
#endif
}

// ********************** 5u ****************************** //

OWN_QPLC_FUN(void, qplc_pack_8u5u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u5u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u != start_bit) {
        *dst_ptr |= *src_ptr << start_bit;
        start_bit += 5u;
        if (OWN_BYTE_WIDTH < start_bit) {
            start_bit -= OWN_BYTE_WIDTH;
            dst_ptr++;
            *dst_ptr = *src_ptr >> (5u - start_bit);
        }
        num_elements--;
        src_ptr++;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
        if (0u == num_elements) {
            return;
        }
    }
    while (num_elements > 32u) {
        uint64_t bit_buf64_0;
        uint64_t bit_buf64_1;
        uint32_t bit_buf32;
        uint64_t *dst64_ptr = (uint64_t *) dst_ptr;
        uint32_t *dst32_ptr = (uint32_t *) (dst_ptr + 2u * sizeof(uint64_t));

        bit_buf64_0 = (uint64_t) src_ptr[0];
        bit_buf64_0 |= ((uint64_t) (src_ptr[1])) << 5u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[2])) << 10u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[3])) << 15u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[4])) << 20u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[5])) << 25u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[6])) << 30u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[7])) << 35u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[8])) << 40u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[9])) << 45u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[10])) << 50u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[11])) << 55u;
        bit_buf64_0 |= ((uint64_t) (src_ptr[12])) << 60u;
        bit_buf64_1 = ((uint64_t) (src_ptr[12])) >> 4u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[13])) << 1u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[14])) << 6u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[15])) << 11u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[16])) << 16u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[17])) << 21u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[18])) << 26u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[19])) << 31u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[20])) << 36u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[21])) << 41u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[22])) << 46u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[23])) << 51u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[24])) << 56u;
        bit_buf64_1 |= ((uint64_t) (src_ptr[25])) << 61u;
        bit_buf32   = ((uint32_t) (src_ptr[25])) >> 3u;
        bit_buf32 |= ((uint32_t) (src_ptr[26])) << 2u;
        bit_buf32 |= ((uint32_t) (src_ptr[27])) << 7u;
        bit_buf32 |= ((uint32_t) (src_ptr[28])) << 12u;
        bit_buf32 |= ((uint32_t) (src_ptr[29])) << 17u;
        bit_buf32 |= ((uint32_t) (src_ptr[30])) << 22u;
        bit_buf32 |= ((uint32_t) (src_ptr[31])) << 27u;

        *dst64_ptr = bit_buf64_0;
        dst64_ptr++;
        *dst64_ptr = bit_buf64_1;
        *dst32_ptr = bit_buf32;
        src_ptr += 32u;
        dst_ptr += 20u;
        num_elements -= 32u;
    }
    while (num_elements > 16u) {
        uint64_t bit_buf64;
        uint16_t bit_buf16;
        uint64_t *dst64_ptr = (uint64_t *) dst_ptr;
        uint16_t *dst16_ptr = (uint16_t *) (dst_ptr + sizeof(uint64_t));

        bit_buf64 = (uint64_t) src_ptr[0];
        bit_buf64 |= ((uint64_t) (src_ptr[1])) << 5u;
        bit_buf64 |= ((uint64_t) (src_ptr[2])) << 10u;
        bit_buf64 |= ((uint64_t) (src_ptr[3])) << 15u;
        bit_buf64 |= ((uint64_t) (src_ptr[4])) << 20u;
        bit_buf64 |= ((uint64_t) (src_ptr[5])) << 25u;
        bit_buf64 |= ((uint64_t) (src_ptr[6])) << 30u;
        bit_buf64 |= ((uint64_t) (src_ptr[7])) << 35u;
        bit_buf64 |= ((uint64_t) (src_ptr[8])) << 40u;
        bit_buf64 |= ((uint64_t) (src_ptr[9])) << 45u;
        bit_buf64 |= ((uint64_t) (src_ptr[10])) << 50u;
        bit_buf64 |= ((uint64_t) (src_ptr[11])) << 55u;
        bit_buf64 |= ((uint64_t) (src_ptr[12])) << 60u;
        bit_buf16 = ((uint16_t) (src_ptr[12])) >> 4u;
        bit_buf16 |= ((uint16_t) (src_ptr[13])) << 1u;
        bit_buf16 |= ((uint16_t) (src_ptr[14])) << 6u;
        bit_buf16 |= ((uint16_t) (src_ptr[15])) << 11u;

        *dst64_ptr = bit_buf64;
        *dst16_ptr = bit_buf16;
        src_ptr += 16u;
        dst_ptr += 10u;
        num_elements -= 16u;
    }
    while (num_elements > 8u) {
        uint32_t bit_buf32;
        uint8_t  bit_buf8;
        uint32_t *dst32_ptr = (uint32_t *) (dst_ptr);

        bit_buf32 = (uint32_t) src_ptr[0];
        bit_buf32 |= ((uint32_t) (src_ptr[1])) << 5u;
        bit_buf32 |= ((uint32_t) (src_ptr[2])) << 10u;
        bit_buf32 |= ((uint32_t) (src_ptr[3])) << 15u;
        bit_buf32 |= ((uint32_t) (src_ptr[4])) << 20u;
        bit_buf32 |= ((uint32_t) (src_ptr[5])) << 25u;
        bit_buf32 |= ((uint32_t) (src_ptr[6])) << 30u;
        bit_buf8  = src_ptr[6] >> 2u;
        bit_buf8 |= src_ptr[7] << 3u;
        *dst32_ptr = bit_buf32;
        dst_ptr += sizeof(uint32_t);
        *dst_ptr = bit_buf8;
        src_ptr += 8u;
        dst_ptr++;
        num_elements -= 8u;
    }
    if (0u < num_elements) {
        uint32_t bits_in_buf = 5u;
        uint16_t src         = (uint16_t) (*src_ptr);
        src_ptr++;
        num_elements--;
        while (0u < num_elements) {
            src = src | (((uint16_t) (*src_ptr)) << bits_in_buf);
            src_ptr++;
            num_elements--;
            bits_in_buf += 5u;
            if (8u <= bits_in_buf) {
                *dst_ptr = (uint8_t) (src);
                dst_ptr++;
                src = src >> OWN_BYTE_WIDTH;
                bits_in_buf -= OWN_BYTE_WIDTH;
            }
        }
        if (0u < bits_in_buf) {
            *dst_ptr = (uint8_t) (src);
        }
    }
#endif
}

// ********************** 6u - 8u ****************************** //

#if PLATFORM < K0

OWN_QPLC_INLINE(void, qplc_pack_8u_nu, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t bit_width,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
    uint32_t bits_in_buf = bit_width + start_bit;
    uint16_t src         = (uint16_t) (*dst_ptr) & OWN_BIT_MASK(start_bit);
    src |= ((uint16_t) (*src_ptr)) << start_bit;
    src_ptr++;
    num_elements--;
    while (0u < num_elements) {
        if (OWN_BYTE_WIDTH <= bits_in_buf) {
            *dst_ptr = (uint8_t) (src);
            dst_ptr++;
            src = src >> OWN_BYTE_WIDTH;
            bits_in_buf -= OWN_BYTE_WIDTH;
        }
        src = src | (((uint16_t) (*src_ptr)) << bits_in_buf);
        src_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    *dst_ptr = (uint8_t) (src);
    if (OWN_BYTE_WIDTH < bits_in_buf) {
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
        *dst_ptr = (uint8_t) (src);
    }
}

#endif

OWN_QPLC_FUN(void, qplc_pack_8u6u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u6u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_8u_nu(src_ptr, num_elements, 6u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_8u7u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t start_bit)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u7u)(src_ptr, num_elements, dst_ptr, start_bit);
#else
    qplc_pack_8u_nu(src_ptr, num_elements, 7u, dst_ptr, start_bit);
#endif
}

OWN_QPLC_FUN(void, qplc_pack_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements);
}

OWN_QPLC_FUN(void, qplc_pack_8u16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u16u)(src_ptr, num_elements, dst_ptr);
#else
    uint16_t *dst_16u_ptr = (uint16_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst_16u_ptr[i] = src_ptr[i];
    }
#endif
}

OWN_QPLC_FUN(void, qplc_pack_8u32u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint8_t *dst_ptr,
        uint32_t UNREFERENCED_PARAMETER(start_bit))) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_pack_8u32u)(src_ptr, num_elements, dst_ptr);
#else
    uint32_t *dst_32u_ptr = (uint32_t *) dst_ptr;

    for (uint32_t i = 0u; i < num_elements; i++) {
        dst_32u_ptr[i] = src_ptr[i];
    }
#endif
}
