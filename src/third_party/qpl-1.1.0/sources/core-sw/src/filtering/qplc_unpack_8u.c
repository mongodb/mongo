/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 1..8-bit data to bytes
 * @date 07/06/2020
 *
 * @details Function list:
 *          - @ref qplc_unpack_1u8u
 *          - @ref qplc_unpack_2u8u
 *          - @ref qplc_unpack_3u8u
 *          - @ref qplc_unpack_4u8u
 *          - @ref qplc_unpack_5u8u
 *          - @ref qplc_unpack_6u8u
 *          - @ref qplc_unpack_7u8u
 *          - @ref qplc_unpack_8u8u
 *
 */

#include "own_qplc_defs.h"
#include "qplc_memop.h"
#include "qplc_unpack.h"

#if PLATFORM >= K0

#include "opt/qplc_unpack_8u_k0.h"

#endif

// ********************** 1u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_1u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_1u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    uint64_t bit_mask = 0x0101010101010101LLU;
    uint32_t i;
    uint8_t  mask;

    // Align to byte boundary
    if (0u < start_bit) {
        mask = OWN_1_BIT_MASK << start_bit;
        while (0u < mask) {
            *dst_ptr = (0u < (*src_ptr & mask)) ? 1u : 0u;
            dst_ptr++;
            mask = mask << 1u;
            num_elements--;
            if (0u == num_elements) {
                return;
            }
        }
        src_ptr++;
    }
    while (num_elements > 64u) {
        uint64_t               *tmp_src = (uint64_t *) src_ptr;
        uint64_t               src      = *tmp_src;
        qplc_bit_byte_pool64_t bit_byte_pool;

        for (i = 0u; i < 8u; i++) {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u]  = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 8u]  = bit_byte_pool.byte_buf[1];
            dst_ptr[i + 16u] = bit_byte_pool.byte_buf[2];
            dst_ptr[i + 24u] = bit_byte_pool.byte_buf[3];
            dst_ptr[i + 32u] = bit_byte_pool.byte_buf[4];
            dst_ptr[i + 40u] = bit_byte_pool.byte_buf[5];
            dst_ptr[i + 48u] = bit_byte_pool.byte_buf[6];
            dst_ptr[i + 56u] = bit_byte_pool.byte_buf[7];
            src = src >> 1;
        }
        src_ptr += sizeof(uint64_t);
        dst_ptr += 64u;
        num_elements -= 64u;
    }
    if (num_elements > 32u) {
        qplc_bit_byte_pool32_t bit_byte_pool;
        uint32_t               *tmp_src = (uint32_t *) src_ptr;
        uint32_t               src      = *tmp_src;

        for (i = 0u; i < 8u; i++) {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u]  = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 8u]  = bit_byte_pool.byte_buf[1];
            dst_ptr[i + 16u] = bit_byte_pool.byte_buf[2];
            dst_ptr[i + 24u] = bit_byte_pool.byte_buf[3];
            src = src >> 1u;
        }
        src_ptr += sizeof(uint32_t);
        dst_ptr += 32u;
        num_elements -= 32u;
    }
    if (num_elements > 16u) {
        qplc_bit_byte_pool16_t bit_byte_pool;
        uint16_t               *tmp_src = (uint16_t *) src_ptr;
        uint16_t               src      = *tmp_src;

        for (i = 0u; i < 8u; i++) {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u] = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 8u] = bit_byte_pool.byte_buf[1];
            src = src >> 1u;
        }
        src_ptr += sizeof(uint16_t);
        dst_ptr += 16u;
        num_elements -= 16u;
    }
    if (num_elements) {
        mask        = OWN_1_BIT_MASK;
        uint8_t src = *src_ptr;
        for (i = 0u; i < num_elements; i++) {
            if (8u == i) {
                src = *(++src_ptr);
            }
            dst_ptr[i] = (src & mask);
            src = src >> 1u;
        }
    }
#endif
}

// ********************** 2u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_2u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_2u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    uint64_t bit_mask = 0x0303030303030303LLU;
    uint32_t i;
    uint8_t mask = 3u;

    // Align to byte boundary
    if (0u < start_bit)
    {
        uint8_t src = (*src_ptr) >> start_bit;
        src_ptr++;
        while (OWN_BYTE_WIDTH > start_bit)
        {
            *dst_ptr = src & mask;
            dst_ptr++;
            src = src >> 2u;
            start_bit += 2u; 
            num_elements--;
            if (0u == num_elements)
            {
                return;
            }
        }
    }
    while (num_elements > 32u)
    {
        qplc_bit_byte_pool64_t bit_byte_pool;
        uint64_t *tmp_src = (uint64_t *)src_ptr;
        uint64_t src = *tmp_src;

        for (i = 0u; i < 4u; i++)
        {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u]  = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 4u]  = bit_byte_pool.byte_buf[1];
            dst_ptr[i + 8u]  = bit_byte_pool.byte_buf[2];
            dst_ptr[i + 12u] = bit_byte_pool.byte_buf[3];
            dst_ptr[i + 16u] = bit_byte_pool.byte_buf[4];
            dst_ptr[i + 20u] = bit_byte_pool.byte_buf[5];
            dst_ptr[i + 24u] = bit_byte_pool.byte_buf[6];
            dst_ptr[i + 28u] = bit_byte_pool.byte_buf[7];
            src = src >> 2u;
        }
        src_ptr += sizeof(uint64_t);
        dst_ptr += 32u;
        num_elements -= 32u;
    }
    if (num_elements > 16u)
    {
        qplc_bit_byte_pool32_t bit_byte_pool;
        uint32_t *tmp_src = (uint32_t *)src_ptr;
        uint32_t src = *tmp_src;

        for (i = 0u; i < 4u; i++)
        {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u]  = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 4u]  = bit_byte_pool.byte_buf[1];
            dst_ptr[i + 8u]  = bit_byte_pool.byte_buf[2];
            dst_ptr[i + 12u] = bit_byte_pool.byte_buf[3];
            src = src >> 2u;
        }
        src_ptr += sizeof(uint32_t);
        dst_ptr += 16u;
        num_elements -= 16u;
    }
    if (num_elements > 8u)
    {
        qplc_bit_byte_pool16_t bit_byte_pool;
        uint16_t *tmp_src = (uint16_t *)src_ptr;
        uint16_t src = *tmp_src;

        for (i = 0u; i < 4u; i++)
        {
            bit_byte_pool.bit_buf = src & bit_mask;
            dst_ptr[i + 0u]  = bit_byte_pool.byte_buf[0];
            dst_ptr[i + 4u]  = bit_byte_pool.byte_buf[1];
            src = src >> 2u;
        }
        src_ptr += sizeof(uint16_t);
        dst_ptr += 8u;
        num_elements -= 8u;
    }

    if (num_elements) {
        // 4 elements are stored in a single byte
        const uint32_t elements_in_byte = 4u;

        // Process rest of elements (there are 8 or less elements left)
        mask               = OWN_2_BIT_MASK;
        uint8_t src_buffer = *src_ptr;

        for (i = 0u; i < num_elements; i++) {
            // Check if 1st byte is processed, shift to next one if true
            if (elements_in_byte == i) {
                src_buffer = *(++src_ptr);
            }

            dst_ptr[i] = (src_buffer & mask);
            src_buffer = src_buffer >> 2u;
        }
    }
#endif
}

// ********************** 3u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_3u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_3u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    uint64_t bit_mask0 = 0x0007000007000007LLU;
    uint64_t bit_mask1 = 0x0700000700000700LLU;
    uint64_t bit_mask2 = 0x0000070000070000LLU;

    // Align to byte boundary
    if (0u < start_bit)
    {
        uint16_t mask = OWN_3_BIT_MASK;
        uint16_t next_byte;
        uint32_t bits_in_buf = OWN_BYTE_WIDTH - start_bit;
        uint16_t src = (uint16_t)((*src_ptr) >> start_bit);
        src_ptr++;
        while (0u < bits_in_buf)
        {
            if (3u > bits_in_buf)
            {
                next_byte = (uint16_t)(*src_ptr);
                src_ptr++;
                next_byte = next_byte << bits_in_buf;
                src = src | next_byte;
                bits_in_buf += OWN_BYTE_WIDTH;
            }
            *dst_ptr = (uint8_t)(src & mask);
            src = src >> 3u;
            bits_in_buf -= 3u;
            dst_ptr++;
            num_elements--;
            if (0u == num_elements)
            {
                return;
            }
        }
    }
    while (num_elements > 32u)
    {
        qplc_bit_byte_pool64_t bit_byte_pool64;
        uint64_t *tmp_src64 = (uint64_t *)src_ptr;
        uint64_t src64 = *tmp_src64;
        src_ptr = src_ptr + 8u;
        qplc_bit_byte_pool32_t bit_byte_pool32;
        uint32_t *tmp_src32 = (uint32_t *)src_ptr;
        uint32_t src32 = (*tmp_src32);
        src_ptr = src_ptr + 4u;

        bit_byte_pool64.bit_buf  = src64 & bit_mask0;
        dst_ptr[0]  = bit_byte_pool64.byte_buf[0];
        dst_ptr[8]  = bit_byte_pool64.byte_buf[3];
        dst_ptr[16] = bit_byte_pool64.byte_buf[6];
        bit_byte_pool64.bit_buf  = (src64 >> 1u) & bit_mask1;
        dst_ptr[3]  = bit_byte_pool64.byte_buf[1];
        dst_ptr[11] = bit_byte_pool64.byte_buf[4];
        dst_ptr[19] = bit_byte_pool64.byte_buf[7];
        bit_byte_pool64.bit_buf = (src64 >> 2u) & bit_mask2;
        dst_ptr[6]  = bit_byte_pool64.byte_buf[2];
        dst_ptr[14] = bit_byte_pool64.byte_buf[5];
        bit_byte_pool64.bit_buf  = (src64 >> 3u) & bit_mask0;
        dst_ptr[1]  = bit_byte_pool64.byte_buf[0];
        dst_ptr[9]  = bit_byte_pool64.byte_buf[3];
        dst_ptr[17] = bit_byte_pool64.byte_buf[6];
        bit_byte_pool64.bit_buf  = (src64 >> 4u) & bit_mask1;
        dst_ptr[4]  = bit_byte_pool64.byte_buf[1];
        dst_ptr[12] = bit_byte_pool64.byte_buf[4];
        dst_ptr[20] = bit_byte_pool64.byte_buf[7];
        bit_byte_pool64.bit_buf  = (src64 >> 5u) & bit_mask2;
        dst_ptr[7]  = bit_byte_pool64.byte_buf[2];
        dst_ptr[15] = bit_byte_pool64.byte_buf[5];
        bit_byte_pool64.bit_buf = (src64 >> 6u) & bit_mask0;
        dst_ptr[2] = bit_byte_pool64.byte_buf[0];
        dst_ptr[10] = bit_byte_pool64.byte_buf[3];
        dst_ptr[18] = bit_byte_pool64.byte_buf[6];
        bit_byte_pool64.bit_buf = (src64 >> 7u) & bit_mask1;
        dst_ptr[5] = bit_byte_pool64.byte_buf[1];
        dst_ptr[13] = bit_byte_pool64.byte_buf[4];
        dst_ptr[21] = bit_byte_pool64.byte_buf[7] | (((uint8_t)src32 & 3u) << 1u);
        src32 = src32 >> 2u;
        bit_byte_pool32.bit_buf = src32 & bit_mask0;
        dst_ptr[22] = bit_byte_pool32.byte_buf[0];
        dst_ptr[30] = bit_byte_pool32.byte_buf[3];
        bit_byte_pool32.bit_buf = (src32 >> 1u) & bit_mask1;
        dst_ptr[25] = bit_byte_pool32.byte_buf[1];
        bit_byte_pool32.bit_buf = (src32 >> 2u) & bit_mask2;
        dst_ptr[28] = bit_byte_pool32.byte_buf[2];
        bit_byte_pool32.bit_buf = (src32 >> 3u) & bit_mask0;
        dst_ptr[23] = bit_byte_pool32.byte_buf[0];
        dst_ptr[31] = bit_byte_pool32.byte_buf[3];
        bit_byte_pool32.bit_buf = (src32 >> 4u) & bit_mask1;
        dst_ptr[26] = bit_byte_pool32.byte_buf[1];
        bit_byte_pool32.bit_buf = (src32 >> 5u) & bit_mask2;
        dst_ptr[29] = bit_byte_pool32.byte_buf[2];
        bit_byte_pool32.bit_buf = (src32 >> 6u) & bit_mask0;
        dst_ptr[24] = bit_byte_pool32.byte_buf[0];
        bit_byte_pool32.bit_buf = (src32 >> 7u) & bit_mask1;
        dst_ptr[27] = bit_byte_pool32.byte_buf[1];
        dst_ptr += 32u;
        num_elements -= 32u;
    }
    if (num_elements > 16u)
    {
        qplc_bit_byte_pool48_t bit_byte_pool48;
        uint32_t *tmp_src32 = (uint32_t *)src_ptr;
        bit_byte_pool48.dw_buf[0] = (*tmp_src32);
        src_ptr = src_ptr + sizeof(uint32_t);
        uint16_t *tmp_src16 = (uint16_t *)src_ptr;
        bit_byte_pool48.word_buf[2] = (*tmp_src16);
        src_ptr = src_ptr + sizeof(uint16_t);
        uint64_t src64 = bit_byte_pool48.bit_buf;
        bit_byte_pool48.bit_buf = src64 & bit_mask0;
        dst_ptr[0] = bit_byte_pool48.byte_buf[0];
        dst_ptr[8] = bit_byte_pool48.byte_buf[3];
        bit_byte_pool48.bit_buf = (src64 >> 1u) & bit_mask1;
        dst_ptr[3] = bit_byte_pool48.byte_buf[1];
        dst_ptr[11] = bit_byte_pool48.byte_buf[4];
        bit_byte_pool48.bit_buf = (src64 >> 2u) & bit_mask2;
        dst_ptr[6] = bit_byte_pool48.byte_buf[2];
        dst_ptr[14] = bit_byte_pool48.byte_buf[5];
        bit_byte_pool48.bit_buf = (src64 >> 3u) & bit_mask0;
        dst_ptr[1] = bit_byte_pool48.byte_buf[0];
        dst_ptr[9] = bit_byte_pool48.byte_buf[3];
        bit_byte_pool48.bit_buf = (src64 >> 4u) & bit_mask1;
        dst_ptr[4] = bit_byte_pool48.byte_buf[1];
        dst_ptr[12] = bit_byte_pool48.byte_buf[4];
        bit_byte_pool48.bit_buf = (src64 >> 5u) & bit_mask2;
        dst_ptr[7] = bit_byte_pool48.byte_buf[2];
        dst_ptr[15] = bit_byte_pool48.byte_buf[5];
        bit_byte_pool48.bit_buf = (src64 >> 6u) & bit_mask0;
        dst_ptr[2] = bit_byte_pool48.byte_buf[0];
        dst_ptr[10] = bit_byte_pool48.byte_buf[3];
        bit_byte_pool48.bit_buf = (src64 >> 7u) & bit_mask1;
        dst_ptr[5] = bit_byte_pool48.byte_buf[1];
        dst_ptr[13] = bit_byte_pool48.byte_buf[4];
        dst_ptr += 16u;
        num_elements -= 16u;
    }
    if (num_elements > 8u)
    {
        qplc_bit_byte_pool32_t bit_byte_pool32;
        uint16_t *tmp_src16 = (uint16_t *)src_ptr;
        bit_byte_pool32.word_buf[0] = (*tmp_src16);
        src_ptr = src_ptr + 2u;
        bit_byte_pool32.byte_buf[2] = (*src_ptr);
        src_ptr = src_ptr + 1u;
        uint32_t src32 = bit_byte_pool32.bit_buf;
        bit_byte_pool32.bit_buf = src32 & (uint32_t)bit_mask0;
        dst_ptr[0] = bit_byte_pool32.byte_buf[0];
        bit_byte_pool32.bit_buf = (src32 >> 3u) & (uint32_t)bit_mask0;
        dst_ptr[1] = bit_byte_pool32.byte_buf[0];
        bit_byte_pool32.bit_buf = (src32 >> 6u) & (uint32_t)bit_mask0;
        dst_ptr[2] = bit_byte_pool32.byte_buf[0];
        bit_byte_pool32.bit_buf = (src32 >> 1u) & (uint32_t)bit_mask1;
        dst_ptr[3] = bit_byte_pool32.byte_buf[1];
        bit_byte_pool32.bit_buf = (src32 >> 4u) & (uint32_t)bit_mask1;
        dst_ptr[4] = bit_byte_pool32.byte_buf[1];
        bit_byte_pool32.bit_buf = (src32 >> 7u) & (uint32_t)bit_mask1;
        dst_ptr[5] = bit_byte_pool32.byte_buf[1];
        bit_byte_pool32.bit_buf = (src32 >> 2u) & (uint32_t)bit_mask2;
        dst_ptr[6] = bit_byte_pool32.byte_buf[2];
        bit_byte_pool32.bit_buf = (src32 >> 5u) & (uint32_t)bit_mask2;
        dst_ptr[7] = bit_byte_pool32.byte_buf[2];
        dst_ptr += 8u;
        num_elements -= 8u;
    }
    if (0u < num_elements)
    {
        uint16_t mask = OWN_3_BIT_MASK;
        uint16_t next_byte;
        uint32_t bits_in_buf = OWN_BYTE_WIDTH;
        uint16_t src = (uint16_t)(*src_ptr);
        src_ptr++;
        while (0u != num_elements)
        {
            if (3u > bits_in_buf)
            {
                next_byte = (uint16_t)(*src_ptr);
                src_ptr++;
                next_byte = next_byte << bits_in_buf;
                src = src | next_byte;
                bits_in_buf += OWN_BYTE_WIDTH;
            }
            *dst_ptr = (uint8_t)(src & mask);
            src = src >> 3u;
            bits_in_buf -= 3u;
            dst_ptr++;
            num_elements--;
        }
    }
#endif
}

// ********************** 4u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_4u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_4u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    uint64_t bit_mask = 0x0f0f0f0f0f0f0f0fLLU;
    uint32_t i;
    uint8_t mask = OWN_4_BIT_MASK;

    // Align to byte boundary
    if (0u < start_bit)
    {
        uint8_t src = (*src_ptr) >> start_bit;
        src_ptr++;
        while (OWN_BYTE_WIDTH > start_bit)
        {
            *dst_ptr = src & mask;
            dst_ptr++;
            src = src >> 4u;
            start_bit += 4u;
            num_elements--;
            if (0u == num_elements)
            {
                return;
            }
        }
    }
    while (num_elements > 32u)
    {
        qplc_bit_byte_pool64_t bit_byte_pool;
        uint64_t *tmp_src = (uint64_t *)src_ptr;
        uint64_t src = *tmp_src;

        bit_byte_pool.bit_buf = src & bit_mask;
        dst_ptr[0] = bit_byte_pool.byte_buf[0];
        dst_ptr[2] = bit_byte_pool.byte_buf[1];
        dst_ptr[4] = bit_byte_pool.byte_buf[2];
        dst_ptr[6] = bit_byte_pool.byte_buf[3];
        dst_ptr[8] = bit_byte_pool.byte_buf[4];
        dst_ptr[10] = bit_byte_pool.byte_buf[5];
        dst_ptr[12] = bit_byte_pool.byte_buf[6];
        dst_ptr[14] = bit_byte_pool.byte_buf[7];
        bit_byte_pool.bit_buf = (src >> 4u) & bit_mask;
        dst_ptr[1] = bit_byte_pool.byte_buf[0];
        dst_ptr[3] = bit_byte_pool.byte_buf[1];
        dst_ptr[5] = bit_byte_pool.byte_buf[2];
        dst_ptr[7] = bit_byte_pool.byte_buf[3];
        dst_ptr[9] = bit_byte_pool.byte_buf[4];
        dst_ptr[11] = bit_byte_pool.byte_buf[5];
        dst_ptr[13] = bit_byte_pool.byte_buf[6];
        dst_ptr[15] = bit_byte_pool.byte_buf[7];
        tmp_src++;
        dst_ptr += 16u;
        src = *tmp_src;
        bit_byte_pool.bit_buf = src & bit_mask;
        dst_ptr[0] = bit_byte_pool.byte_buf[0];
        dst_ptr[2] = bit_byte_pool.byte_buf[1];
        dst_ptr[4] = bit_byte_pool.byte_buf[2];
        dst_ptr[6] = bit_byte_pool.byte_buf[3];
        dst_ptr[8] = bit_byte_pool.byte_buf[4];
        dst_ptr[10] = bit_byte_pool.byte_buf[5];
        dst_ptr[12] = bit_byte_pool.byte_buf[6];
        dst_ptr[14] = bit_byte_pool.byte_buf[7];
        bit_byte_pool.bit_buf = (src >> 4u) & bit_mask;
        dst_ptr[1] = bit_byte_pool.byte_buf[0];
        dst_ptr[3] = bit_byte_pool.byte_buf[1];
        dst_ptr[5] = bit_byte_pool.byte_buf[2];
        dst_ptr[7] = bit_byte_pool.byte_buf[3];
        dst_ptr[9] = bit_byte_pool.byte_buf[4];
        dst_ptr[11] = bit_byte_pool.byte_buf[5];
        dst_ptr[13] = bit_byte_pool.byte_buf[6];
        dst_ptr[15] = bit_byte_pool.byte_buf[7];
        num_elements -= 32u;
        dst_ptr += 16u;
        src_ptr += 16u;
    }
    if (num_elements > 16u)
    {
        qplc_bit_byte_pool64_t bit_byte_pool;
        uint64_t *tmp_src = (uint64_t *)src_ptr;
        uint64_t src = *tmp_src;

        bit_byte_pool.bit_buf = src & bit_mask;
        dst_ptr[0] = bit_byte_pool.byte_buf[0];
        dst_ptr[2] = bit_byte_pool.byte_buf[1];
        dst_ptr[4] = bit_byte_pool.byte_buf[2];
        dst_ptr[6] = bit_byte_pool.byte_buf[3];
        dst_ptr[8] = bit_byte_pool.byte_buf[4];
        dst_ptr[10] = bit_byte_pool.byte_buf[5];
        dst_ptr[12] = bit_byte_pool.byte_buf[6];
        dst_ptr[14] = bit_byte_pool.byte_buf[7];
        bit_byte_pool.bit_buf = (src >> 4u) & bit_mask;
        dst_ptr[1] = bit_byte_pool.byte_buf[0];
        dst_ptr[3] = bit_byte_pool.byte_buf[1];
        dst_ptr[5] = bit_byte_pool.byte_buf[2];
        dst_ptr[7] = bit_byte_pool.byte_buf[3];
        dst_ptr[9] = bit_byte_pool.byte_buf[4];
        dst_ptr[11] = bit_byte_pool.byte_buf[5];
        dst_ptr[13] = bit_byte_pool.byte_buf[6];
        dst_ptr[15] = bit_byte_pool.byte_buf[7];
        dst_ptr += 16u;
        num_elements -= 16u;
        src_ptr += 8u;
    }
    if (num_elements > 8u)
    {
        qplc_bit_byte_pool32_t bit_byte_pool;
        uint32_t *tmp_src = (uint32_t *)src_ptr;
        uint32_t src = *tmp_src;

        bit_byte_pool.bit_buf = src & (uint32_t)bit_mask;
        dst_ptr[0] = bit_byte_pool.byte_buf[0];
        dst_ptr[2] = bit_byte_pool.byte_buf[1];
        dst_ptr[4] = bit_byte_pool.byte_buf[2];
        dst_ptr[6] = bit_byte_pool.byte_buf[3];
        bit_byte_pool.bit_buf = (src >> 4u) & (uint32_t)bit_mask;
        dst_ptr[1] = bit_byte_pool.byte_buf[0];
        dst_ptr[3] = bit_byte_pool.byte_buf[1];
        dst_ptr[5] = bit_byte_pool.byte_buf[2];
        dst_ptr[7] = bit_byte_pool.byte_buf[3];

        src_ptr += sizeof(uint32_t);
        dst_ptr += 8u;
        num_elements -= 8u;
    }
    uint8_t src = *src_ptr;
    i = 0u;
    while (num_elements >= 2u)
    {
        src = *src_ptr;
        dst_ptr[i] = (src & mask);
        src = src >> 4u;
        dst_ptr[i + 1] = (src & mask);
        ++src_ptr;

        num_elements -= 2u;
        i += 2u;
    }

    if (num_elements > 0)
    {
        src = *src_ptr;
        dst_ptr[i] = (src & mask);
    }
#endif
}

// ********************** 5u ****************************** //

OWN_QPLC_FUN(void, qplc_unpack_5u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_5u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    uint64_t bit_mask0 = 0x00001f000000001fLLU;
    uint64_t bit_mask1 = 0x000000001f000000LLU;
    uint64_t bit_mask2 = 0x001f000000001f00LLU;
    uint64_t bit_mask3 = 0x0000001f00000000LLU;
    uint64_t bit_mask4 = 0x1f000000001f0000LLU;
    uint16_t mask = OWN_5_BIT_MASK;

    // Align to byte boundary
    if (0u < start_bit)
    {
        uint16_t next_byte;
        uint32_t bits_in_buf = OWN_BYTE_WIDTH - start_bit;
        uint16_t src = (uint16_t)((*src_ptr) >> start_bit);
        src_ptr++;
        while (0u < bits_in_buf)
        {
            if (5u > bits_in_buf)
            {
                next_byte = (uint16_t)(*src_ptr);
                src_ptr++;
                next_byte = next_byte << bits_in_buf;
                src = src | next_byte;
                bits_in_buf += OWN_BYTE_WIDTH;
            }
            *dst_ptr = (uint8_t)(src & mask);
            src = src >> 5u;
            bits_in_buf -= 5u;
            dst_ptr++;
            num_elements--;
            if (0u == num_elements)
            {
                return;
            }
        }
    }
    while (num_elements > 16u)
    {
        qplc_bit_byte_pool64_t bit_byte_pool64;
        uint64_t *tmp_src64 = (uint64_t *)src_ptr;
        uint64_t src64 = *tmp_src64;
        src_ptr = src_ptr + sizeof(uint64_t);
        uint16_t *tmp_src16 = (uint16_t *)src_ptr;
        uint16_t src16 = *tmp_src16;
        src_ptr = src_ptr + sizeof(uint16_t);

        bit_byte_pool64.bit_buf = src64 & bit_mask0;
        dst_ptr[0] = bit_byte_pool64.byte_buf[0];
        dst_ptr[8] = bit_byte_pool64.byte_buf[5];
        bit_byte_pool64.bit_buf = (src64 >> 1u) & bit_mask1;
        dst_ptr[5] = bit_byte_pool64.byte_buf[3];
        bit_byte_pool64.bit_buf = (src64 >> 2u) & bit_mask2;
        dst_ptr[2] = bit_byte_pool64.byte_buf[1];
        dst_ptr[10] = bit_byte_pool64.byte_buf[6];
        bit_byte_pool64.bit_buf = (src64 >> 3u) & bit_mask3;
        dst_ptr[7] = bit_byte_pool64.byte_buf[4];
        bit_byte_pool64.bit_buf = (src64 >> 4u) & bit_mask4;
        dst_ptr[4] = bit_byte_pool64.byte_buf[2];
        dst_ptr[12] = bit_byte_pool64.byte_buf[7] | (((uint8_t)(src16 & 1u)) << 4u);
        bit_byte_pool64.bit_buf = (src64 >> 5u) & bit_mask0;
        dst_ptr[1] = bit_byte_pool64.byte_buf[0];
        dst_ptr[9] = bit_byte_pool64.byte_buf[5];
        bit_byte_pool64.bit_buf = (src64 >> 6u) & bit_mask1;
        dst_ptr[6] = bit_byte_pool64.byte_buf[3];
        bit_byte_pool64.bit_buf = (src64 >> 7u) & bit_mask2;
        dst_ptr[3] = bit_byte_pool64.byte_buf[1];
        dst_ptr[11] = bit_byte_pool64.byte_buf[6];
        dst_ptr[13] = (uint8_t)((src16 >> 1u) & mask);
        dst_ptr[14] = (uint8_t)((src16 >> 6u) & mask);
        dst_ptr[15] = (uint8_t)((src16 >> 11u) & mask);
        dst_ptr += 16u;
        num_elements -= 16u;
    }
    if (num_elements > 8u)
    {
        uint32_t *tmp_src32 = (uint32_t *)src_ptr;
        uint32_t src32 = (*tmp_src32);
        src_ptr += sizeof(uint32_t);
        uint8_t src8 = *src_ptr;
        src_ptr++;

        dst_ptr[0] = (uint8_t)(src32 & (uint32_t)mask);
        dst_ptr[1] = (uint8_t)((src32 >> 5u) & (uint32_t)mask);
        dst_ptr[2] = (uint8_t)((src32 >> 10u) & (uint32_t)mask);
        dst_ptr[3] = (uint8_t)((src32 >> 15u) & (uint32_t)mask);
        dst_ptr[4] = (uint8_t)((src32 >> 20u) & (uint32_t)mask);
        dst_ptr[5] = (uint8_t)((src32 >> 25u) & (uint32_t)mask);
        dst_ptr[6] = (uint8_t)((src32 >> 30u) & (uint32_t)mask) | ((src8 << 2u) & (uint8_t)mask);
        dst_ptr[7] = ((src8 >> 3u) & (uint8_t)mask);

        dst_ptr += 8u;
        num_elements -= 8u;
    }
    if (0u < num_elements)
    {
        uint16_t next_byte;
        uint32_t bits_in_buf = OWN_BYTE_WIDTH;
        uint16_t src = (uint16_t)(*src_ptr);
        src_ptr++;
        while (0u != num_elements)
        {
            if (5u > bits_in_buf)
            {
                next_byte = (uint16_t)(*src_ptr);
                src_ptr++;
                next_byte = next_byte << bits_in_buf;
                src = src | next_byte;
                bits_in_buf += OWN_BYTE_WIDTH;
            }
            *dst_ptr = (uint8_t)(src & mask);
            src = src >> 5u;
            bits_in_buf -= 5u;
            dst_ptr++;
            num_elements--;
        }
    }
#endif
}

#if PLATFORM < K0

OWN_QPLC_INLINE(void, qplc_unpack_Nu8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint16_t mask = OWN_BIT_MASK(bit_width);

    {
        uint16_t next_byte;
        uint32_t bits_in_buf = OWN_BYTE_WIDTH - start_bit;
        uint16_t src         = (uint16_t) ((*src_ptr) >> start_bit);
        src_ptr++;
        while (0u < num_elements) {
            if (bit_width > bits_in_buf) {
                next_byte = (uint16_t) (*src_ptr);
                src_ptr++;
                next_byte = next_byte << bits_in_buf;
                src       = src | next_byte;
                bits_in_buf += OWN_BYTE_WIDTH;
            }
            *dst_ptr = (uint8_t) (src & mask);
            src = src >> bit_width;
            bits_in_buf -= bit_width;
            dst_ptr++;
            num_elements--;
        }
    }
}

#endif

OWN_QPLC_FUN(void, qplc_unpack_6u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_6u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu8u(src_ptr, num_elements, start_bit, 6u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_7u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint8_t *dst_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_qplc_unpack_7u8u)(src_ptr, num_elements, start_bit, dst_ptr);
#else
    qplc_unpack_Nu8u(src_ptr, num_elements, start_bit, 7u, dst_ptr);
#endif
}

OWN_QPLC_FUN(void, qplc_unpack_8u8u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t UNREFERENCED_PARAMETER(start_bit),
        uint8_t *dst_ptr)) {
    CALL_CORE_FUN(qplc_copy_8u)(src_ptr, dst_ptr, num_elements);
}
