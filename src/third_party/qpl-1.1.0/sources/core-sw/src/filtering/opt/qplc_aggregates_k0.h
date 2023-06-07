/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of functions for vector packing byte integers indexes
  * @date 03/11/2021
  *
  * @details Function list:
  *          - @ref qplc_bit_aggregates_8u
  *          - @ref qplc_aggregates_8u
  *          - @ref qplc_aggregates_16u
  *          - @ref qplc_aggregates_32u
  */
#ifndef OWN_AGGREGATES_H
#define OWN_AGGREGATES_H

#include "own_qplc_defs.h"
#include "immintrin.h"

// ********************** bit ****************************** //

OWN_OPT_FUN(void, k0_qplc_bit_aggregates_8u, (const uint8_t* src_ptr,
    uint32_t length,
    uint32_t* min_value_ptr,
    uint32_t* max_value_ptr,
    uint32_t* sum_ptr,
    uint32_t* index_ptr)) {

    const uint8_t* src_ptr_start;
    __m512i     z_data;
    __m512i     z_zero = _mm512_setzero_si512();

    uint32_t    rem_len;
    uint32_t    len_crn;
    uint32_t    len_new = length;
    uint32_t    index = *index_ptr;
    uint32_t    idx;
    __mmask64   msk64;

    src_ptr_start = src_ptr;
    *index_ptr += length;

    if (OWN_MAX_32U == *min_value_ptr) {
        rem_len = length & 63;
        len_crn = length - rem_len;
        msk64 = 0;
        for (idx = 0; idx < len_crn; idx += 64) {
            msk64 = _mm512_cmpgt_epu8_mask(_mm512_loadu_si512((void const*)(src_ptr + idx)), z_zero);
            if (msk64)
                break;
        }
        if (!msk64) {
            if (rem_len) {
                msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), rem_len);
                z_data = _mm512_mask_loadu_epi8(z_zero, msk64, (void const*)(src_ptr + len_crn));
                msk64 = _mm512_cmpgt_epu8_mask(z_data, z_zero);
            }
        }
        if (!msk64)
            return;
        idx += (uint32_t)_tzcnt_u64((uint64_t)msk64);
        *min_value_ptr = idx + index;
        src_ptr_start += idx;
        len_new -= idx;
    }

    {
        rem_len = length & 63;
        len_crn = length - rem_len;
        msk64 = 0;
        for (idx = length; idx > rem_len; idx -= 64) {
            msk64 = _mm512_cmpgt_epu8_mask(_mm512_loadu_si512((void const*)(src_ptr + idx - 64)), z_zero);
            if (msk64)
                break;
        }
        if (!msk64) {
            if (rem_len) {
                msk64 = (__mmask64)(uint64_t)((int64_t)(-1)) << (uint64_t)(64 - rem_len);
                z_data = _mm512_mask_loadu_epi8(_mm512_set1_epi8(1), msk64, (void const*)(src_ptr + rem_len - 64));
                msk64 = _mm512_cmpgt_epu8_mask(z_data, z_zero);
            }
        }
        if (msk64)
            idx -= (1 + (uint32_t)_lzcnt_u64((uint64_t)msk64));
        if ((int32_t)idx < (int32_t)0)
            return;
        *max_value_ptr = idx + index;
        len_new -= (length - (idx + 1));
    }

    {
        int64_t sum = 0;
        rem_len = len_new & 63;
        len_crn = len_new - rem_len;
        for (idx = 0; idx < len_crn; idx += 64) {
            msk64 = _mm512_cmpgt_epu8_mask(_mm512_loadu_si512((void const*)(src_ptr_start + idx)), z_zero);
            sum += _mm_popcnt_u64((uint64_t)msk64);
        }
        if (rem_len) {
            msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), rem_len);
            z_data = _mm512_mask_loadu_epi8(z_zero, msk64, (void const*)(src_ptr_start + len_crn));
            msk64 = _mm512_cmpgt_epu8_mask(z_data, z_zero);
            sum += _mm_popcnt_u64((uint64_t)msk64);
        }
        *sum_ptr += (uint32_t)sum;
    }
}

// ********************** 8u ****************************** //
#if defined _MSC_VER
#if _MSC_VER <= 1916
/* if MSVC <= MSVC2017 */
/*
There is the problem with compiler of MSVC2017.
*/
#pragma optimize("", off)
#endif
#endif

OWN_OPT_FUN(void, k0_qplc_aggregates_8u, (const uint8_t* src_ptr,
    uint32_t length,
    uint32_t* min_value_ptr,
    uint32_t* max_value_ptr,
    uint32_t* sum_ptr)) {
    uint32_t    min_value = *min_value_ptr;
    uint32_t    max_value = *max_value_ptr;
    __m512i     z_data;
    __m512i     z_sum = _mm512_setzero_si512();
    __m512i     z_min = _mm512_set1_epi8((char)min_value);
    __m512i     z_max = _mm512_set1_epi8((char)max_value);
    __m512i     z_zero = _mm512_setzero_si512();
    __m256i     y_data;
    __m128i     x_data;
    __mmask64   msk64;
    uint32_t    remind = length & 63;

    length -= remind;

    if (0 == min_value) {
        if (max_value >= 0xff) {
            for (uint32_t idx = 0u; idx < length; idx += 64) {
                z_data = _mm512_sad_epu8(z_zero, _mm512_loadu_si512((void const*)(src_ptr + idx))); /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */
            }
            if (remind) {
                msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
                z_data = _mm512_sad_epu8(z_zero, _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length))); /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */
            }
        } else {
            for (uint32_t idx = 0u; idx < length; idx += 64) {
                z_data = _mm512_loadu_si512((void const*)(src_ptr + idx));
                z_max = _mm512_max_epu8(z_max, z_data);                     /* z_max  = max */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */

            }
            if (remind) {
                msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
                z_data = _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length));
                z_max = _mm512_max_epu8(z_max, z_data);                     /* z_max  = max */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */
            }
        }
    } else {
        if (max_value >= 0xff) {
            for (uint32_t idx = 0u; idx < length; idx += 64) {
                z_data = _mm512_loadu_si512((void const*)(src_ptr + idx));
                z_min = _mm512_min_epu8(z_min, z_data);                     /* z_min  = min */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */

            }
            if (remind) {
                msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
                z_data = _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length));
                z_min = _mm512_mask_min_epu8(z_min, msk64, z_min, z_data);  /* z_min  = min */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */
            }
        } else {
            for (uint32_t idx = 0u; idx < length; idx += 64) {
                z_data = _mm512_loadu_si512((void const*)(src_ptr + idx));
                z_min = _mm512_min_epu8(z_min, z_data);                     /* z_min  = min */
                z_max = _mm512_max_epu8(z_max, z_data);                     /* z_max  = max */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */

            }
            if (remind) {
                msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
                z_data = _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length));
                z_min = _mm512_mask_min_epu8(z_min, msk64, z_min, z_data);  /* z_min  = min */
                z_max = _mm512_max_epu8(z_max, z_data);                     /* z_max  = max */
                z_data = _mm512_sad_epu8(z_data, z_zero);                   /* z_data = s7 s6 s5 s4 s3 s2 s1 s0 */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum  = S7 S6 S5 S4 S3 S2 S1 S0 */
            }
        }
    }
    if (min_value != 0) {
        y_data = _mm512_extracti64x4_epi64(z_min, 1);
        y_data = _mm256_min_epu8(y_data, _mm512_castsi512_si256(z_min));    /* y_data = mn31 .. mn0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_min_epu8(x_data, _mm256_castsi256_si128(y_data));      /* x_data = mn15 .. mn0 */
        x_data = _mm_min_epu8(x_data, _mm_srli_si128(x_data, 8));           /* x_data =  mn7 .. mn0 */
        x_data = _mm_min_epu8(x_data, _mm_srli_epi64(x_data, 32));          /* x_data =  mn3 .. mn0 */
        x_data = _mm_min_epu8(x_data, _mm_srli_epi32(x_data, 16));          /* x_data =  mn1 mn0 */
        x_data = _mm_min_epu8(x_data, _mm_srli_epi16(x_data, 8));           /* x_data =  mn0 */
        *min_value_ptr = (uint32_t)(_mm_cvtsi128_si32(x_data) & 0xff);
    }
    if (max_value < 0xff) {
        y_data = _mm512_extracti64x4_epi64(z_max, 1);
        y_data = _mm256_max_epu8(y_data, _mm512_castsi512_si256(z_max));    /* y_data = mx31 .. mx0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_max_epu8(x_data, _mm256_castsi256_si128(y_data));      /* x_data = mx15 .. mx0 */
        x_data = _mm_max_epu8(x_data, _mm_srli_si128(x_data, 8));           /* x_data =  mx7 .. mx0 */
        x_data = _mm_max_epu8(x_data, _mm_srli_epi64(x_data, 32));          /* x_data =  mx3 .. mx0 */
        x_data = _mm_max_epu8(x_data, _mm_srli_epi32(x_data, 16));          /* x_data =  mx1 mx0 */
        x_data = _mm_max_epu8(x_data, _mm_srli_epi16(x_data, 8));           /* x_data =  mx0 */
        *max_value_ptr = (uint32_t)(_mm_cvtsi128_si32(x_data) & 0xff);
    }
    y_data = _mm512_extracti64x4_epi64(z_sum, 1);
    y_data = _mm256_add_epi32(y_data, _mm512_castsi512_si256(z_sum));       /* y_data =S3 S2 S1 S0 */
    x_data = _mm256_extracti128_si256(y_data, 1);
    x_data = _mm_add_epi32(x_data, _mm256_castsi256_si128(y_data));         /* x_data = S1 S0 */
    x_data = _mm_add_epi32(x_data, _mm_srli_si128(x_data, 8));              /* x_data = S0 */
    *sum_ptr += (uint32_t)_mm_cvtsi128_si32(x_data);
}
#if defined _MSC_VER
#if _MSC_VER <= 1916
/* if MSVC <= MSVC2017 */
/*
There is the problem with compiler of MSVC2017.
*/
#pragma optimize("", on)
#endif
#endif


// ********************** 16u ****************************** //

OWN_OPT_FUN(void, k0_qplc_aggregates_16u, (const uint8_t* src_ptr,
    uint32_t  length,
    uint32_t* min_value_ptr,
    uint32_t* max_value_ptr,
    uint32_t* sum_ptr)) {
    const uint16_t* src_16u_ptr = (uint16_t*)src_ptr;
    uint32_t    min_value = *min_value_ptr;
    uint32_t    max_value = *max_value_ptr;
    __m512i     z_data_0;
    __m512i     z_data_1;
    __m512i     z_sum  = _mm512_setzero_si512();
    __m512i     z_min = _mm512_set1_epi16((short)min_value);
    __m512i     z_max = _mm512_set1_epi16((short)max_value);
    __m256i     y_data;
    __m128i     x_data;
    __mmask32   msk32;
    uint32_t    remind = length & 31;

    length -= remind;

    if (0 == min_value) {
        if (max_value >= OWN_MAX_16U) {
            for (uint32_t idx = 0u; idx < length; idx += 32) {
                z_data_0 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i*)(src_16u_ptr + idx)));
                z_data_1 = _mm512_cvtepu16_epi32(_mm256_loadu_si256((const __m256i*)(src_16u_ptr + idx + 16)));
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);
            }
            if (remind) {
                msk32 = (__mmask32)_bzhi_u32((uint32_t)(-1), remind);
                z_data_0 = _mm512_maskz_loadu_epi16(msk32, (void const*)(src_16u_ptr + length));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);
            }
        } else  {
            for (uint32_t idx = 0u; idx < length; idx += 32) {
                z_data_0 = _mm512_loadu_si512((void const*)(src_16u_ptr + idx));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_max = _mm512_max_epu16(z_max, z_data_0);                  /* z_max  = max */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk32 = (__mmask32)_bzhi_u32((uint32_t)(-1), remind);
                z_data_0 = _mm512_maskz_loadu_epi16(msk32, (void const*)(src_16u_ptr + length));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_max = _mm512_max_epu16(z_max, z_data_0);                  /* z_max  = max */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
        }
    } else {
        if (max_value >= OWN_MAX_16U) {
            for (uint32_t idx = 0u; idx < length; idx += 32) {
                z_data_0 = _mm512_loadu_si512((void const*)(src_16u_ptr + idx));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_min = _mm512_min_epu16(z_min, z_data_0);                  /* z_min  = min */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk32 = (__mmask32)_bzhi_u32((uint32_t)(-1), remind);
                z_data_0 = _mm512_maskz_loadu_epi16(msk32, (void const*)(src_16u_ptr + length));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_min = _mm512_mask_min_epu16(z_min, msk32, z_min, z_data_0); /* z_min  = min */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
        } else {
            for (uint32_t idx = 0u; idx < length; idx += 32) {
                z_data_0 = _mm512_loadu_si512((void const*)(src_16u_ptr + idx));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_min = _mm512_min_epu16(z_min, z_data_0);                  /* z_min  = min */
                z_max = _mm512_max_epu16(z_max, z_data_0);                  /* z_max  = max */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk32 = (__mmask32)_bzhi_u32((uint32_t)(-1), remind);
                z_data_0 = _mm512_maskz_loadu_epi16(msk32, (void const*)(src_16u_ptr + length));
                y_data = _mm512_extracti64x4_epi64(z_data_0, 1);
                z_min = _mm512_mask_min_epu16(z_min, msk32, z_min, z_data_0); /* z_min  = min */
                z_max = _mm512_max_epu16(z_max, z_data_0);                  /* z_max  = max */
                z_data_0 = _mm512_cvtepu16_epi32(_mm512_castsi512_si256(z_data_0));
                z_data_1 = _mm512_cvtepu16_epi32(y_data);
                z_sum = _mm512_add_epi32(z_sum, z_data_0);
                z_sum = _mm512_add_epi32(z_sum, z_data_1);                  /* z_sum = s15 .. s0 */
            }
        }
    }
    if (min_value != 0) {
        y_data = _mm512_extracti64x4_epi64(z_min, 1);
        y_data = _mm256_min_epu16(y_data, _mm512_castsi512_si256(z_min));   /* y_data = mn16 .. mn0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_min_epu16(x_data, _mm256_castsi256_si128(y_data));     /* x_data = mn7 .. mn0 */
        x_data = _mm_min_epu16(x_data, _mm_srli_si128(x_data, 8));          /* x_data = mn3 .. mn0 */
        x_data = _mm_min_epu16(x_data, _mm_srli_epi64(x_data, 32));         /* x_data = mn1 mn0 */
        x_data = _mm_min_epu16(x_data, _mm_srli_epi32(x_data, 16));         /* x_data = mn0 */
        *min_value_ptr = (uint32_t)(_mm_cvtsi128_si32(x_data) & 0xffff);
    }
    if (max_value < OWN_MAX_16U) {
        y_data = _mm512_extracti64x4_epi64(z_max, 1);
        y_data = _mm256_max_epu16(y_data, _mm512_castsi512_si256(z_max));   /* y_data = mx16 .. mx0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_max_epu16(x_data, _mm256_castsi256_si128(y_data));     /* x_data = mx7 .. mx0 */
        x_data = _mm_max_epu16(x_data, _mm_srli_si128(x_data, 8));          /* x_data = mx3 .. mx0 */
        x_data = _mm_max_epu16(x_data, _mm_srli_epi64(x_data, 32));         /* x_data = mx1 mx0 */
        x_data = _mm_max_epu16(x_data, _mm_srli_epi32(x_data, 16));        /* x_data = mx0 */
        *max_value_ptr = (uint32_t)(_mm_cvtsi128_si32(x_data) & 0xffff);
    }
    y_data = _mm512_extracti64x4_epi64(z_sum, 1);
    y_data = _mm256_add_epi32(y_data, _mm512_castsi512_si256(z_sum));       /* y_data = S7 .. S0 */
    x_data = _mm256_extracti128_si256(y_data, 1);
    x_data = _mm_add_epi32(x_data, _mm256_castsi256_si128(y_data));         /* x_data = S3 .. S0 */
    x_data = _mm_add_epi32(x_data, _mm_srli_si128(x_data, 8));              /* x_data = S1 S0 */
    x_data = _mm_add_epi32(x_data, _mm_srli_epi64(x_data, 32));             /* x_data = S0 */
    *sum_ptr += (uint32_t)_mm_cvtsi128_si32(x_data);
}

// ********************** 32u ****************************** //
#if defined _MSC_VER
#if _MSC_VER <= 1916
/* if MSVC <= MSVC2017 */
/*
There is the problem with compiler of MSVC2017.
*/
#pragma optimize("", off)
#endif
#endif

OWN_OPT_FUN(void, k0_qplc_aggregates_32u, (const uint8_t* src_ptr,
    uint32_t  length,
    uint32_t* min_value_ptr,
    uint32_t* max_value_ptr,
    uint32_t* sum_ptr)) {
    const uint32_t* src_32u_ptr = (uint32_t*)src_ptr;
    uint32_t    min_value = *min_value_ptr;
    uint32_t    max_value = *max_value_ptr;
    __m512i     z_data;
    __m512i     z_sum = _mm512_setzero_si512();
    __m512i     z_min = _mm512_set1_epi32((int)min_value);
    __m512i     z_max = _mm512_set1_epi32((int)max_value);
    __m256i     y_data;
    __m128i     x_data;
    __mmask16   msk16;
    uint32_t    remind = length & 15;

    length -= remind;

    if (0 == min_value) {
        if (OWN_MAX_32U == max_value) {
            __m512i     z_sum_1 = _mm512_setzero_si512();
            uint32_t    remind_16 = length & 16;
            length -= remind_16;
            for (uint32_t idx = 0u; idx < length; idx += 32) {
                z_sum   = _mm512_add_epi32(  z_sum, _mm512_loadu_si512((void const*)(src_32u_ptr + idx)));
                z_sum_1 = _mm512_add_epi32(z_sum_1, _mm512_loadu_si512((void const*)(src_32u_ptr + idx + 16)));
            }
            z_sum = _mm512_add_epi32(z_sum, z_sum_1);
            if (remind_16) {
                z_sum = _mm512_add_epi32(z_sum, _mm512_loadu_si512((void const*)(src_32u_ptr + length)));
                length += 16;
            }
            if (remind) {
                msk16 = (__mmask16)_bzhi_u32(0xffff, remind);
                z_sum = _mm512_add_epi32(z_sum, _mm512_maskz_loadu_epi32(msk16, (void const*)(src_32u_ptr + length)));
            }
        } else {
            for (uint32_t idx = 0u; idx < length; idx += 16) {
                z_data = _mm512_loadu_si512((void const*)(src_32u_ptr + idx));
                z_max = _mm512_max_epu32(z_max, z_data);                    /* z_max = max */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk16 = (__mmask16)_bzhi_u32(0xffff, remind);
                z_data = _mm512_maskz_loadu_epi32(msk16, (void const*)(src_32u_ptr + length));
                z_max = _mm512_max_epu32(z_max, z_data);                    /* z_max = max */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
        }
    } else {
        if (OWN_MAX_32U == max_value) {
            for (uint32_t idx = 0u; idx < length; idx += 16) {
                z_data = _mm512_loadu_si512((void const*)(src_32u_ptr + idx));
                z_min = _mm512_min_epu32(z_min, z_data);                    /* z_min = min */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk16 = (__mmask16)_bzhi_u32(0xffff, remind);
                z_data = _mm512_maskz_loadu_epi32(msk16, (void const*)(src_32u_ptr + length));
                z_min = _mm512_mask_min_epu32(z_min, msk16, z_min, z_data); /* z_min = min */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
        } else {
            for (uint32_t idx = 0u; idx < length; idx += 16) {
                z_data = _mm512_loadu_si512((void const*)(src_32u_ptr + idx));
                z_min = _mm512_min_epu32(z_min, z_data);                    /* z_min = min */
                z_max = _mm512_max_epu32(z_max, z_data);                    /* z_max = max */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
            if (remind) {
                msk16 = (__mmask16)_bzhi_u32(0xffff, remind);
                z_data = _mm512_maskz_loadu_epi32(msk16, (void const*)(src_32u_ptr + length));
                z_min = _mm512_mask_min_epu32(z_min, msk16, z_min, z_data); /* z_min = min */
                z_max = _mm512_max_epu32(z_max, z_data);                    /* z_max = max */
                z_sum = _mm512_add_epi32(z_sum, z_data);                    /* z_sum = s15 .. s0 */
            }
        }
    }
    if (min_value != 0) {
        y_data = _mm512_extracti64x4_epi64(z_min, 1);
        y_data = _mm256_min_epu32(y_data, _mm512_castsi512_si256(z_min));   /* y_data = mn7 .. mn0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_min_epu32(x_data, _mm256_castsi256_si128(y_data));     /* x_data = mn3 .. mn0 */
        x_data = _mm_min_epu32(x_data, _mm_srli_si128(x_data, 8));          /* x_data = mn1 mn0 */
        x_data = _mm_min_epu32(x_data, _mm_srli_epi64(x_data, 32));         /* x_data = mn0 */
        *min_value_ptr = (uint32_t)_mm_cvtsi128_si32(x_data);
    }
    if (max_value != OWN_MAX_32U) {
        y_data = _mm512_extracti64x4_epi64(z_max, 1);
        y_data = _mm256_max_epu32(y_data, _mm512_castsi512_si256(z_max));   /* y_data = mx7 .. mx0 */
        x_data = _mm256_extracti128_si256(y_data, 1);
        x_data = _mm_max_epu32(x_data, _mm256_castsi256_si128(y_data));     /* x_data = mx3 .. mx0 */
        x_data = _mm_max_epu32(x_data, _mm_srli_si128(x_data, 8));          /* x_data = mx1 mx0 */
        x_data = _mm_max_epu32(x_data, _mm_srli_epi64(x_data, 32));         /* x_data = mx0 */
        *max_value_ptr = (uint32_t)_mm_cvtsi128_si32(x_data);
    }
    y_data = _mm512_extracti64x4_epi64(z_sum, 1);
    y_data = _mm256_add_epi32(y_data, _mm512_castsi512_si256(z_sum));      /* y_data = s7 .. s0 */
    x_data = _mm256_extracti128_si256(y_data, 1);
    x_data = _mm_add_epi32(x_data, _mm256_castsi256_si128(y_data));        /* x_data = s3 .. s0 */
    x_data = _mm_add_epi32(x_data, _mm_srli_si128(x_data, 8));             /* x_data = s1 s0 */
    x_data = _mm_add_epi32(x_data, _mm_srli_epi64(x_data, 32));            /* x_data = s0 */
    *sum_ptr += (uint32_t)_mm_cvtsi128_si32(x_data);
}
#if defined _MSC_VER
#if _MSC_VER <= 1916
/* if MSVC <= MSVC2017 */
/*
There is the problem with compiler of MSVC2017.
*/
#pragma optimize("", on)
#endif
#endif


#endif // OWN_AGGREGATES_H
