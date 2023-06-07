/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of functions for expand
  * @date 03/26/2021
  *
  * @details Function list:
  *          - @ref k0_qplc_expand_8u
  *          - @ref k0_qplc_expand_16u
  *          - @ref k0_qplc_expand_32u
*
  */
#ifndef OWN_EXPAND_H
#define OWN_EXPAND_H

#include "own_qplc_defs.h"
#include "immintrin.h"

// ********************** 8u ****************************** //
OWN_OPT_FUN(uint32_t, k0_qplc_qplc_expand_8u, (const uint8_t* src1_ptr,
    uint32_t length_1,
    const uint8_t* src2_ptr,
    uint32_t* length_2_ptr,
    uint8_t* dst_ptr)) {

    __m512i     z_data;
    __m128i     x_zero = _mm_setzero_si128();
    __m128i     x_data;
    __mmask16   msk16;
    __mmask16   msk_remind;
    uint32_t length_2 = *length_2_ptr;
    uint32_t remind = length_2 & 15;
    uint32_t expanded = 0u;
    uint32_t idx;

    if (length_1 >= length_2) {
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            x_data = x_zero;
            if (msk16) {
                z_data = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i*)(src1_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                x_data = _mm512_cvtepi32_epi8(z_data);
            }
            _mm_storeu_si128((__m128i*)(dst_ptr + idx), x_data);
        }
        if (remind) {
            msk_remind = _bzhi_u32(0xffff, remind);
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_maskz_loadu_epi8(msk_remind, (void const*)(src2_ptr + length_2)));
            x_data = x_zero;
            if (msk16) {
                z_data = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(msk_remind, (const __m128i*)(src1_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                x_data = _mm512_cvtepi32_epi8(z_data);
            }
            _mm_mask_storeu_epi8((__m128i*)(dst_ptr + length_2), msk_remind, x_data);
        }
        *length_2_ptr = 0;;
        return expanded;
    }

    {
        uint32_t    num_data;
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            if ((expanded + 16) > length_1)
                break;
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            x_data = x_zero;
            if (msk16) {
                z_data = _mm512_cvtepu8_epi32(_mm_loadu_si128((const __m128i*)(src1_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                x_data = _mm512_cvtepi32_epi8(z_data);
            }
            _mm_storeu_si128((__m128i*)(dst_ptr + idx), x_data);
        }
        for (; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            x_data = x_zero;
            if (msk16) {
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                if ((expanded + num_data) > length_1)
                    break;
                msk_remind = _bzhi_u32(0xffff, num_data);
                z_data = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(msk_remind, (const __m128i*)(src1_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += num_data;
                x_data = _mm512_cvtepi32_epi8(z_data);
            }
            _mm_storeu_si128((__m128i*)(dst_ptr + idx), x_data);
        }
        length_2 += remind;
        for (; idx < length_2; idx++) {
            if (src2_ptr[idx]) {
                OWN_CONDITION_BREAK(expanded >= length_1);
                dst_ptr[idx] = src1_ptr[expanded++];
            } else {
                dst_ptr[idx] = 0u;
            }
        }
        *length_2_ptr -= idx;
    }
    return expanded;
}

// ********************** 16u ****************************** //
OWN_OPT_FUN(uint32_t, k0_qplc_qplc_expand_16u, (const uint8_t* src1_ptr,
    uint32_t length_1,
    const uint8_t* src2_ptr,
    uint32_t* length_2_ptr,
    uint8_t* dst_ptr)) {

    __m512i     z_data;
    __m128i     x_zero = _mm_setzero_si128();
    __m256i     y_zero = _mm256_setzero_si256();
    __m256i     y_data;
    __mmask16   msk16;
    __mmask16   msk_remind;
    uint16_t* src_16u_ptr = (uint16_t*)src1_ptr;
    uint16_t* dst_16u_ptr = (uint16_t*)dst_ptr;
    uint32_t length_2 = *length_2_ptr;
    uint32_t remind = length_2 & 15;
    uint32_t expanded = 0u;
    uint32_t idx;

    if (length_1 >= length_2) {
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            y_data = y_zero;
            if (msk16) {
                z_data = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i*)(src_16u_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                y_data = _mm512_cvtepi32_epi16(z_data);
            }
            _mm256_storeu_si256((__m256i*)(dst_16u_ptr + idx), y_data);
        }
        if (remind) {
            msk_remind = _bzhi_u32(0xffff, remind);
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_maskz_loadu_epi8(msk_remind, (void const*)(src2_ptr + length_2)));
            y_data = y_zero;
            if (msk16) {
                z_data = _mm512_cvtepu16_epi32(_mm256_maskz_loadu_epi16(msk_remind, (const __m256i*)(src_16u_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                y_data = _mm512_cvtepi32_epi16(z_data);
            }
            _mm256_mask_storeu_epi16((__m256i*)(dst_16u_ptr + length_2), msk_remind, y_data);
        }
        *length_2_ptr = 0;;
        return expanded;
    }

    {
        uint32_t    num_data;
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            if ((expanded + 16) > length_1)
                break;
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            y_data = y_zero;
            if (msk16) {
                z_data = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i*)(src_16u_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
                y_data = _mm512_cvtepi32_epi16(z_data);
            }
            _mm256_storeu_si256((__m256i*)(dst_16u_ptr + idx), y_data);
        }
        for (; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            y_data = y_zero;
            if (msk16) {
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                if ((expanded + num_data) > length_1)
                    break;
                msk_remind = _bzhi_u32(0xffff, num_data);
                z_data = _mm512_cvtepu16_epi32(_mm256_maskz_loadu_epi16(msk_remind, (const __m256i*)(src_16u_ptr + expanded)));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += num_data;
                y_data = _mm512_cvtepi32_epi16(z_data);
            }
            _mm256_storeu_si256((__m256i*)(dst_16u_ptr + idx), y_data);
        }
        length_2 += remind;
        for (; idx < length_2; idx++) {
            if (src2_ptr[idx]) {
                OWN_CONDITION_BREAK(expanded >= length_1);
                dst_16u_ptr[idx] = src_16u_ptr[expanded++];
            }
            else {
                dst_16u_ptr[idx] = 0u;
            }
        }
        *length_2_ptr -= idx;
    }
    return expanded;
}

// ********************** 32u ****************************** //
OWN_OPT_FUN(uint32_t, k0_qplc_qplc_expand_32u, (const uint8_t* src1_ptr,
    uint32_t length_1,
    const uint8_t* src2_ptr,
    uint32_t* length_2_ptr,
    uint8_t* dst_ptr)) {

    __m512i     z_data;
    __m128i     x_zero = _mm_setzero_si128();
    __m512i     z_zero = _mm512_setzero_si512();
    __mmask16   msk16;
    __mmask16   msk_remind;
    uint32_t* src_32u_ptr = (uint32_t*)src1_ptr;
    uint32_t* dst_32u_ptr = (uint32_t*)dst_ptr;
    uint32_t length_2 = *length_2_ptr;
    uint32_t remind = length_2 & 15;
    uint32_t expanded = 0u;
    uint32_t idx;

    if (length_1 >= length_2) {
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            z_data = z_zero;
            if (msk16) {
                z_data = _mm512_maskz_expand_epi32(msk16, _mm512_loadu_si512((const __m512i*)(src_32u_ptr + expanded)));
                expanded += _mm_popcnt_u32((uint32_t)msk16);
            }
            _mm512_storeu_si512((__m512i*)(dst_32u_ptr + idx), z_data);
        }
        if (remind) {
            msk_remind = _bzhi_u32(0xffff, remind);
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_maskz_loadu_epi8(msk_remind, (void const*)(src2_ptr + length_2)));
            z_data = z_zero;
            if (msk16) {
                z_data = _mm512_maskz_loadu_epi32(msk_remind, (const __m512i*)(src_32u_ptr + expanded));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += _mm_popcnt_u32((uint32_t)msk16);
            }
            _mm512_mask_storeu_epi32((__m512i*)(dst_32u_ptr + length_2), msk_remind, z_data);
        }
        *length_2_ptr = 0;;
        return expanded;
    }

    {
        uint32_t    num_data;
        length_2 -= remind;
        for (idx = 0; idx < length_2; idx += 16) {
            if ((expanded + 16) > length_1)
                break;
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            z_data = z_zero;
            if (msk16) {
                z_data = _mm512_maskz_expand_epi32(msk16, _mm512_loadu_si512((const __m512i*)(src_32u_ptr + expanded)));
                expanded += _mm_popcnt_u32((uint32_t)msk16);
            }
            _mm512_storeu_si512((__m512i*)(dst_32u_ptr + idx), z_data);
        }
        for (; idx < length_2; idx += 16) {
            msk16 = _mm_cmpneq_epi8_mask(x_zero, _mm_loadu_si128((const __m128i*)(src2_ptr + idx)));
            z_data = z_zero;
            if (msk16) {
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                if ((expanded + num_data) > length_1)
                    break;
                msk_remind = _bzhi_u32(0xffff, num_data);
                z_data = _mm512_maskz_loadu_epi32(msk_remind, (const __m512i*)(src_32u_ptr + expanded));
                z_data = _mm512_maskz_expand_epi32(msk16, z_data);
                expanded += num_data;
            }
            _mm512_storeu_si512((__m512i*)(dst_32u_ptr + idx), z_data);
        }
        length_2 += remind;
        for (; idx < length_2; idx++) {
            if (src2_ptr[idx]) {
                OWN_CONDITION_BREAK(expanded >= length_1);
                dst_32u_ptr[idx] = src_32u_ptr[expanded++];
            } else {
                dst_32u_ptr[idx] = 0u;
            }
        }
        *length_2_ptr -= idx;
    }
    return expanded;
}


#endif // OWN_EXPAND_H
