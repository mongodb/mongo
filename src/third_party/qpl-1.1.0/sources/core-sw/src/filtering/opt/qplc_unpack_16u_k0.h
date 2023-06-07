/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of functions for unpacking 9..16-bit data to words
 * @date 08/02/2020
 *
 * @details Function list:
 *          - @ref k0_qplc_unpack_9u16u
 *          - @ref k0_qplc_unpack_10u16u
 *          - @ref k0_qplc_unpack_11u16u
 *          - @ref k0_qplc_unpack_12u16u
 *          - @ref k0_qplc_unpack_13u16u
 *          - @ref k0_qplc_unpack_14u16u
 *          - @ref k0_qplc_unpack_15u16u
 *          - @ref k0_qplc_unpack_16u16u
 *
 */

#include "own_qplc_defs.h"

// ------------------------------------ 9u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_0[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u, 14u, 14u, 15u, 15u, 16u, 16u, 17u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_9u_1[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 14u, 15u, 15u, 16u, 16u, 17u, 17u, 18u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_9u_0[16]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u, 0u, 2u, 4u, 6u, 8u, 10u, 12u, 14u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_9u_1[16]) = {
    7u, 5u, 3u, 1u, 15u, 13u, 11u, 9u, 7u, 5u, 3u, 1u, 15u, 13u, 11u, 9u};

// ------------------------------------ 10u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_0[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 10u, 11u, 11u, 12u, 12u, 13u, 13u, 14u, 15u, 16u, 16u, 17u, 17u, 18u, 18u, 19u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_1[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 13u, 14u, 14u, 15u, 15u, 16u, 16u, 17u, 18u, 19u, 19u, 20u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_10u_0[16]) = {
    0u, 4u, 8u, 12u, 0u, 4u, 8u, 12u, 0u, 4u, 8u, 12u, 0u, 4u, 8u, 12u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_10u_1[16]) = {
    6u, 2u, 14u, 10u, 6u, 2u, 14u, 10u, 6u, 2u, 14u, 10u, 6u, 2u, 14u, 10u};

// ------------------------------------ 11u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_0[32]) = {
    0u, 1u, 1u, 2u, 2u, 3u, 4u, 5u, 5u, 6u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u, 12u, 13u, 13u, 14u, 15u, 16u, 16u, 17u, 17u, 18u, 19u, 20u, 20u, 21u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_11u_1[32]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 4u, 5u, 6u, 7u, 7u, 8u, 8u, 9u, 10u, 11u, 11u, 12u, 13u, 14u, 14u, 15u, 15u, 16u, 17u, 18u, 18u, 19u, 19u, 20u, 21u, 22u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_0[16]) = {
    0u, 6u, 12u, 2u, 8u, 14u, 4u, 10u, 0u, 6u, 12u, 2u, 8u, 14u, 4u, 10u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_11u_1[16]) = {
    5u, 15u, 9u, 3u, 13u, 7u, 1u, 11u, 5u, 15u, 9u, 3u, 13u, 7u, 1u, 11u};

// ------------------------------------ 12u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 6u, 7u, 7u, 8u, 9u, 10u, 10u, 11u, 12u, 13u, 13u, 14u, 15u, 16u, 16u, 17u, 18u, 19u, 19u, 20u, 21u, 22u, 22u, 23u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_1[32]) = {
    0u, 1u, 2u, 3u, 3u, 4u, 5u, 6u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u, 12u, 13u, 14u, 15u, 15u, 16u, 17u, 18u, 18u, 19u, 20u, 21u, 21u, 22u, 23u, 24u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_12u_0[16]) = {
    0u, 8u, 0u, 8u, 0u, 8u, 0u, 8u, 0u, 8u, 0u, 8u, 0u, 8u, 0u, 8u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_12u_1[16]) = {
    4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u, 4u, 12u};

// ------------------------------------ 13u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 4u, 5u, 6u, 7u, 8u, 9u, 9u, 10u, 11u, 12u,
    13u, 14u, 14u, 15u, 16u, 17u, 17u, 18u, 19u, 20u, 21u, 22u, 22u, 23u, 24u, 25u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_13u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u, 12u, 13u,
    13u, 14u, 15u, 16u, 17u, 18u, 18u, 19u, 20u, 21u, 21u, 22u, 23u, 24u, 25u, 26u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_0[16]) = {
    0u, 10u, 4u, 14u, 8u, 2u, 12u, 6u, 0u, 10u, 4u, 14u, 8u, 2u, 12u, 6u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_13u_1[16]) = {
    3u, 9u, 15u, 5u, 11u, 1u, 7u, 13u, 3u, 9u, 15u, 5u, 11u, 1u, 7u, 13u};

// ------------------------------------ 14u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 22u, 23u, 24u, 25u, 26u, 27u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 14u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_14u_0[16]) = {
    0u, 12u, 8u, 4u, 0u, 12u, 8u, 4u, 0u, 12u, 8u, 4u, 0u, 12u, 8u, 4u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_14u_1[16]) = {
    2u, 6u, 10u, 14u, 2u, 6u, 10u, 14u, 2u, 6u, 10u, 14u, 2u, 6u, 10u, 14u};

// ------------------------------------ 15u -----------------------------------------
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_0[32]) = {
    0u, 1u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_15u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 15u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_0[16]) = {
    0u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0u, 14u, 12u, 10u, 8u, 6u, 4u, 2u};
OWN_ALIGNED_64_ARRAY(static uint32_t shift_table_15u_1[16]) = {
    1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u, 1u, 3u, 5u, 7u, 9u, 11u, 13u, 15u};

OWN_QPLC_INLINE(uint32_t, own_get_align, (uint32_t start_bit, uint32_t base, uint32_t bitsize)) {
    uint32_t remnant = bitsize - start_bit;
    uint32_t ret_value = 0xFFFFFFFF;
    for (uint32_t i = 0u; i < bitsize; ++i) {
        uint32_t test_value = (i * base) % bitsize;
        if (test_value == remnant) {
            ret_value = i;
            break;
        }
    }
    return ret_value;
}

OWN_QPLC_INLINE(void, px_qplc_unpack_Nu16u, (const uint8_t *src_ptr,
        uint32_t num_elements,
        uint32_t start_bit,
        uint32_t bit_width,
        uint8_t *dst_ptr)) {
    uint32_t mask        = OWN_BIT_MASK(bit_width);
    uint32_t next_word;
    uint32_t bits_in_buf = OWN_WORD_WIDTH - start_bit;
    uint16_t *src16u_ptr = (uint16_t *) src_ptr;
    uint16_t *dst16u_ptr = (uint16_t *) dst_ptr;
    uint32_t src         = (uint32_t) ((*src16u_ptr) >> start_bit);
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
        next_word = (uint32_t)(bit_width - bits_in_buf > 8u ? *src16u_ptr : *((uint8_t *)src16u_ptr));
        next_word = next_word << bits_in_buf;
        src = src | next_word;
    }
    *dst16u_ptr = (uint16_t)(src & mask);
}

OWN_OPT_FUN(void, k0_qplc_unpack_9u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 9u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 9u, dst_ptr);
        src_ptr += ((align * 9u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(9u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(9u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_9u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_9u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_9u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_9u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 9u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 9u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_10u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 10u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 10u, dst_ptr);
        src_ptr += ((align * 10u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(10u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(10u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_10u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_10u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_10u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_10u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 10u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 10u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_11u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 11u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 11u, dst_ptr);
        src_ptr += ((align * 11u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(11u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(11u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_11u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_11u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_11u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_11u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 11u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 11u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_12u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 12u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 12u, dst_ptr);
        src_ptr += ((align * 12u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(12u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(12u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_12u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_12u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_12u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_12u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 12u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 12u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_13u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 13u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 13u, dst_ptr);
        src_ptr += ((align * 13u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(13u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(13u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_13u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_13u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_13u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_13u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 13u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 13u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_14u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 14u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 14u, dst_ptr);
        src_ptr += ((align * 14u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(14u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(14u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_14u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_14u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_14u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_14u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 14u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 14u, dst_ptr);
    }
}

OWN_OPT_FUN(void, k0_qplc_unpack_15u16u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint32_t start_bit,
    uint8_t *dst_ptr)) {
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 15u, 16u);
        if (align > num_elements) {
            align = num_elements;
        }
        px_qplc_unpack_Nu16u(src_ptr, align, start_bit, 15u, dst_ptr);
        src_ptr += ((align * 15u) + start_bit) >> 3;
        dst_ptr += align * 2;
        num_elements -= align;
    }

    if (num_elements >= 32u) {
        __mmask32 read_mask = OWN_BIT_MASK(OWN_BITS_2_WORD(15u * OWN_DWORD_WIDTH));
        __m512i   parse_mask0 = _mm512_set1_epi16(OWN_BIT_MASK(15u));

        __m512i   permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_15u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_15u_1);

        __m512i   shift_mask_ptr[2];
        shift_mask_ptr[0] = _mm512_load_si512(shift_table_15u_0);
        shift_mask_ptr[1] = _mm512_load_si512(shift_table_15u_1);

        while (num_elements >= 32u) {
            __m512i srcmm, zmm[2];

            srcmm = _mm512_maskz_loadu_epi16(read_mask, src_ptr);

            // permuting so in zmm[0] will be elements with even indexes and in zmm[1] - with odd ones
            zmm[0] = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm);
            zmm[1] = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm);

            // shifting elements so they start from the start of the word
            zmm[0] = _mm512_srlv_epi32(zmm[0], shift_mask_ptr[0]);
            zmm[1] = _mm512_sllv_epi32(zmm[1], shift_mask_ptr[1]);

            // gathering even and odd elements together
            zmm[0] = _mm512_mask_mov_epi16(zmm[0], 0xAAAAAAAA, zmm[1]);
            zmm[0] = _mm512_and_si512(zmm[0], parse_mask0);

            _mm512_storeu_si512(dst_ptr, zmm[0]);

            src_ptr += 4u * 15u;
            dst_ptr += 64u;
            num_elements -= 32u;
        }
    }

    if (num_elements > 0) {
        px_qplc_unpack_Nu16u(src_ptr, num_elements, 0u, 15u, dst_ptr);
    }
}
