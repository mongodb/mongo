/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*------- qplc_pack_16u_k0.h -------*/

/**
 * @brief Contains implementation of functions for vector packing byte integers to 9...16-bit integers
 * @date 18/06/2021
 *
 * @details Function list:
 *          - @ref k0_qplc_pack_16u9u
 *          - @ref k0_qplc_pack_16u10u
 *          - @ref k0_qplc_pack_16u11u
 *          - @ref k0_qplc_pack_16u12u
 *          - @ref k0_qplc_pack_16u13u
 *          - @ref k0_qplc_pack_16u14u
 *          - @ref k0_qplc_pack_16u15u
 *          - @ref k0_qplc_pack_16u32u
 *
 */


#ifndef OWN_PACK_BE_16U_H
#define OWN_PACK_BE_16U_H

#include "own_qplc_defs.h"

OWN_QPLC_INLINE(void, qplc_pack_be_16u_nu, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint32_t bit_width,
    uint8_t* dst_ptr,
    uint32_t start_bit));

// *********************** Masks  ****************************** //

static uint8_t pshufb_idx_ptr[64] = {
     1,  0,  3,  2,  5,  4,  7,  6,  9,  8, 11, 10, 13, 12, 15, 14,
     1,  0,  3,  2,  5,  4,  7,  6,  9,  8, 11, 10, 13, 12, 15, 14,
     1,  0,  3,  2,  5,  4,  7,  6,  9,  8, 11, 10, 13, 12, 15, 14,
     1,  0,  3,  2,  5,  4,  7,  6,  9,  8, 11, 10, 13, 12, 15, 14};

static uint16_t permute_idx_16u[32] = {
    0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};


// ----------------------- 16u9u ------------------------------- //
static uint16_t permutex_idx_table_9u_0[32] = {
     0u,  1u,  3u,  5u,  7u,  8u, 10u, 12u, 14u, 16u, 17u, 19u, 21u, 23u, 24u, 26u,
    28u, 30u, 32u, 33u, 35u, 37u, 39u, 40u, 42u, 44u, 46u, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_9u_1[32] = {
    0x0,  2u,  4u,  6u, 0x0,  9u, 11u, 13u, 0x0, 0x0, 18u, 20u, 22u, 0x0, 25u, 27u,
    29u, 0x0, 0x0, 34u, 36u, 38u, 0x0, 41u, 43u, 45u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_9u_2[32] = {
     1u,  3u,  5u,  7u,  8u, 10u, 12u, 14u, 15u, 17u, 19u, 21u, 23u, 24u, 26u, 28u,
    30u, 31u, 33u, 35u, 37u, 39u, 40u, 42u, 44u, 46u, 47u, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_9u_3[32] = {
    16u, 17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 32u, 33u, 35u, 37u, 39u, 40u, 42u,
    44u, 46u, 48u, 49u, 51u, 53u, 55u, 56u, 58u, 60u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_9u_4[32] = {
    0x0, 18u, 20u, 22u, 0x0, 25u, 27u, 29u, 0x0, 0x0, 34u, 36u, 38u, 0x0, 41u, 43u,
    45u, 0x0, 0x0, 50u, 52u, 54u, 0x0, 57u, 59u, 61u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_9u_5[32] = {
    17u, 19u, 21u, 23u, 24u, 26u, 28u, 30u, 31u, 33u, 35u, 37u, 39u, 40u, 42u, 44u,
    46u, 47u, 49u, 51u, 53u, 55u, 56u, 58u, 60u, 62u, 63u, 0x0, 0x0, 0x0, 0x0, 0x0};
static __mmask32 permutex_masks_9u_ptr[3] = {0x07FFFFFF, 0x03B9DCEE, 0x07FFFFFF};

static uint16_t shift_mask_table_9u_0[32] = {
     7u, 14u, 12u, 10u,  8u, 15u, 13u, 11u,  9u,  7u, 14u, 12u, 10u,  8u, 15u, 13u,
    11u,  9u,  7u, 14u, 12u, 10u,  8u, 15u, 13u, 11u,  9u, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_9u_1[32] = {
    0x0,  5u,  3u,  1u, 0x0,  6u,  4u,  2u, 0x0, 0x0,  5u,  3u,  1u, 0x0,  6u,  4u,
     2u, 0x0, 0x0,  5u,  3u,  1u, 0x0,  6u,  4u,  2u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_9u_2[32] = {
     2u,  4u,  6u,  8u,  1u,  3u,  5u,  7u,  0u,  2u,  4u,  6u,  8u,  1u,  3u,  5u,
     7u,  0u,  2u,  4u,  6u,  8u,  1u,  3u,  5u,  7u,  0u, 0x0, 0x0, 0x0, 0x0, 0x0};

static uint32_t table_align_16u9u[16] = {
     0,  7, 14,  5, 12,  3, 10,  1,  8, 15,  6, 13,  4, 11,  2,  9};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u9u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = table_align_16u9u[start_bit & 15];

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 9u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_96;
        uint32_t num_elements_32;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;
        __m512i permutex_idx_ptr[6];
        __m512i shift_masks_ptr[3];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_9u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_9u_1);
        permutex_idx_ptr[2] = _mm512_loadu_si512(permutex_idx_table_9u_2);
        permutex_idx_ptr[3] = _mm512_loadu_si512(permutex_idx_table_9u_3);
        permutex_idx_ptr[4] = _mm512_loadu_si512(permutex_idx_table_9u_4);
        permutex_idx_ptr[5] = _mm512_loadu_si512(permutex_idx_table_9u_5);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_9u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_9u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_9u_2);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 9 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);

            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);

            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 9u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements % 32u;
            num_elements_96 = num_elements / 96u;
            num_elements_32 = (num_elements % 96u) / 32u;

            for (uint32_t idx = 0; idx < num_elements_96; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
                srcmm2 = _mm512_loadu_si512((src_ptr + 128u));

                zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
                zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
                zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
                zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[0], srcmm1, permutex_idx_ptr[3], srcmm2);
                zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[1], srcmm1, permutex_idx_ptr[4], srcmm2);
                zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_9u_ptr[2], srcmm1, permutex_idx_ptr[5], srcmm2);

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

                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                zmm3 = _mm512_shuffle_epi8(zmm3, pshufb_idx);

                _mm512_mask_storeu_epi16(dst_ptr, 0x07FFFFFF, zmm0);
                _mm512_mask_storeu_epi16((dst_ptr + 54u), 0x07FFFFFF, zmm3);

                src_ptr += 96u * sizeof(uint16_t);
                dst_ptr += 54u * sizeof(uint16_t);
            }

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[2], permutex_idx_ptr[2], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x0003FFFF, zmm0);

                src_ptr += 32u * sizeof(uint16_t);
                dst_ptr += 18u * sizeof(uint16_t);
            }
            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 9 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_9u_ptr[2], permutex_idx_ptr[2], srcmm0);
                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u10u ------------------------------- //
static uint16_t permutex_idx_table_10u_0[32] = {
     0u,  1u,  3u,  4u,  6u,  8u,  9u, 11u, 12u, 14u, 16u, 17u, 19u, 20u, 22u, 24u,
    25u, 27u, 28u, 30u, 32u, 33u, 35u, 36u, 38u, 40u, 41u, 43u, 44u, 46u, 0x0, 0x0};
static uint16_t permutex_idx_table_10u_1[32] = {
    0x0,  2u, 0x0,  5u, 0x0, 0x0, 10u, 0x0, 13u, 0x0, 0x0, 18u, 0x0, 21u, 0x0, 0x0,
    26u, 0x0, 29u, 0x0, 0x0, 34u, 0x0, 37u, 0x0, 0x0, 42u, 0x0, 45u, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_10u_2[32] = {
     1u,  3u,  4u,  6u,  7u,  9u, 11u, 12u, 14u, 15u, 17u, 19u, 20u, 22u, 23u, 25u,
    27u, 28u, 30u, 31u, 33u, 35u, 36u, 38u, 39u, 41u, 43u, 44u, 46u, 47u, 0x0, 0x0};
static uint16_t permutex_idx_table_10u_3[32] = {
    16u, 17u, 19u, 20u, 22u, 24u, 25u, 27u, 28u, 30u, 32u, 33u, 35u, 36u, 38u, 40u,
    41u, 43u, 44u, 46u, 48u, 49u, 51u, 52u, 54u, 56u, 57u, 59u, 60u, 62u, 0x0, 0x0};
static uint16_t permutex_idx_table_10u_4[32] = {
    0x0, 18u, 0x0, 21u, 0x0, 0x0, 26u, 0x0, 29u, 0x0, 0x0, 34u, 0x0, 37u, 0x0, 0x0,
    42u, 0x0, 45u, 0x0, 0x0, 50u, 0x0, 53u, 0x0, 0x0, 58u, 0x0, 61u, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_10u_5[32] = {
    17u, 19u, 20u, 22u, 23u, 25u, 27u, 28u, 30u, 31u, 33u, 35u, 36u, 38u, 39u, 41u,
    43u, 44u, 46u, 47u, 49u, 51u, 52u, 54u, 55u, 57u, 59u, 60u, 62u, 63u, 0x0, 0x0};
static __mmask32 permutex_masks_10u_ptr[3] = {0x3FFFFFFF, 0x14A5294A, 0x3FFFFFFF};

static uint16_t shift_mask_table_10u_0[32] = {
     6u, 12u,  8u, 14u, 10u,  6u, 12u,  8u, 14u, 10u,  6u, 12u,  8u, 14u, 10u,  6u,
    12u,  8u, 14u, 10u,  6u, 12u,  8u, 14u, 10u,  6u, 12u,  8u, 14u, 10u, 0x0, 0x0};
static uint16_t shift_mask_table_10u_1[32] = {
    0x0,  2u, 0x0,  4u, 0x0, 0x0,  2u, 0x0,  4u, 0x0, 0x0,  2u, 0x0,  4u, 0x0, 0x0,
     2u, 0x0,  4u, 0x0, 0x0,  2u, 0x0,  4u, 0x0, 0x0,  2u, 0x0,  4u, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_10u_2[32] = {
     4u,  8u,  2u,  6u,  0u,  4u,  8u,  2u,  6u,  0u,  4u,  8u,  2u,  6u,  0u,  4u,
     8u,  2u,  6u,  0u,  4u,  8u,  2u,  6u,  0u,  4u,  8u,  2u,  6u,  0u, 0x0, 0x0};

static uint32_t table_align_16u10u[16] = {
     0, 0xffffffff, 3, 0xffffffff, 6, 0xffffffff, 1, 0xffffffff, 4, 0xffffffff, 7, 0xffffffff, 2, 0xffffffff, 5, 0xffffffff};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u10u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = table_align_16u10u[start_bit & 15];

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 10u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_96;
        uint32_t num_elements_32;
        __m512i srcmm0, srcmm1, srcmm2;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;
        __m512i permutex_idx_ptr[6];
        __m512i shift_masks_ptr[3];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_10u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_10u_1);
        permutex_idx_ptr[2] = _mm512_loadu_si512(permutex_idx_table_10u_2);
        permutex_idx_ptr[3] = _mm512_loadu_si512(permutex_idx_table_10u_3);
        permutex_idx_ptr[4] = _mm512_loadu_si512(permutex_idx_table_10u_4);
        permutex_idx_ptr[5] = _mm512_loadu_si512(permutex_idx_table_10u_5);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_10u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_10u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_10u_2);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 10 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);

            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);

            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 10u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements % 32u;
            num_elements_96 = num_elements / 96u;
            num_elements_32 = (num_elements % 96u) / 32u;

            for (uint32_t idx = 0; idx < num_elements_96; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
                srcmm2 = _mm512_loadu_si512((src_ptr + 128u));

                zmm0 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], srcmm0, permutex_idx_ptr[0], srcmm1);
                zmm1 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], srcmm0, permutex_idx_ptr[1], srcmm1);
                zmm2 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], srcmm0, permutex_idx_ptr[2], srcmm1);
                zmm3 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[0], srcmm1, permutex_idx_ptr[3], srcmm2);
                zmm4 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[1], srcmm1, permutex_idx_ptr[4], srcmm2);
                zmm5 = _mm512_maskz_permutex2var_epi16(permutex_masks_10u_ptr[2], srcmm1, permutex_idx_ptr[5], srcmm2);

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

                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                zmm3 = _mm512_shuffle_epi8(zmm3, pshufb_idx);

                _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);
                _mm512_mask_storeu_epi16((dst_ptr + 60u), 0x3FFFFFFF, zmm3);

                src_ptr += 96u * 2u;
                dst_ptr += 60u * 2u;
            }

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x000FFFFF, zmm0);

                src_ptr += 32u * 2u;
                dst_ptr += 20u * 2u;
            }
            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 10 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_10u_ptr[2], permutex_idx_ptr[2], srcmm0);
                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);
                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u11u ------------------------------- //
static uint16_t permutex_idx_table_11u_0[32] = {
     0u,  1u,  2u,  4u,  5u,  7u,  8u, 10u, 11u, 13u, 14u, 16u, 17u, 18u, 20u, 21u,
    23u, 24u, 26u, 27u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u};
static uint16_t permutex_idx_table_11u_1[32] = {
    0x0, 0x0,  3u, 0x0,  6u, 0x0,  9u, 0x0, 12u, 0x0, 0x0, 0x0, 0x0, 19u, 0x0, 22u,
    0x0, 25u, 0x0, 28u, 0x0, 0x0, 0x0, 0x0, 35u, 0x0, 38u, 0x0, 41u, 0x0, 44u, 0x0};
static uint16_t permutex_idx_table_11u_2[32] = {
     1u,  2u,  4u,  5u,  7u,  8u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 20u, 21u, 23u,
    24u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u, 46u};
static uint16_t permutex_idx_table_11u_3[32] = {
    14u, 16u, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u, 30u, 32u, 33u, 34u, 36u,
    37u, 39u, 40u, 42u, 43u, 45u, 46u, 48u, 49u, 50u, 52u, 53u, 55u, 56u, 58u, 59u};
static uint16_t permutex_idx_table_11u_4[32] = {
    0x0, 0x0, 0x0, 19u, 0x0, 22u, 0x0, 25u, 0x0, 28u, 0x0, 0x0, 0x0, 0x0, 35u, 0x0,
    38u, 0x0, 41u, 0x0, 44u, 0x0, 0x0, 0x0, 0x0, 51u, 0x0, 54u, 0x0, 57u, 0x0, 60u};
static uint16_t permutex_idx_table_11u_5[32] = {
    15u, 17u, 18u, 20u, 21u, 23u, 24u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 36u, 37u,
    39u, 40u, 42u, 43u, 45u, 46u, 47u, 49u, 50u, 52u, 53u, 55u, 56u, 58u, 59u, 61u};
static uint16_t permutex_idx_table_11u_6[32] = {
    29u, 30u, 32u, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u, 46u, 48u, 49u, 50u,
    52u, 53u, 55u, 56u, 58u, 59u, 61u, 62u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_11u_7[32] = {
    0x0, 0x0, 0x0, 0x0, 35u, 0x0, 38u, 0x0, 41u, 0x0, 44u, 0x0, 0x0, 0x0, 0x0, 51u,
    0x0, 54u, 0x0, 57u, 0x0, 60u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_11u_8[32] = {
    30u, 31u, 33u, 34u, 36u, 37u, 39u, 40u, 42u, 43u, 45u, 46u, 47u, 49u, 50u, 52u,
    53u, 55u, 56u, 58u, 59u, 61u, 62u, 63u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};


static uint16_t shift_mask_table_11u_0[32] = {
     5u, 10u, 15u,  9u, 14u,  8u, 13u,  7u, 12u,  6u, 11u,  5u, 10u, 15u,  9u, 14u,
     8u, 13u,  7u, 12u,  6u, 11u,  5u, 10u, 15u,  9u, 14u,  8u, 13u,  7u, 12u,  6u};
static uint16_t shift_mask_table_11u_1[32] = {
    0x0, 0x0,  4u, 0x0,  3u, 0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0,  4u, 0x0,  3u,
    0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0,  4u, 0x0,  3u, 0x0,  2u, 0x0,  1u, 0x0};
static uint16_t shift_mask_table_11u_2[32] = {
     6u,  1u,  7u,  2u,  8u,  3u,  9u,  4u, 10u,  5u,  0u,  6u,  1u,  7u,  2u,  8u,
     3u,  9u,  4u, 10u,  5u,  0u,  6u,  1u,  7u,  2u,  8u,  3u,  9u,  4u, 10u,  5u};
static uint16_t shift_mask_table_11u_3[32] = {
    11u,  5u, 10u, 15u,  9u, 14u,  8u, 13u,  7u, 12u,  6u, 11u,  5u, 10u, 15u,  9u,
    14u,  8u, 13u,  7u, 12u,  6u, 11u,  5u, 10u, 15u,  9u, 14u,  8u, 13u,  7u, 12u};
static uint16_t shift_mask_table_11u_4[32] = {
    0x0, 0x0, 0x0,  4u, 0x0,  3u, 0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0,  4u, 0x0,
     3u, 0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0,  4u, 0x0,  3u, 0x0,  2u, 0x0,  1u};
static uint16_t shift_mask_table_11u_5[32] = {
     0u,  6u,  1u,  7u,  2u,  8u,  3u,  9u,  4u, 10u,  5u,  0u,  6u,  1u,  7u,  2u,
     8u,  3u,  9u,  4u, 10u,  5u,  0u,  6u,  1u,  7u,  2u,  8u,  3u,  9u,  4u, 10u};
static uint16_t shift_mask_table_11u_6[32] = {
     6u, 11u,  5u, 10u, 15u,  9u, 14u,  8u, 13u,  7u, 12u,  6u, 11u,  5u, 10u, 15u,
     9u, 14u,  8u, 13u,  7u, 12u,  6u, 11u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_11u_7[32] = {
    0x0, 0x0, 0x0, 0x0,  4u, 0x0,  3u, 0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0,  4u,
    0x0,  3u, 0x0,  2u, 0x0,  1u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_11u_8[32] = {
     5u,  0u,  6u,  1u,  7u,  2u,  8u,  3u,  9u,  4u, 10u,  5u,  0u,  6u,  1u,  7u,
     2u,  8u,  3u,  9u,  4u, 10u,  5u,  0u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

static uint32_t table_align_16u11u[16] = {
     0, 13, 10,  7,  4,  1, 14, 11,  8,  5,  2, 15, 12,  9,  6,  3};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u11u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = table_align_16u11u[start_bit & 15];

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 11u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_128;
        uint32_t num_elements_32;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm00, zmm01, zmm02, zmm10, zmm11, zmm12, zmm20, zmm21, zmm22;
        __m512i permutex_idx_ptr[9];
        __m512i shift_masks_ptr[9];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_11u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_11u_1);
        permutex_idx_ptr[2] = _mm512_loadu_si512(permutex_idx_table_11u_2);
        permutex_idx_ptr[3] = _mm512_loadu_si512(permutex_idx_table_11u_3);
        permutex_idx_ptr[4] = _mm512_loadu_si512(permutex_idx_table_11u_4);
        permutex_idx_ptr[5] = _mm512_loadu_si512(permutex_idx_table_11u_5);
        permutex_idx_ptr[6] = _mm512_loadu_si512(permutex_idx_table_11u_6);
        permutex_idx_ptr[7] = _mm512_loadu_si512(permutex_idx_table_11u_7);
        permutex_idx_ptr[8] = _mm512_loadu_si512(permutex_idx_table_11u_8);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_11u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_11u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_11u_2);
        shift_masks_ptr[3] = _mm512_loadu_si512(shift_mask_table_11u_3);
        shift_masks_ptr[4] = _mm512_loadu_si512(shift_mask_table_11u_4);
        shift_masks_ptr[5] = _mm512_loadu_si512(shift_mask_table_11u_5);
        shift_masks_ptr[6] = _mm512_loadu_si512(shift_mask_table_11u_6);
        shift_masks_ptr[7] = _mm512_loadu_si512(shift_mask_table_11u_7);
        shift_masks_ptr[8] = _mm512_loadu_si512(shift_mask_table_11u_8);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 11 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);

            zmm00 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
            zmm01 = _mm512_maskz_permutexvar_epi16(0x550aa154, permutex_idx_ptr[1], srcmm0);
            zmm02 = _mm512_permutexvar_epi16(permutex_idx_ptr[2], srcmm0);

            zmm00 = _mm512_sllv_epi16(zmm00, shift_masks_ptr[0]);
            zmm01 = _mm512_sllv_epi16(zmm01, shift_masks_ptr[1]);
            zmm02 = _mm512_srlv_epi16(zmm02, shift_masks_ptr[2]);

            zmm00 = _mm512_or_si512(zmm00, zmm01);
            zmm00 = _mm512_or_si512(zmm00, zmm02);

            zmm01 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm00);
            zmm00 = _mm512_srl_epi16(zmm00, _mm_cvtsi32_si128(start_bit));
            zmm01 = _mm512_sll_epi16(zmm01, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm01 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm01), (short)src, 0));
            zmm00 = _mm512_or_si512(zmm00, zmm01);
            zmm00 = _mm512_shuffle_epi8(zmm00, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm00);

            src_ptr += align * 2;
            dst_ptr += ((align * 11u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements & 0x1fu;
            num_elements_128 = num_elements >> 7u;
            num_elements_32 = (num_elements >> 5u) & 3u;

            for (uint32_t idx = 0; idx < num_elements_128; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
                srcmm2 = _mm512_loadu_si512((src_ptr + 128u));
                srcmm3 = _mm512_loadu_si512((src_ptr + 192u));

                zmm00 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[0], srcmm1);
                zmm01 = _mm512_maskz_permutex2var_epi16(0x550aa154, srcmm0, permutex_idx_ptr[1], srcmm1);
                zmm02 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[2], srcmm1);
                zmm10 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[3], srcmm2);
                zmm11 = _mm512_maskz_permutex2var_epi16(0xaa1542a8, srcmm1, permutex_idx_ptr[4], srcmm2);
                zmm12 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[5], srcmm2);
                zmm20 = _mm512_maskz_permutex2var_epi16(0x00ffffff, srcmm2, permutex_idx_ptr[6], srcmm3);
                zmm21 = _mm512_maskz_permutex2var_epi16(0x002a8550, srcmm2, permutex_idx_ptr[7], srcmm3);
                zmm22 = _mm512_maskz_permutex2var_epi16(0x00ffffff, srcmm2, permutex_idx_ptr[8], srcmm3);

                zmm00 = _mm512_sllv_epi16(zmm00, shift_masks_ptr[0]);
                zmm01 = _mm512_sllv_epi16(zmm01, shift_masks_ptr[1]);
                zmm02 = _mm512_srlv_epi16(zmm02, shift_masks_ptr[2]);
                zmm10 = _mm512_sllv_epi16(zmm10, shift_masks_ptr[3]);
                zmm11 = _mm512_sllv_epi16(zmm11, shift_masks_ptr[4]);
                zmm12 = _mm512_srlv_epi16(zmm12, shift_masks_ptr[5]);
                zmm20 = _mm512_sllv_epi16(zmm20, shift_masks_ptr[6]);
                zmm21 = _mm512_sllv_epi16(zmm21, shift_masks_ptr[7]);
                zmm22 = _mm512_srlv_epi16(zmm22, shift_masks_ptr[8]);

                zmm00 = _mm512_or_si512(zmm00, zmm01);
                zmm10 = _mm512_or_si512(zmm10, zmm11);
                zmm20 = _mm512_or_si512(zmm20, zmm21);
                zmm00 = _mm512_or_si512(zmm00, zmm02);
                zmm10 = _mm512_or_si512(zmm10, zmm12);
                zmm20 = _mm512_or_si512(zmm20, zmm22);

                zmm00 = _mm512_shuffle_epi8(zmm00, pshufb_idx);
                zmm10 = _mm512_shuffle_epi8(zmm10, pshufb_idx);
                zmm20 = _mm512_shuffle_epi8(zmm20, pshufb_idx);

                _mm512_storeu_si512(dst_ptr, zmm00);
                _mm512_storeu_si512((dst_ptr + 64u), zmm10);
                _mm512_mask_storeu_epi16(dst_ptr + 128u, 0x00FFFFFF, zmm20);

                src_ptr += 128u * 2u;
                dst_ptr += 88u * 2u;
            }

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm00 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
                zmm01 = _mm512_maskz_permutexvar_epi16(0x550aa154, permutex_idx_ptr[1], srcmm0);
                zmm02 = _mm512_permutexvar_epi16(permutex_idx_ptr[2], srcmm0);

                zmm00 = _mm512_sllv_epi16(zmm00, shift_masks_ptr[0]);
                zmm01 = _mm512_sllv_epi16(zmm01, shift_masks_ptr[1]);
                zmm02 = _mm512_srlv_epi16(zmm02, shift_masks_ptr[2]);

                zmm00 = _mm512_or_si512(zmm00, zmm01);
                zmm00 = _mm512_or_si512(zmm00, zmm02);
                zmm00 = _mm512_shuffle_epi8(zmm00, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x003FFFFF, zmm00);

                src_ptr += 32u * 2u;
                dst_ptr += 22u * 2u;
            }
            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 11 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm00 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
                zmm01 = _mm512_maskz_permutexvar_epi16(0x550aa154, permutex_idx_ptr[1], srcmm0);
                zmm02 = _mm512_permutexvar_epi16(permutex_idx_ptr[2], srcmm0);

                zmm00 = _mm512_sllv_epi16(zmm00, shift_masks_ptr[0]);
                zmm01 = _mm512_sllv_epi16(zmm01, shift_masks_ptr[1]);
                zmm02 = _mm512_srlv_epi16(zmm02, shift_masks_ptr[2]);

                zmm00 = _mm512_or_si512(zmm00, zmm01);
                zmm00 = _mm512_or_si512(zmm00, zmm02);
                zmm00 = _mm512_shuffle_epi8(zmm00, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm00);
            }
        }
    }
}

// ----------------------- 16u12u ------------------------------- //
static uint16_t permutex_idx_table_12u_0[32] = {
     0u,  1u,  2u,  4u,  5u,  6u,  8u,  9u, 10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u,
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u};
static uint16_t permutex_idx_table_12u_1[32] = {
     1u,  2u,  3u,  5u,  6u,  7u,  9u, 10u, 11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u,
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u};
static uint16_t permutex_idx_table_12u_2[32] = {
    10u, 12u, 13u, 14u, 16u, 17u, 18u, 20u, 21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u,
    32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u, 42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u};
static uint16_t permutex_idx_table_12u_3[32] = {
    11u, 13u, 14u, 15u, 17u, 18u, 19u, 21u, 22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u,
    33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u, 43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u};
static uint16_t permutex_idx_table_12u_4[32] = {
    21u, 22u, 24u, 25u, 26u, 28u, 29u, 30u, 32u, 33u, 34u, 36u, 37u, 38u, 40u, 41u,
    42u, 44u, 45u, 46u, 48u, 49u, 50u, 52u, 53u, 54u, 56u, 57u, 58u, 60u, 61u, 62u};
static uint16_t permutex_idx_table_12u_5[32] = {
    22u, 23u, 25u, 26u, 27u, 29u, 30u, 31u, 33u, 34u, 35u, 37u, 38u, 39u, 41u, 42u,
    43u, 45u, 46u, 47u, 49u, 50u, 51u, 53u, 54u, 55u, 57u, 58u, 59u, 61u, 62u, 63u};

static uint16_t shift_mask_table_12u_0[32] = {
     4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,
     8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u};
static uint16_t shift_mask_table_12u_2[32] = {
    12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,
     4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u};
static uint16_t shift_mask_table_12u_4[32] = {
     8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u,
    12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u,  4u,  8u, 12u};
static uint16_t shift_mask_table_12u_1[32] = {
     8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,
     4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u};
static uint16_t shift_mask_table_12u_3[32] = {
     0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,
     8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u};
static uint16_t shift_mask_table_12u_5[32] = {
     4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,
     0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u,  8u,  4u,  0u};

static uint32_t table_align_16u12u[16] = {
     0, 0xffffffff, 0xffffffff, 0xffffffff,
     1, 0xffffffff, 0xffffffff, 0xffffffff,
     2, 0xffffffff, 0xffffffff, 0xffffffff,
     3, 0xffffffff, 0xffffffff, 0xffffffff};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u12u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = table_align_16u12u[start_bit & 15];

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 12u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_128;
        uint32_t num_elements_32;
        __m512i srcmm0, srcmm1, srcmm2, srcmm3;
        __m512i zmm0, zmm1, zmm2, zmm3, zmm4, zmm5;
        __m512i permutex_idx_ptr[6];
        __m512i shift_masks_ptr[6];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_12u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_12u_1);
        permutex_idx_ptr[2] = _mm512_loadu_si512(permutex_idx_table_12u_2);
        permutex_idx_ptr[3] = _mm512_loadu_si512(permutex_idx_table_12u_3);
        permutex_idx_ptr[4] = _mm512_loadu_si512(permutex_idx_table_12u_4);
        permutex_idx_ptr[5] = _mm512_loadu_si512(permutex_idx_table_12u_5);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_12u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_12u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_12u_2);
        shift_masks_ptr[3] = _mm512_loadu_si512(shift_mask_table_12u_3);
        shift_masks_ptr[4] = _mm512_loadu_si512(shift_mask_table_12u_4);
        shift_masks_ptr[5] = _mm512_loadu_si512(shift_mask_table_12u_5);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 12 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
            zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 12u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements & 31u;
            num_elements_128 = num_elements >> 7u;
            num_elements_32 = (num_elements & 127u) >> 5u;

            for (uint32_t idx = 0; idx < num_elements_128; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                srcmm1 = _mm512_loadu_si512((src_ptr + 64u));
                srcmm2 = _mm512_loadu_si512((src_ptr + 128u));
                srcmm3 = _mm512_loadu_si512((src_ptr + 192u));

                zmm0 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[0], srcmm1);
                zmm1 = _mm512_permutex2var_epi16(srcmm0, permutex_idx_ptr[1], srcmm1);
                zmm2 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[2], srcmm2);
                zmm3 = _mm512_permutex2var_epi16(srcmm1, permutex_idx_ptr[3], srcmm2);
                zmm4 = _mm512_permutex2var_epi16(srcmm2, permutex_idx_ptr[4], srcmm3);
                zmm5 = _mm512_permutex2var_epi16(srcmm2, permutex_idx_ptr[5], srcmm3);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_sllv_epi16(zmm2, shift_masks_ptr[2]);
                zmm3 = _mm512_srlv_epi16(zmm3, shift_masks_ptr[3]);
                zmm4 = _mm512_sllv_epi16(zmm4, shift_masks_ptr[4]);
                zmm5 = _mm512_srlv_epi16(zmm5, shift_masks_ptr[5]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm2 = _mm512_or_si512(zmm2, zmm3);
                zmm4 = _mm512_or_si512(zmm4, zmm5);

                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                zmm2 = _mm512_shuffle_epi8(zmm2, pshufb_idx);
                zmm4 = _mm512_shuffle_epi8(zmm4, pshufb_idx);

                _mm512_storeu_si512(dst_ptr, zmm0);
                _mm512_storeu_si512((dst_ptr + 64u), zmm2);
                _mm512_storeu_si512((dst_ptr + 128u), zmm4);

                src_ptr += 128u * sizeof(uint16_t);
                dst_ptr += 96u * sizeof(uint16_t);
            }

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x00FFFFFF, zmm0);

                src_ptr += 32u * sizeof(uint16_t);
                dst_ptr += 24u * sizeof(uint16_t);
            }
            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 12 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_permutexvar_epi16(permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_permutexvar_epi16(permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u13u ------------------------------- //
static uint16_t permutex_idx_table_13u_0[32] = {
     0u,  1u,  2u,  3u,  4u,  6u,  7u,  8u,  9u, 11u, 12u, 13u, 14u, 16u, 17u, 18u,
    19u, 20u, 22u, 23u, 24u, 25u, 27u, 28u, 29u, 30u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_13u_1[32] = {
    0x0, 0x0, 0x0, 0x0,  5u, 0x0, 0x0, 0x0, 10u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 21u, 0x0, 0x0, 0x0, 26u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_13u_2[32] = {
     1u,  2u,  3u,  4u,  6u,  7u,  8u,  9u, 11u, 12u, 13u, 14u, 15u, 17u, 18u, 19u,
    20u, 22u, 23u, 24u, 25u, 27u, 28u, 29u, 30u, 31u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};

static __mmask32 permutex_masks_13u_ptr[3] = {0x03FFFFFF, 0x00220110, 0x03FFFFFF};

static uint16_t shift_mask_table_13u_0[32] = {
     3u,  6u,  9u, 12u, 15u,  5u,  8u, 11u, 14u,  4u,  7u, 10u, 13u,  3u,  6u,  9u,
    12u, 15u,  5u,  8u, 11u, 14u,  4u,  7u, 10u, 13u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_13u_1[32] = {
    0x0, 0x0, 0x0, 0x0,  2u, 0x0, 0x0, 0x0,  1u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0,  2u, 0x0, 0x0, 0x0,  1u, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_13u_2[32] = {
    10u,  7u,  4u,  1u, 11u,  8u,  5u,  2u, 12u,  9u,  6u,  3u,  0u, 10u,  7u,  4u,
     1u, 11u,  8u,  5u,  2u, 12u,  9u,  6u,  3u,  0u,  7u, 0x0, 0x0, 0x0, 0x0, 0x0};

static uint32_t table_align_16u13u[16] = {
     0, 11,  6,  1, 12,  7,  2, 13,  8,  3, 14,  9,  4, 15, 10,  5};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u13u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = table_align_16u13u[start_bit & 15];

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 13u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_32;
        __m512i srcmm0;
        __m512i zmm0, zmm1, zmm2;
        __m512i permutex_idx_ptr[3];
        __m512i shift_masks_ptr[3];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_13u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_13u_1);
        permutex_idx_ptr[2] = _mm512_loadu_si512(permutex_idx_table_13u_2);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_13u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_13u_1);
        shift_masks_ptr[2] = _mm512_loadu_si512(shift_mask_table_13u_2);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 13 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[0], permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[1], permutex_idx_ptr[1], srcmm0);
            zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[2], permutex_idx_ptr[2], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
            zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_or_si512(zmm0, zmm2);

            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 13u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements & 31u;
            num_elements_32 = num_elements >> 5u;

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[2], permutex_idx_ptr[2], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x03FFFFFF, zmm0);

                src_ptr += 32u * sizeof(uint16_t);
                dst_ptr += 26u * sizeof(uint16_t);
            }

            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 13 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[0], permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[1], permutex_idx_ptr[1], srcmm0);
                zmm2 = _mm512_maskz_permutexvar_epi16(permutex_masks_13u_ptr[2], permutex_idx_ptr[2], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_sllv_epi16(zmm1, shift_masks_ptr[1]);
                zmm2 = _mm512_srlv_epi16(zmm2, shift_masks_ptr[2]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_or_si512(zmm0, zmm2);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u14u ------------------------------- //
static uint16_t permutex_idx_table_14u_0[32] = {
     0u,  1u,  2u,  3u,  4u,  5u,  6u,  8u,  9u, 10u, 11u, 12u, 13u, 14u, 16u, 17u,
    18u, 19u, 20u, 21u, 22u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 0x0, 0x0, 0x0, 0x0};
static uint16_t permutex_idx_table_14u_1[32] = {
     1u,  2u,  3u,  4u,  5u,  6u,  7u,  9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u, 18u,
    19u, 20u, 21u, 22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 0x0, 0x0, 0x0, 0x0};

static uint16_t shift_mask_table_14u_0[32] = {
      2u,  4u,  6u,  8u, 10u, 12u, 14u,  2u,  4u,  6u,  8u, 10u, 12u, 14u,  2u,  4u,
      6u,  8u, 10u, 12u, 14u,  2u,  4u,  6u,  8u, 10u, 12u, 14u, 0x0, 0x0, 0x0, 0x0};
static uint16_t shift_mask_table_14u_1[32] = {
    12u, 10u,  8u,  6u,  4u,  2u,  0u, 12u, 10u,  8u,  6u,  4u,  2u,  0u, 12u, 10u,
     8u,  6u,  4u,  2u,  0u, 12u, 10u,  8u,  6u,  4u,  2u,  0u, 0x0, 0x0, 0x0, 0x0};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u14u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = (start_bit & 1) ? 0xffffffff : (start_bit & 15) >> 1;

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 14u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_32;
        __m512i srcmm0;
        __m512i zmm0, zmm1;
        __m512i permutex_idx_ptr[2];
        __m512i shift_masks_ptr[2];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_14u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_14u_1);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_14u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_14u_1);
        pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 14 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);

            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 14u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements & 31u;
            num_elements_32 = num_elements >> 5u;

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x0FFFFFFF, zmm0);

                src_ptr += 32u * sizeof(uint16_t);
                dst_ptr += 28u * sizeof(uint16_t);
            }

            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 14 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(0x0FFFFFFF, permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u15u ------------------------------- //

static uint16_t permutex_idx_table_15u_0[32] = {
     0u,  1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u, 10u, 11u, 12u, 13u, 14u, 16u,
    17u, 18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 0x0, 0x0};
static uint16_t permutex_idx_table_15u_1[32] = {
     1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u, 10u, 11u, 12u, 13u, 14u, 15u, 17u,
    18u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 0x0};

static uint16_t shift_mask_table_15u_0[32] = {
     1u,  2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u, 10u, 11u, 12u, 13u, 14u, 15u,  1u,
     2u,  3u,  4u,  5u,  6u,  7u,  8u,  9u, 10u, 11u, 12u, 13u, 14u, 15u, 0x0, 0x0};
static uint16_t shift_mask_table_15u_1[32] = {
    14u, 13u, 12u, 11u, 10u,  9u,  8u,  7u,  6u,  5u,  4u,  3u,  2u,  1u,  0u, 14u,
    13u, 12u, 11u, 10u,  9u,  8u,  7u,  6u,  5u,  4u,  3u,  2u,  1u,  0u, 0x0, 0x0};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u15u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr,
    uint32_t start_bit))
{
    uint32_t align = start_bit & 15;

    if (align > num_elements) {
        align = num_elements;
        qplc_pack_be_16u_nu(src_ptr, align, 15u, dst_ptr, start_bit);
        return;
    }

    {
        uint32_t tail;
        uint32_t num_elements_32;
        __m512i srcmm0;
        __m512i zmm0, zmm1;
        __m512i permutex_idx_ptr[2];
        __m512i shift_masks_ptr[2];
        __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);
        permutex_idx_ptr[0] = _mm512_loadu_si512(permutex_idx_table_15u_0);
        permutex_idx_ptr[1] = _mm512_loadu_si512(permutex_idx_table_15u_1);
        shift_masks_ptr[0] = _mm512_loadu_si512(shift_mask_table_15u_0);
        shift_masks_ptr[1] = _mm512_loadu_si512(shift_mask_table_15u_1);

        if (align) {
            __m512i permute_idx = _mm512_loadu_si512(permute_idx_16u);
            __mmask32 mask32_load = (1 << align) - 1;
            uint64_t    num_bytes_out = ((uint64_t)align * 15 + start_bit) / OWN_BYTE_WIDTH;
            __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;
            uint32_t src = ((uint32_t)qplc_swap_bytes_16u(*(uint16_t*)dst_ptr)) & (0xffff << (OWN_WORD_WIDTH - start_bit));

            srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
            zmm0 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[0], srcmm0);
            zmm1 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[1], srcmm0);

            zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
            zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

            zmm0 = _mm512_or_si512(zmm0, zmm1);

            zmm1 = _mm512_maskz_permutexvar_epi16(0x0000FFFE, permute_idx, zmm0);
            zmm0 = _mm512_srl_epi16(zmm0, _mm_cvtsi32_si128(start_bit));
            zmm1 = _mm512_sll_epi16(zmm1, _mm_cvtsi32_si128(OWN_WORD_WIDTH - start_bit));
            zmm1 = _mm512_castsi256_si512(_mm256_insert_epi16(_mm512_castsi512_si256(zmm1), (short)src, 0));
            zmm0 = _mm512_or_si512(zmm0, zmm1);
            zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
            _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);

            src_ptr += align * 2;
            dst_ptr += ((align * 15u) + start_bit) >> 3u;
            num_elements -= align;
        }

        {
            tail = num_elements & 31u;
            num_elements_32 = num_elements >> 5u;

            for (uint32_t idx = 0; idx < num_elements_32; ++idx)
            {
                srcmm0 = _mm512_loadu_si512(src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi16(dst_ptr, 0x3FFFFFFF, zmm0);

                src_ptr += 32u * sizeof(uint16_t);
                dst_ptr += 30u * sizeof(uint16_t);
            }

            if (tail > 0) {
                uint64_t    num_bytes_out = ((uint64_t)tail * 15 + 7) / OWN_BYTE_WIDTH;
                __mmask32   mask32_load = (1 << tail) - 1;
                __mmask64   mask64_store = ((uint64_t)1 << num_bytes_out) - (uint64_t)1;

                srcmm0 = _mm512_mask_loadu_epi16(_mm512_setzero_si512(), mask32_load, (void const*)src_ptr);
                zmm0 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[0], srcmm0);
                zmm1 = _mm512_maskz_permutexvar_epi16(0x3FFFFFFF, permutex_idx_ptr[1], srcmm0);

                zmm0 = _mm512_sllv_epi16(zmm0, shift_masks_ptr[0]);
                zmm1 = _mm512_srlv_epi16(zmm1, shift_masks_ptr[1]);

                zmm0 = _mm512_or_si512(zmm0, zmm1);
                zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
                _mm512_mask_storeu_epi8(dst_ptr, mask64_store, zmm0);
            }
        }
    }
}

// ----------------------- 16u16u ------------------------------- //

OWN_OPT_FUN(void, k0_qplc_pack_be_16u16u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr))
{
    uint16_t* dst_16u_ptr = (uint16_t*)dst_ptr;
    uint16_t* src_16u_ptr = (uint16_t*)src_ptr;
    uint32_t num_elements_32 = num_elements >> 5u;;
    uint32_t tail = num_elements & 31u;
    __m512i zmm0;

    __m512i pshufb_idx = _mm512_loadu_si512(pshufb_idx_ptr);

    for (uint32_t i = 0u; i < num_elements_32; i++) {
        zmm0 = _mm512_shuffle_epi8(_mm512_loadu_si512(src_16u_ptr), pshufb_idx);
        _mm512_storeu_si512((void*)dst_16u_ptr, zmm0);
        src_16u_ptr += 32;
        dst_16u_ptr += 32;
    }

    if (tail) {
        __mmask32 mask32_loadstore = (1 << tail) - 1;
        zmm0 = _mm512_shuffle_epi8(_mm512_maskz_loadu_epi16(mask32_loadstore, src_16u_ptr), pshufb_idx);
        _mm512_mask_storeu_epi16((void*)dst_16u_ptr, mask32_loadstore, zmm0);
    }
}

// ----------------------- 16u16u ------------------------------- //

static uint8_t pshufb16u32u_idx_ptr[64] = {
     3,  2,  1,  0,  7,  6,  5,  4, 11, 10,  9,  8, 15, 14, 13, 12,
     3,  2,  1,  0,  7,  6,  5,  4, 11, 10,  9,  8, 15, 14, 13, 12,
     3,  2,  1,  0,  7,  6,  5,  4, 11, 10,  9,  8, 15, 14, 13, 12,
     3,  2,  1,  0,  7,  6,  5,  4, 11, 10,  9,  8, 15, 14, 13, 12};

OWN_OPT_FUN(void, k0_qplc_pack_be_16u32u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t* dst_ptr))
{
    uint32_t* dst_32u_ptr = (uint32_t*)dst_ptr;
    uint16_t* src_16u_ptr = (uint16_t*)src_ptr;
    uint32_t num_elements_16 = num_elements >> 4u;;
    uint32_t tail = num_elements & 15u;
    __m512i zmm0;
    __m512i pshufb_idx = _mm512_loadu_si512(pshufb16u32u_idx_ptr);

    for (uint32_t i = 0u; i < num_elements_16; i++) {
        zmm0 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i*)src_16u_ptr));
        zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
        _mm512_storeu_si512((void*)dst_32u_ptr, zmm0);
        src_16u_ptr += 16;
        dst_32u_ptr += 16;
    }

    if (tail) {
        __mmask16 mask16_loadstore = (1 << tail) - 1;
        zmm0 = _mm512_cvtepu16_epi32(_mm256_maskz_loadu_epi16(mask16_loadstore, (const __m256i*)src_16u_ptr));
        zmm0 = _mm512_shuffle_epi8(zmm0, pshufb_idx);
        _mm512_mask_storeu_epi32((void*)dst_32u_ptr, mask16_loadstore, zmm0);
    }
}


#endif // OWN_PACK_BE_16U_H

