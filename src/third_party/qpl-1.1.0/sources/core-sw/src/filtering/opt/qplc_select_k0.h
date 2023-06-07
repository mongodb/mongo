/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of all functions for select analytics operation
  * @date 04/08/2021
  *
  * @details Function list:
  *          - @ref qplc_select_8u_i
  *          - @ref qplc_select_16u_i
  *          - @ref qplc_select_32u_i
  *          - @ref qplc_select_8u
  *          - @ref qplc_select_16u
  *          - @ref qplc_select_32u
  *
  */

#ifndef OWN_SELECT_H
#define OWN_SELECT_H

#include "own_qplc_defs.h"
#include "immintrin.h"

  /******** out-of-place select functions ********/
OWN_OPT_FUN(uint32_t, k0_qplc_select_8u, (const uint8_t* src_ptr,
    const uint8_t* src2_ptr,
    uint8_t* dst_ptr,
    uint32_t length)) {
    uint32_t    selected = 0u;
    uint32_t    remind = length & 63;
    uint32_t    num_data;
    __m512i     z_zero = _mm512_setzero_si512();
    __m512i     z_data;
    __m128i     x_data;
    __mmask64   msk;
    __mmask16   msk16;

    length -= remind;
    for (uint32_t idx = 0u; idx < length; idx += 64) {
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_loadu_si512((__m512i const*)(src2_ptr + idx)));
        for (uint32_t idx_inloop = idx; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_cvtepu8_epi32(_mm_loadu_si128((__m128i const*)(src_ptr + idx_inloop)));
                z_data = _mm512_maskz_compress_epi32(msk16, z_data);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                x_data = _mm512_cvtepi32_epi8(z_data);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm_mask_storeu_epi8((void*)(dst_ptr + selected), msk16, x_data);
                selected += num_data;
            }
        }
    }
    if (remind) {
        msk = _bzhi_u64((uint64_t)((int64_t)(-1)), remind);
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_maskz_loadu_epi8(msk, (__m512i const*)(src2_ptr + length)));
        for (uint32_t idx_inloop = length; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_cvtepu8_epi32(_mm_maskz_loadu_epi8(msk16, (__m128i const*)(src_ptr + idx_inloop)));
                z_data = _mm512_maskz_compress_epi32(msk16, z_data);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                x_data = _mm512_cvtepi32_epi8(z_data);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm_mask_storeu_epi8((void*)(dst_ptr + selected), msk16, x_data);
                selected += num_data;
            }
        }
    }
    return selected;
}

OWN_OPT_FUN(uint32_t, k0_qplc_select_16u, (const uint8_t* src_ptr,
    const uint8_t* src2_ptr,
    uint8_t* dst_ptr,
    uint32_t length)) {
    uint16_t*   src_16u_ptr = (uint16_t*)src_ptr;
    uint16_t*   dst_16u_ptr = (uint16_t*)dst_ptr;
    uint32_t    selected = 0u;
    uint32_t    remind = length & 63;
    uint32_t    num_data;
    __m512i     z_zero = _mm512_setzero_si512();
    __m512i     z_data;
    __m256i     y_data;
    __mmask64   msk;
    __mmask16   msk16;

    length -= remind;
    for (uint32_t idx = 0u; idx < length; idx += 64) {
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_loadu_si512((__m512i const*)(src2_ptr + idx)));
        for (uint32_t idx_inloop = idx; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_cvtepu16_epi32(_mm256_loadu_si256((__m256i const*)(src_16u_ptr + idx_inloop)));
                z_data = _mm512_maskz_compress_epi32(msk16, z_data);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                y_data = _mm512_cvtepi32_epi16(z_data);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm256_mask_storeu_epi16((void*)(dst_16u_ptr + selected), msk16, y_data);
                selected += num_data;
            }
        }
    }
    if (remind) {
        msk = _bzhi_u64((uint64_t)((int64_t)(-1)), remind);
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_maskz_loadu_epi8(msk, (__m512i const*)(src2_ptr + length)));
        for (uint32_t idx_inloop = length; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_cvtepu16_epi32(_mm256_maskz_loadu_epi16(msk16, (__m256i const*)(src_16u_ptr + idx_inloop)));
                z_data = _mm512_maskz_compress_epi32(msk16, z_data);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                y_data = _mm512_cvtepi32_epi16(z_data);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm256_mask_storeu_epi16((void*)(dst_16u_ptr + selected), msk16, y_data);
                selected += num_data;
            }
        }
    }
    return selected;
}

OWN_OPT_FUN(uint32_t, k0_qplc_select_32u, (const uint8_t* src_ptr,
    const uint8_t* src2_ptr,
    uint8_t* dst_ptr,
    uint32_t length)) {
    uint32_t* src_32u_ptr = (uint32_t*)src_ptr;
    uint32_t* dst_32u_ptr = (uint32_t*)dst_ptr;
    uint32_t  selected = 0u;
    uint32_t  remind = length & 63;
    uint32_t  num_data;
    __m512i   z_zero = _mm512_setzero_si512();
    __m512i   z_data;
    __mmask64 msk;
    __mmask16 msk16;

    length -= remind;
    for (uint32_t idx = 0u; idx < length; idx += 64) {
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_loadu_si512((__m512i const*)(src2_ptr + idx)));
        for (uint32_t idx_inloop = idx; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_maskz_compress_epi32(msk16, _mm512_loadu_si512((__m512i const*)(src_32u_ptr + idx_inloop)));
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm512_mask_storeu_epi32((void*)(dst_32u_ptr + selected), msk16, z_data);
                selected += num_data;
            }
        }
    }
    if (remind) {
        msk = _bzhi_u64((uint64_t)((int64_t)(-1)), remind);
        msk = _mm512_cmpneq_epi8_mask(z_zero, _mm512_maskz_loadu_epi8(msk, (__m512i const*)(src2_ptr + length)));
        for (uint32_t idx_inloop = length; (msk != 0); idx_inloop += 16, msk = (__mmask64)((uint64_t)msk >> 16u)) {
            msk16 = (__mmask16)msk;
            if (msk16 != 0) {
                z_data = _mm512_maskz_compress_epi32(msk16, _mm512_maskz_loadu_epi32(msk16, (__m512i const*)(src_32u_ptr + idx_inloop)));
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                msk16 = _bzhi_u32(0xffff, num_data);
                _mm512_mask_storeu_epi32((void*)(dst_32u_ptr + selected), msk16, z_data);
                selected += num_data;
            }
        }
    }
    return selected;
}

#endif // OWN_SELECT_H
