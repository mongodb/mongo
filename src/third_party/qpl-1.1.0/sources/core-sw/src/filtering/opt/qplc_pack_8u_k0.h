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
  *          - @ref k0_qplc_pack_8u1u
  *          - @ref k0_qplc_pack_8u2u
  *          - @ref k0_qplc_pack_8u3u
  *          - @ref k0_qplc_pack_8u4u
  *          - @ref k0_qplc_pack_8u5u
  *          - @ref k0_qplc_pack_8u6u
  *          - @ref k0_qplc_pack_8u7u
  *          - @ref k0_qplc_pack_8u8u
  *          - @ref k0_qplc_pack_8u16u
  *          - @ref k0_qplc_pack_8u32u
  *
  */
#ifndef OWN_PACK_8U_H
#define OWN_PACK_8U_H

#include "own_qplc_defs.h"
#include "qplc_memop.h"

// ----------------------- 8u2u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint32_t permutex_idx_table_2u[16]) = {
    0u, 4u, 8u, 12u, 1u, 5u, 9u, 13u, 2u, 6u, 10u, 14u, 3u, 7u, 11u, 15u};

// ----------------------- 8u3u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_3u_0[32]) = {
    1u, 2u, 3u, 9u, 10u, 11u, 17u, 18u, 19u, 25u, 26u, 27u, 5u, 6u, 7u, 13u,
    14u, 15u, 21u, 22u, 23u, 29u, 30u, 31u, 33u, 34u, 35u, 41u, 42u, 43u, 49u, 50u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_3u_1[32]) = {
    0u, 1u, 2u, 8u, 9u, 10u, 16u, 17u, 18u, 24u, 25u, 26u, 4u, 5u, 6u, 12u,
    13u, 14u, 20u, 21u, 22u, 28u, 29u, 30u, 32u, 33u, 34u, 40u, 41u, 42u, 48u, 49u};

// ----------------------- 16u10u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_0[32]) = {
    0u, 2u, 0x0, 5u, 7u, 8u, 10u, 0x0, 13u, 15u, 16u, 18u, 0x0, 21u, 23u, 24u,
    26u, 0x0, 29u, 31u, 32u, 34u, 0x0, 37u, 39u, 40u, 42u, 0x0, 45u, 47u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_1[32]) = {
    1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0, 25u,
    27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u, 43u, 44u, 46u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_2[32]) = {
    0x0, 1u, 3u, 4u, 6u, 0x0, 9u, 11u, 12u, 14u, 0x0, 17u, 19u, 20u, 22u, 0x0,
    25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u, 43u, 44u, 46u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_3[32]) = {
    16u, 18u, 0x0, 21u, 23u, 24u, 26u, 0x0, 29u, 31u, 32u, 34u, 0x0, 37u, 39u, 40u,
    42u, 0x0, 45u, 47u, 48u, 50u, 0x0, 53u, 55u, 56u, 58u, 0x0, 61u, 63u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_4[32]) = {
    17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0, 41u,
    43u, 44u, 46u, 0x0, 49u, 51u, 52u, 54u, 0x0, 57u, 59u, 60u, 62u, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_10u_5[32]) = {
    0x0, 17u, 19u, 20u, 22u, 0x0, 25u, 27u, 28u, 30u, 0x0, 33u, 35u, 36u, 38u, 0x0,
    41u, 43u, 44u, 46u, 0x0, 49u, 51u, 52u, 54u, 0x0, 57u, 59u, 60u, 62u, 0x0, 0x0};
static __mmask32 permutex_masks_10u_ptr[3] = {0x37BDEF7B, 0x1EF7BDEF, 0x3DEF7BDE};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_0[32]) = {
    0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u,
    4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0u, 4u, 0u, 2u, 6u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_1[32]) = {
    10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u,
    14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 10u, 14u, 8u, 12u, 0u, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_10u_2[32]) = {
    0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u,
    6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0u, 6u, 2u, 8u, 4u, 0x0, 0x0};

// ----------------------- 16u12u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_0[32]) = {
    1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u,
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_1[32]) = {
    0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u,
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_2[32]) = {
    11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u, 22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u,
    33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u, 43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_3[32]) = {
    10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u,
    32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u, 42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_4[32]) = {
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u,
    43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u, 54u, 55u, 57u, 58u, 59u, 61u, 62u, 63u};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_12u_5[32]) = {
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u,
    42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u, 53u, 54u, 56u, 57u, 58u, 60u, 61u, 62u};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_0[32]) = {
    12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u,
    8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_1[32]) = {
    0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u,
    4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_2[32]) = {
    4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u,
    12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_3[32]) = {
    8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u,
    0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_4[32]) = {
    8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u,
    4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u, 12u, 8u, 4u};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_12u_5[32]) = {
    4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u,
    8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u, 0u, 4u, 8u};

// ----------------------- 16u14u ------------------------------ //
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_0[32]) = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u, 18u,
    19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t permutex_idx_table_14u_1[32]) = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u,
    18u, 19u, 20u, 21u, 22u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 0x0, 0x0, 0x0, 0x0};

OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_14u_0[32]) = {
    14u, 12u, 10u, 8u, 6u, 4u, 2u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 14u, 12u,
    10u, 8u, 6u, 4u, 2u, 14u, 12u, 10u, 8u, 6u, 4u, 2u, 0x0, 0x0, 0x0, 0x0};
OWN_ALIGNED_64_ARRAY(static uint16_t shift_mask_table_14u_1[32]) = {
    0u, 2u, 4u, 6u, 8u, 10u, 12u, 0u, 2u, 4u, 6u, 8u, 10u, 12u, 0u, 2u,
    4u, 6u, 8u, 10u, 12u, 0u, 2u, 4u, 6u, 8u, 10u, 12, 0x0, 0x0, 0x0, 0x0};


OWN_QPLC_INLINE(void, k0_qplc_pack_8u16u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m256i srcmm;
    __m512i dstmm;

    __mmask32 tail_mask = OWN_BIT_MASK(num_elements);
    srcmm = _mm256_maskz_loadu_epi8(tail_mask, (const __m256i *)src_ptr);
    dstmm = _mm512_maskz_cvtepu8_epi16(tail_mask, srcmm);
    _mm512_mask_storeu_epi16(dst_ptr, tail_mask, dstmm);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u16u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m256i srcmm0, srcmm1;
    __m512i dstmm0, dstmm1;

    while (num_elements > 64u)
    {
        srcmm0 = _mm256_loadu_si256((const __m256i *)src_ptr);
        srcmm1 = _mm256_loadu_si256((const __m256i *)(src_ptr + 32u));

        dstmm0 = _mm512_cvtepu8_epi16(srcmm0);
        dstmm1 = _mm512_cvtepu8_epi16(srcmm1);

        _mm512_storeu_si512(dst_ptr, dstmm0);
        _mm512_storeu_si512((dst_ptr + 64u), dstmm1);

        num_elements -= 64u;
        src_ptr += 64u;
        dst_ptr += 128u;
    }

    if (num_elements > 32u)
    {
        srcmm0 = _mm256_loadu_si256((const __m256i *)src_ptr);
        dstmm0 = _mm512_cvtepu8_epi16(srcmm0);
        _mm512_storeu_si512(dst_ptr, dstmm0);

        num_elements -= 32u;
        src_ptr += 32u;
        dst_ptr += 64u;
    }

    k0_qplc_pack_8u16u_tail(src_ptr, num_elements, dst_ptr);
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u32u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m128i srcmm;
    __m512i dstmm;

    __mmask16 tail_mask = OWN_BIT_MASK(num_elements);
    srcmm = _mm_maskz_loadu_epi8(tail_mask, (const __m128i *)src_ptr);
    dstmm = _mm512_maskz_cvtepu8_epi32(tail_mask, srcmm);
    _mm512_mask_storeu_epi32(dst_ptr, tail_mask, dstmm);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u32u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m128i srcmm;
    __m512i dstmm;

    while (num_elements > 16u)
    {
        srcmm = _mm_loadu_si128((const __m128i *)src_ptr);
        dstmm = _mm512_cvtepu8_epi32(srcmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        num_elements -= 16u;
        src_ptr += 16u;
        dst_ptr += 64u;
    }

    k0_qplc_pack_8u32u_tail(src_ptr, num_elements, dst_ptr);
}

OWN_QPLC_INLINE(uint32_t, own_get_align, (uint32_t start_bit, uint32_t base, uint32_t bitsize)) {
    uint32_t    remnant = bitsize - start_bit;
    uint32_t    ret_value = 0xFFFFFFFF;
    for (uint32_t i = 0u; i < bitsize; ++i) {
        uint32_t test_value = (i * base) % bitsize;
        if (test_value == remnant) {
            ret_value = i;
            break;
        }
    }
    return ret_value;
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u1u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u < num_elements) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit++;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u1u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0;
    __mmask64 tail_mask = OWN_BIT_MASK(num_elements);

    srcmm0 = _mm512_maskz_loadu_epi8(tail_mask, src_ptr);
    uint32_t num_bytes = OWN_BITS_2_BYTE(num_elements);
    uint8_t result[8];
    *(uint64_t *)result = _mm512_mask_cmpgt_epi8_mask(tail_mask, srcmm0, _mm512_setzero_si512());
    for (uint32_t i = 0u; i < num_bytes; ++i) {
        dst_ptr[i] = result[i];
    }
}

OWN_OPT_FUN(void, k0_qplc_pack_8u1u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = 8u - start_bit;
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u1u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += (align + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u)
    {
        uint32_t num_elements_1024 = num_elements / 1024u;
        uint32_t num_elements_256 = (num_elements % 1024u) / 256u;
        uint32_t num_elements_64 = (num_elements % 256u) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3, srcmm4, srcmm5, srcmm6, srcmm7;
        __m512i srcmm8, srcmm9, srcmm10, srcmm11, srcmm12, srcmm13, srcmm14, srcmm15;

        for (uint32_t idx = 0u; idx < num_elements_1024; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);
            srcmm4 = _mm512_loadu_si512(src_ptr + 256u);
            srcmm5 = _mm512_loadu_si512(src_ptr + 320u);
            srcmm6 = _mm512_loadu_si512(src_ptr + 384u);
            srcmm7 = _mm512_loadu_si512(src_ptr + 448u);
            srcmm8 = _mm512_loadu_si512(src_ptr + 512u);
            srcmm9 = _mm512_loadu_si512(src_ptr + 576u);
            srcmm10 = _mm512_loadu_si512(src_ptr + 640u);
            srcmm11 = _mm512_loadu_si512(src_ptr + 704u);
            srcmm12 = _mm512_loadu_si512(src_ptr + 768u);
            srcmm13 = _mm512_loadu_si512(src_ptr + 832u);
            srcmm14 = _mm512_loadu_si512(src_ptr + 896u);
            srcmm15 = _mm512_loadu_si512(src_ptr + 960u);

            *(uint64_t *)dst_ptr = _mm512_cmpgt_epi8_mask(srcmm0, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 8u) = _mm512_cmpgt_epi8_mask(srcmm1, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 16u) = _mm512_cmpgt_epi8_mask(srcmm2, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 24u) = _mm512_cmpgt_epi8_mask(srcmm3, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 32u) = _mm512_cmpgt_epi8_mask(srcmm4, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 40u) = _mm512_cmpgt_epi8_mask(srcmm5, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 48u) = _mm512_cmpgt_epi8_mask(srcmm6, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 56u) = _mm512_cmpgt_epi8_mask(srcmm7, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 64u) = _mm512_cmpgt_epi8_mask(srcmm8, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 72u) = _mm512_cmpgt_epi8_mask(srcmm9, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 80u) = _mm512_cmpgt_epi8_mask(srcmm10, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 88u) = _mm512_cmpgt_epi8_mask(srcmm11, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 96u) = _mm512_cmpgt_epi8_mask(srcmm12, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 104u) = _mm512_cmpgt_epi8_mask(srcmm13, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 112u) = _mm512_cmpgt_epi8_mask(srcmm14, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 120u) = _mm512_cmpgt_epi8_mask(srcmm15, _mm512_setzero_si512());

            src_ptr += 1024;
            dst_ptr += 1u * 128u;
        }

        for (uint32_t idx = 0u; idx < num_elements_256; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            *(uint64_t *)dst_ptr = _mm512_cmpgt_epi8_mask(srcmm0, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 8u) = _mm512_cmpgt_epi8_mask(srcmm1, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 16u) = _mm512_cmpgt_epi8_mask(srcmm2, _mm512_setzero_si512());
            *(uint64_t *)(dst_ptr + 24u) = _mm512_cmpgt_epi8_mask(srcmm3, _mm512_setzero_si512());

            src_ptr += 256u;
            dst_ptr += 1u * 32u;
        }

        for (uint32_t idx = 0u; idx < num_elements_64; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            *(uint64_t *)dst_ptr = _mm512_cmpgt_epi8_mask(srcmm0, _mm512_setzero_si512());
            src_ptr += 64u;
            dst_ptr += 1u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u1u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u2u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    while (0u < num_elements) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit += 2u;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u2u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __mmask64 tail_mask = OWN_BIT_MASK(num_elements);
    __mmask16 store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 2u));

    __m512i srcmm0 = _mm512_maskz_loadu_epi8(tail_mask, src_ptr);

    // uniting each two 2u(8u) to one 4u(16u)
    __m512i zmm0 = _mm512_srli_epi16(srcmm0, 6u);
    zmm0 = _mm512_or_si512(srcmm0, zmm0);

    // and then two 4u(16u) to one 8u(32u)
    __m512i zmm1 = _mm512_srli_epi32(zmm0, 12u);
    zmm0 = _mm512_or_si512(zmm1, zmm0);

    _mm512_mask_cvtepi32_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u2u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 2u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u2u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += ((align * 2u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    
    if (num_elements >= 64u) {
        uint32_t num_elements_256 = num_elements / 256u;
        uint32_t num_elements_64 = (num_elements % 256u) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7;

        __m512i permutex_idx_ptr = _mm512_load_si512(permutex_idx_table_2u);

        for (uint32_t idx = 0; idx < num_elements_256; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            // uniting each two 2u(8u) to one 4u(16u)
            zmm0 = _mm512_srli_epi16(srcmm0, 6u);
            zmm1 = _mm512_maskz_mov_epi8(0x5555555555555555, srcmm0);
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm2 = _mm512_srli_epi16(srcmm1, 6u);
            zmm3 = _mm512_maskz_mov_epi8(0x5555555555555555, srcmm1);
            zmm2 = _mm512_or_si512(zmm2, zmm3);
            zmm4 = _mm512_srli_epi16(srcmm2, 6u);
            zmm5 = _mm512_maskz_mov_epi8(0x5555555555555555, srcmm2);
            zmm4 = _mm512_or_si512(zmm4, zmm5);
            zmm6 = _mm512_srli_epi16(srcmm3, 6u);
            zmm7 = _mm512_maskz_mov_epi8(0x5555555555555555, srcmm3);
            zmm6 = _mm512_or_si512(zmm6, zmm7);

            zmm0 = _mm512_packus_epi16(zmm0, zmm2);
            zmm4 = _mm512_packus_epi16(zmm4, zmm6);

            zmm2 = _mm512_srli_epi16(zmm0, 4u);
            zmm1 = _mm512_maskz_mov_epi8(0x5555555555555555, zmm0);
            zmm0 = _mm512_or_si512(zmm1, zmm2);
            zmm6 = _mm512_srli_epi16(zmm4, 4u);
            zmm5 = _mm512_maskz_mov_epi8(0x5555555555555555, zmm4);
            zmm4 = _mm512_or_si512(zmm5, zmm6);

            zmm0 = _mm512_packus_epi16(zmm0, zmm4);
            zmm0 = _mm512_permutexvar_epi32(permutex_idx_ptr, zmm0);

            _mm512_storeu_si512(dst_ptr, zmm0);

            src_ptr += 256u;
            dst_ptr += 2u * 32u;
        }

        for (uint32_t idx = 0; idx < num_elements_64; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);

            // uniting each two 2u(8u) to one 4u(16u)
            zmm0 = _mm512_srli_epi16(srcmm0, 6u);
            zmm0 = _mm512_or_si512(srcmm0, zmm0);

            // and then two 4u(16u) to one 8u(32u)
            zmm1 = _mm512_srli_epi32(zmm0, 12u);
            zmm0 = _mm512_or_si512(zmm1, zmm0);

            _mm512_mask_cvtepi32_storeu_epi8(dst_ptr, 0xFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 2u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u2u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u3u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u < num_elements) {
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
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u3u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0, srcmm1;
    __m512i zmm0, zmm1, zmm2;
    __mmask64 read_mask0, read_mask1, tail_mask, store_mask;

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_12u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_12u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_12u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_12u_1);

    tail_mask = OWN_BIT_MASK(num_elements);
    read_mask0 = 0x5555555555555555 & tail_mask;
    read_mask1 = 0xAAAAAAAAAAAAAAAA & tail_mask;
    store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 3u));

    srcmm0 = _mm512_maskz_loadu_epi8(read_mask0, src_ptr);
    srcmm1 = _mm512_maskz_loadu_epi8(read_mask1, src_ptr);

    // uniting each two 3u(8u) to one 6u(16u)
    zmm0 = _mm512_srli_epi16(srcmm1, 5u);
    zmm0 = _mm512_or_si512(srcmm0, zmm0);

    // uniting each two 6u(16u) to one 12u(32u)
    zmm1 = _mm512_srli_epi32(zmm0, 10u);
    zmm1 = _mm512_or_si512(zmm0, zmm1);

    zmm2 = _mm512_castsi256_si512(_mm512_cvtepi32_epi16(zmm1));

    // pack_16u12u optimization
    zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
    zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

    zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u3u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 3u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u3u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += (align * 3 + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u)
    {
        uint32_t num_elements_128 = num_elements / 128u;
        uint32_t num_elements_64 = (num_elements % 128u) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;


        __m512i permutex_idx_ptr[4];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_3u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_3u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_12u_0);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_12u_1);

        __m512i shift_masks_ptr[2];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_12u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_12u_1);

        for (uint32_t idx = 0; idx < num_elements_128; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);
            srcmm2 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr + 64u);
            srcmm3 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr + 64u);

            // uniting each two 3u(8u) to one 6u(16u)
            zmm0 = _mm512_srli_epi16(srcmm1, 5u);
            zmm0 = _mm512_or_si512(srcmm0, zmm0);
            zmm3 = _mm512_srli_epi16(srcmm3, 5u);
            zmm3 = _mm512_or_si512(srcmm2, zmm3);

            // uniting each two 6u(16u) to one 12u(32u)
            zmm1 = _mm512_srli_epi32(zmm0, 10u);
            zmm2 = _mm512_maskz_mov_epi16(0x55555555, zmm0);
            zmm1 = _mm512_or_si512(zmm2, zmm1);
            zmm4 = _mm512_srli_epi32(zmm3, 10u);
            zmm5 = _mm512_maskz_mov_epi16(0x55555555, zmm3);
            zmm4 = _mm512_or_si512(zmm5, zmm4);

            zmm2 = _mm512_packus_epi32(zmm1, zmm4);

            // pack_16u12u optimization
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x00FFFFFF, zmm0);

            src_ptr += 128u;
            dst_ptr += 3u * 16u;
        }

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);

            // uniting each two 3u(8u) to one 6u(16u)
            zmm0 = _mm512_srli_epi16(srcmm1, 5u);
            zmm0 = _mm512_or_si512(srcmm0, zmm0);

            // uniting each two 6u(16u) to one 12u(32u)
            zmm1 = _mm512_srli_epi32(zmm0, 10u);
            zmm1 = _mm512_or_si512(zmm0, zmm1);

            zmm2 = _mm512_castsi256_si512(_mm512_cvtepi32_epi16(zmm1));

            // pack_16u12u optimization
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[2], zmm2);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[3], zmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x00000FFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 3u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u3u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u4u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u < num_elements) {
        *dst_ptr |= *src_ptr << start_bit;
        num_elements--;
        src_ptr++;
        start_bit += 4u;
        if (OWN_BYTE_WIDTH == start_bit) {
            dst_ptr++;
            break;
        }
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u4u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __mmask64 tail_mask;
    __mmask32 store_mask;
    tail_mask = OWN_BIT_MASK(num_elements);
    store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 4u));
    __m512i srcmm0;
    __m512i zmm0;

    srcmm0 = _mm512_maskz_loadu_epi8(tail_mask, src_ptr);

    // uniting each two 4u(8u) to one 8u(16u)
    zmm0 = _mm512_srli_epi16(srcmm0, 4u);
    zmm0 = _mm512_or_si512(srcmm0, zmm0);

    _mm512_mask_cvtepi16_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u4u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 4u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u4u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += ((align * 4u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u) {
        uint32_t num_elements_256 = num_elements / 256u;
        uint32_t num_elements_64 = (num_elements % 256) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3;

        for (uint32_t idx = 0; idx < num_elements_256; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);
            srcmm1 = _mm512_loadu_si512(src_ptr + 64u);
            srcmm2 = _mm512_loadu_si512(src_ptr + 128u);
            srcmm3 = _mm512_loadu_si512(src_ptr + 192u);

            // uniting each two 4u(8u) to one 8u(16u)
            zmm0 = _mm512_srli_epi16(srcmm0, 4u);
            zmm0 = _mm512_or_si512(srcmm0, zmm0);
            zmm1 = _mm512_srli_epi16(srcmm1, 4u);
            zmm1 = _mm512_or_si512(srcmm1, zmm1);
            zmm2 = _mm512_srli_epi16(srcmm2, 4u);
            zmm2 = _mm512_or_si512(srcmm2, zmm2);
            zmm3 = _mm512_srli_epi16(srcmm3, 4u);
            zmm3 = _mm512_or_si512(srcmm3, zmm3);

            _mm512_mask_cvtepi16_storeu_epi8(dst_ptr, 0xFFFFFFFF, zmm0);
            _mm512_mask_cvtepi16_storeu_epi8(dst_ptr + 32u, 0xFFFFFFFF, zmm1);
            _mm512_mask_cvtepi16_storeu_epi8(dst_ptr + 64u, 0xFFFFFFFF, zmm2);
            _mm512_mask_cvtepi16_storeu_epi8(dst_ptr + 96u, 0xFFFFFFFF, zmm3);

            src_ptr += 256u;
            dst_ptr += 4u * 32u;
        }

        for (uint32_t idx = 0; idx < num_elements_64; ++idx) {
            srcmm0 = _mm512_loadu_si512(src_ptr);

            // uniting each two 4u(8u) to one 8u(16u)
            zmm0 = _mm512_srli_epi16(srcmm0, 4u);
            zmm0 = _mm512_or_si512(srcmm0, zmm0);

            _mm512_mask_cvtepi16_storeu_epi8(dst_ptr, 0xFFFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 4u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u4u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u5u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    dst_ptr[0] &= OWN_BIT_MASK(start_bit);
    while (0u < num_elements) {
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
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u5u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0, srcmm1;
    __m512i zmm0, zmm1, zmm2, zmm3;
    __mmask64 read_mask0, read_mask1, tail_mask, store_mask;

    __m512i permutex_idx_ptr[3];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_10u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_10u_1);
    permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_10u_2);

    __m512i shift_masks_ptr[3];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_10u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_10u_1);
    shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_10u_2);

    tail_mask = OWN_BIT_MASK(num_elements);
    read_mask0 = 0x5555555555555555 & tail_mask;
    read_mask1 = 0xAAAAAAAAAAAAAAAA & tail_mask;
    store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 5u));
    

    srcmm0 = _mm512_maskz_loadu_epi8(read_mask0, src_ptr);
    srcmm1 = _mm512_maskz_loadu_epi8(read_mask1, src_ptr);

    // uniting each two 5u(8u) to one 10u(16u)
    zmm3 = _mm512_srli_epi16(srcmm1, 3u);
    zmm3 = _mm512_or_si512(srcmm0, zmm3);

    // pack_16u10u optimization
    zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], zmm3);
    zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], zmm3);
    zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], zmm3);

    zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
    zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    zmm0 = _mm512_or_si512(zmm0, zmm2);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);

}

OWN_OPT_FUN(void, k0_qplc_pack_8u5u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 5u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u5u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += ((align * 5u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u)
    {
        uint32_t num_elements_192 = num_elements / 192u;
        uint32_t num_elements_64 = (num_elements % 192u) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3, srcmm4, srcmm5;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_10u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_10u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_10u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_10u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_10u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_10u_5);

        __m512i shift_masks_ptr[3];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_10u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_10u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_10u_2);

        for (uint32_t idx = 0; idx < num_elements_192; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);
            srcmm2 = _mm512_maskz_loadu_epi8(0x5555555555555555, (src_ptr + 64u));
            srcmm3 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, (src_ptr + 64u));
            srcmm4 = _mm512_maskz_loadu_epi8(0x5555555555555555, (src_ptr + 128u));
            srcmm5 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, (src_ptr + 128u));

            // uniting each two 5u(8u) to one 10u(16u)
            zmm6 = _mm512_srli_epi16(srcmm1, 3u);
            zmm6 = _mm512_or_si512(srcmm0, zmm6);
            zmm7 = _mm512_srli_epi16(srcmm3, 3u);
            zmm7 = _mm512_or_si512(srcmm2, zmm7);
            zmm8 = _mm512_srli_epi16(srcmm5, 3u);
            zmm8 = _mm512_or_si512(srcmm4, zmm8);

            // pack_16u10u optimization
            zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], zmm6, permutex_idx_ptr[0], zmm7);
            zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], zmm6, permutex_idx_ptr[1], zmm7);
            zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], zmm6, permutex_idx_ptr[2], zmm7);
            zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], zmm7, permutex_idx_ptr[3], zmm8);
            zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], zmm7, permutex_idx_ptr[4], zmm8);
            zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], zmm7, permutex_idx_ptr[5], zmm8);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_sllv_epi16(zmm3, shift_masks_ptr[0]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[1]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            zmm3 = _mm512_or_si512(zmm3, zmm4);
            zmm3 = _mm512_or_si512(zmm3, zmm5);

            _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);
            _mm512_mask_storeu_epi16((dst_ptr + 60u), 0x3FFFFFFF, zmm3);

            src_ptr += 192u;
            dst_ptr += 5u * 24u;
        }

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);

            // uniting each two 5u(8u) to one 10u(16u)
            zmm3 = _mm512_srli_epi16(srcmm1, 3u);
            zmm3 = _mm512_or_si512(srcmm0, zmm3);

            // pack_16u10u optimization
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], zmm3);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], zmm3);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], zmm3);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);
            _mm512_mask_storeu_epi16(dst_ptr, 0x000FFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 5u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u5u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u6u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 6u;
    uint32_t bits_in_buf = bit_width + start_bit;
    uint16_t src = (uint16_t)(*dst_ptr) & OWN_BIT_MASK(start_bit);
    src |= ((uint16_t)(*src_ptr)) << start_bit;
    src_ptr++;
    num_elements--;
    while (0u < num_elements) {
        *dst_ptr = (uint8_t)(src);
        dst_ptr++;
        src = src >> OWN_BYTE_WIDTH;
        bits_in_buf -= OWN_BYTE_WIDTH;
        src = src | (((uint16_t)(*src_ptr)) << bits_in_buf);
        src_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    *dst_ptr = (uint8_t)(src);
    if (OWN_BYTE_WIDTH < bits_in_buf) {
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
        *dst_ptr = (uint8_t)(src);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u6u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0, srcmm1;
    __m512i zmm0, zmm1, zmm2;
    __mmask64 read_mask0, read_mask1, tail_mask, store_mask;

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_12u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_12u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_12u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_12u_1);

    tail_mask = OWN_BIT_MASK(num_elements);
    read_mask0 = 0x5555555555555555 & tail_mask;
    read_mask1 = 0xAAAAAAAAAAAAAAAA & tail_mask;
    store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 6u));

    srcmm0 = _mm512_maskz_loadu_epi8(read_mask0, src_ptr);
    srcmm1 = _mm512_maskz_loadu_epi8(read_mask1, src_ptr);

    // uniting each two 6u(8u) to one 12u(16u)
    zmm2 = _mm512_srli_epi16(srcmm1, 2u);
    zmm2 = _mm512_or_si512(srcmm0, zmm2);

    // pack_16u12u optimization
    zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
    zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

    zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);

}

OWN_OPT_FUN(void, k0_qplc_pack_8u6u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 6u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u6u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += ((align * 6u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u)
    {
        uint32_t num_elements_256 = num_elements / 256u;
        uint32_t num_elements_64 = (num_elements % 256u) / 64u;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3, srcmm4, srcmm5, srcmm6, srcmm7;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5, zmm6, zmm7, zmm8, zmm9;

        __m512i permutex_idx_ptr[6];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_12u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_12u_1);
        permutex_idx_ptr[2] = _mm512_load_si512(permutex_idx_table_12u_2);
        permutex_idx_ptr[3] = _mm512_load_si512(permutex_idx_table_12u_3);
        permutex_idx_ptr[4] = _mm512_load_si512(permutex_idx_table_12u_4);
        permutex_idx_ptr[5] = _mm512_load_si512(permutex_idx_table_12u_5);

        __m512i shift_masks_ptr[6];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_12u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_12u_1);
        shift_masks_ptr[2] = _mm512_load_si512(shift_mask_table_12u_2);
        shift_masks_ptr[3] = _mm512_load_si512(shift_mask_table_12u_3);
        shift_masks_ptr[4] = _mm512_load_si512(shift_mask_table_12u_4);
        shift_masks_ptr[5] = _mm512_load_si512(shift_mask_table_12u_5);

        for (uint32_t idx = 0; idx < num_elements_256; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);
            srcmm2 = _mm512_maskz_loadu_epi8(0x5555555555555555, (src_ptr + 64u));
            srcmm3 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, (src_ptr + 64u));
            srcmm4 = _mm512_maskz_loadu_epi8(0x5555555555555555, (src_ptr + 128u));
            srcmm5 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, (src_ptr + 128u));
            srcmm6 = _mm512_maskz_loadu_epi8(0x5555555555555555, (src_ptr + 192u));
            srcmm7 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, (src_ptr + 192u));

            // uniting each two 6u(8u) to one 12u(16u)
            zmm6 = _mm512_srli_epi16(srcmm1, 2u);
            zmm6 = _mm512_or_si512(srcmm0, zmm6);
            zmm7 = _mm512_srli_epi16(srcmm3, 2u);
            zmm7 = _mm512_or_si512(srcmm2, zmm7);
            zmm8 = _mm512_srli_epi16(srcmm5, 2u);
            zmm8 = _mm512_or_si512(srcmm4, zmm8);
            zmm9 = _mm512_srli_epi16(srcmm7, 2u);
            zmm9 = _mm512_or_si512(srcmm6, zmm9);

            // pack_16u12u optimization
            zmm0 = _mm512_permutex2var_epi16(zmm6, permutex_idx_ptr[0], zmm7);
            zmm1 = _mm512_permutex2var_epi16(zmm6, permutex_idx_ptr[1], zmm7);
            zmm2 = _mm512_permutex2var_epi16(zmm7, permutex_idx_ptr[2], zmm8);
            zmm3 = _mm512_permutex2var_epi16(zmm7, permutex_idx_ptr[3], zmm8);
            zmm4 = _mm512_permutex2var_epi16(zmm8, permutex_idx_ptr[4], zmm9);
            zmm5 = _mm512_permutex2var_epi16(zmm8, permutex_idx_ptr[5], zmm9);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_sllv_epi16(zmm2, shift_masks_ptr[2]);
            zmm3 = _mm512_srlv_epi16(zmm3, shift_masks_ptr[3]);
            zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[4]);
            zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[5]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm2 = _mm512_or_si512(zmm2, zmm3);
            zmm4 = _mm512_or_si512(zmm4, zmm5);

            _mm512_storeu_si512(dst_ptr, zmm0);
            _mm512_storeu_si512((dst_ptr + 64u), zmm2);
            _mm512_storeu_si512((dst_ptr + 128u), zmm4);

            src_ptr += 256;
            dst_ptr += 6u * 32u;
        }

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);

            // uniting each two 6u(8u) to one 12u(16u)
            zmm2 = _mm512_srli_epi16(srcmm1, 2u);
            zmm2 = _mm512_or_si512(srcmm0, zmm2);

            // pack_16u12u optimization
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x00FFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 6u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u6u_tail(src_ptr, tail, dst_ptr);
    }
}

OWN_QPLC_INLINE(void, px_qplc_pack_8u7u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    uint32_t bit_width = 7u;
    uint32_t bits_in_buf = bit_width + start_bit;
    uint16_t src = (uint16_t)(*dst_ptr) & OWN_BIT_MASK(start_bit);
    src |= ((uint16_t)(*src_ptr)) << start_bit;
    src_ptr++;
    num_elements--;
    while (0u < num_elements) {
        *dst_ptr = (uint8_t)(src);
        dst_ptr++;
        src = src >> OWN_BYTE_WIDTH;
        bits_in_buf -= OWN_BYTE_WIDTH;
        src = src | (((uint16_t)(*src_ptr)) << bits_in_buf);
        src_ptr++;
        num_elements--;
        bits_in_buf += bit_width;
    }
    *dst_ptr = (uint8_t)(src);
    if (OWN_BYTE_WIDTH < bits_in_buf) {
        dst_ptr++;
        src >>= OWN_BYTE_WIDTH;
        *dst_ptr = (uint8_t)(src);
    }
}

OWN_QPLC_INLINE(void, k0_qplc_pack_8u7u_tail, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr))
{
    __m512i srcmm0, srcmm1;
    __m512i zmm0, zmm1, zmm2;
    __mmask64 read_mask0, read_mask1, tail_mask, store_mask;

    __m512i permutex_idx_ptr[2];
    permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_14u_0);
    permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_14u_1);

    __m512i shift_masks_ptr[2];
    shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_14u_0);
    shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_14u_1);

    tail_mask = OWN_BIT_MASK(num_elements);
    read_mask0 = 0x5555555555555555 & tail_mask;
    read_mask1 = 0xAAAAAAAAAAAAAAAA & tail_mask;
    store_mask = OWN_BIT_MASK(OWN_BITS_2_BYTE(num_elements * 7u));

    srcmm0 = _mm512_maskz_loadu_epi8(read_mask0, src_ptr);
    srcmm1 = _mm512_maskz_loadu_epi8(read_mask1, src_ptr);

    // uniting each two 7u(8u) to one 14u(16u)
    zmm2 = _mm512_srli_epi16(srcmm1, 1u);
    zmm2 = _mm512_or_si512(srcmm0, zmm2);

    // pack_16u14u optimization
    zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
    zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

    zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
    zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

    zmm0 = _mm512_or_si512(zmm0, zmm1);
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, zmm0);
}

OWN_OPT_FUN(void, k0_qplc_pack_8u7u, (const uint8_t *src_ptr, uint32_t num_elements, uint8_t *dst_ptr, uint32_t start_bit))
{
    if (start_bit > 0u) {
        uint32_t align = own_get_align(start_bit, 7u, 8u);
        if (align > num_elements) {
            align = num_elements;
        }

        px_qplc_pack_8u7u(src_ptr, align, dst_ptr, start_bit);
        src_ptr += align;
        dst_ptr += ((align * 7u) + start_bit) >> 3u;
        num_elements -= align;
    }

    uint32_t tail = num_elements % 64u;
    if (num_elements >= 64u)
    {
        uint32_t num_elements_64 = num_elements / 64u;
        __m512i srcmm0, srcmm1;
        __m512i zmm0, zmm1, zmm2;

        __m512i permutex_idx_ptr[2];
        permutex_idx_ptr[0] = _mm512_load_si512(permutex_idx_table_14u_0);
        permutex_idx_ptr[1] = _mm512_load_si512(permutex_idx_table_14u_1);

        __m512i shift_masks_ptr[2];
        shift_masks_ptr[0] = _mm512_load_si512(shift_mask_table_14u_0);
        shift_masks_ptr[1] = _mm512_load_si512(shift_mask_table_14u_1);

        for (uint32_t idx = 0; idx < num_elements_64; ++idx)
        {
            srcmm0 = _mm512_maskz_loadu_epi8(0x5555555555555555, src_ptr);
            srcmm1 = _mm512_maskz_loadu_epi8(0xAAAAAAAAAAAAAAAA, src_ptr);

            // uniting each two 7u(8u) to one 14u(16u)
            zmm2 = _mm512_srli_epi16(srcmm1, 1u);
            zmm2 = _mm512_or_si512(srcmm0, zmm2);

            // pack_16u14u optimization
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], zmm2);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], zmm2);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            _mm512_mask_storeu_epi16(dst_ptr, 0x0FFFFFFF, zmm0);

            src_ptr += 64u;
            dst_ptr += 7u * 8u;
        }
    }

    if (tail > 0) {
        k0_qplc_pack_8u7u_tail(src_ptr, tail, dst_ptr);
    }
}
#endif // OWN_PACK_8U_H
