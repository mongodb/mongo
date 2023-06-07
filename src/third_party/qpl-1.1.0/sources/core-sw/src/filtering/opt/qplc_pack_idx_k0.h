/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of functions for vector packing byte integers indexes
  * @date 02/16/2021
  *
  * @details Function list:
  *          - @ref qplc_pack_index_8u
  *
  */
#ifndef OWN_PACK_INDEX_H
#define OWN_PACK_INDEX_H

#include "own_qplc_defs.h"
#include "immintrin.h"

//#define DEF_SSE42
//#define DEF_MM256
#define DEF_MM512v0
//#define DEF_MM512v1

// ********************** 8u ****************************** //
#if defined(DEF_MM256)
// 256-bit intrinsics code (the Best)

#define STEP_SRC_8U    32

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint8_t **pp_dst,
    uint32_t dst_length,
    uint32_t *index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint8_t *dst_ptr = (uint8_t *)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m256i         y_index0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i         y_index1 = _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15);
    __m256i         y_index2 = _mm256_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23);
    __m256i         y_index3 = _mm256_setr_epi32(24, 25, 26, 27, 28, 29, 30, 31);
    __m256i         y_data;
    __m256i         y_zero = _mm256_setzero_si256();
    __m128i         x_data;
    __m128i         x_index;
    __mmask32       msk;
    __mmask16       msk16;
    __mmask8        msk8;

    /* If Index is great then we do nothing. */
    if (UINT8_MAX < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }

    /* We can only do processing when the index is less than 256. */
    act_num_elements = (UINT8_MAX + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            y_data = _mm256_loadu_si256((__m256i const *)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
    } else {
        /* Here we are obliged to check. */
        uint8_t *end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            y_data = _mm256_loadu_si256((__m256i const *)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if ((status == QPLC_STS_OK) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x0000ff00)) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x00ff0000)) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0xff000000)) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi8(y_data);
                    msk16 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk16 = _bzhi_u32(0xff, num_data);
            _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem &= (STEP_SRC_8U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U) {
            y_data = _mm256_loadu_si256((__m256i const *)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const *)(src_ptr + num_elements));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = dst_ptr;
    return status;
}
#endif

#if defined(DEF_SSE42)
  // SSE42  the Best
#define STEP_SRC_8U    16

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint8_t **pp_dst,
    uint32_t dst_length,
    uint32_t *index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint8_t *dst_ptr = (uint8_t *)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m128i         x_index;
    __m128i         x_index0 = _mm_setr_epi32(0, 1, 2, 3);
    __m128i         x_index1 = _mm_setr_epi32(4, 5, 6, 7);
    __m128i         x_index2 = _mm_setr_epi32(8, 9, 10, 11);
    __m128i         x_index3 = _mm_setr_epi32(12, 13, 14, 15);
    __m128i         x_data;
    __m128i         x_zero = _mm_setzero_si128();
    __mmask16       msk;
    __mmask16       msk16;
    __mmask8        msk8;

    /* If Index is great then we do nothing. */
    if (UINT8_MAX < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }

    /* We can only do processing when the index is less than 256. */
    act_num_elements = (UINT8_MAX + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
            msk = _mm_cmpneq_epi8_mask(x_data, x_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                if (msk & 0x000f) {
                    msk8 = (__mmask8)msk & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00f0) {
                    msk8 = (__mmask8)(msk >> 4) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0f00) {
                    msk8 = (__mmask8)(msk >> 8) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xf000) {
                    msk8 = (__mmask8)(msk >> 12);
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u32(((1 << STEP_SRC_8U) - 1), num_elem_rem);
            x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm_cmpneq_epi8_mask(x_data, x_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                if (msk & 0x000f) {
                    msk8 = (__mmask8)msk & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00f0) {
                    msk8 = (__mmask8)(msk >> 4) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0f00) {
                    msk8 = (__mmask8)(msk >> 8) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xf000) {
                    msk8 = (__mmask8)(msk >> 12);
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
    } else {
        /* Here we are obliged to check. */
        uint8_t *end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
            msk = _mm_cmpneq_epi8_mask(x_data, x_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                if (msk & 0x000f) {
                    msk8 = (__mmask8)msk & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        num_data = (uint32_t)(end_ptr - dst_ptr);
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00f0) {
                    msk8 = (__mmask8)(msk >> 4) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0f00) {
                    msk8 = (__mmask8)(msk >> 8) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xf000) {
                    msk8 = (__mmask8)(msk >> 12);
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32(((1 << STEP_SRC_8U) - 1), num_elem_rem);
            x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                if (msk & 0x000f) {
                    msk8 = (__mmask8)msk & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                if ((QPLC_STS_OK == status) && (msk & 0x00f0)) {
                    msk8 = (__mmask8)(msk >> 4) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x0f00)) {
                    msk8 = (__mmask8)(msk >> 8) & 0xf;
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0xf000)) {
                    msk8 = (__mmask8)(msk >> 12);
                    x_data = _mm_maskz_compress_epi32((__mmask8)msk8, x_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm_cvtepi32_epi8(x_data);
                    msk16 = _bzhi_u32(0xf, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk16 = _bzhi_u32(0xf, num_data);
            _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem &= (STEP_SRC_8U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U) {
            x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32(((1 << STEP_SRC_8U) - 1), num_elem_rem);
            x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = dst_ptr;
    return status;
}
#endif

#if defined (DEF_MM512v0)

#define STEP_SRC_8U    (64u)

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint8_t **pp_dst,
    uint32_t dst_length,
    uint32_t *index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint8_t *dst_ptr = (uint8_t *)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m512i         z_index0 = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m512i         z_index1 = _mm512_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    __m512i         z_index2 = _mm512_setr_epi32(32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47);
    __m512i         z_index3 = _mm512_setr_epi32(48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63);
    __m512i         z_data;
    __m512i         z_zero = _mm512_setzero_si512();
    __m128i         x_data;
    __m128i         x_index;
    __mmask64       msk;
    __mmask16       msk16;

    /* If Index is great then we do nothing. */
    if (UINT8_MAX < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }

    /* We can only do processing when the index is less than 256. */
    act_num_elements = (UINT8_MAX + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            z_data = _mm512_loadu_si512((__m512i const *)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
    } else {
        /* Here we are obliged to check. */
        uint8_t *end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U) {
            z_data = _mm512_loadu_si512((__m512i const *)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 0u;
                        msk16 = (__mmask16)msk;
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 16u;
                        msk16 = (__mmask16)(msk >> 16u);
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 32u;
                        msk16 = (__mmask16)(msk >> 32u);
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 48u;
                        msk16 = (__mmask16)(msk >> 48u);
                        break;
                    }
                    _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
            uint16_t run_1 = 1;
            if (count_last_bytes) {
                for (; run_1; run_1 += run_1) {
                    act_num_elements++;
                    if (msk16 & run_1) {
                        if (0 == --count_last_bytes) {
                            run_1 += run_1;
                            break;
                        }
                    }
                }
            }
            for (; run_1; run_1 += run_1) {
                if (msk16 & run_1) {
                    break;
                }
                act_num_elements++;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi8(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi8(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm_add_epi8(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    } else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements -= num_elem_rem;
                        msk16 = (__mmask16)msk;
                    }
                }
                if (QPLC_STS_OK == status) {
                    msk16 = (__mmask16)(msk >> 16u);
                    if (msk16 != 0) {
                        z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                        num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                        x_data = _mm512_cvtepi32_epi8(z_data);
                        msk16 = _bzhi_u32(0xffff, num_data);
                        x_data = _mm_add_epi8(x_data, x_index);
                        if ((dst_ptr + num_data) <= end_ptr) {
                            _mm_mask_storeu_epi8((void*)dst_ptr, msk16, x_data);
                            dst_ptr += num_data;
                        }
                        else {
                            status = QPLC_STS_DST_IS_SHORT_ERR;
                            act_num_elements -= num_elem_rem;
                            act_num_elements += 16;
                            msk16 = (__mmask16)(msk >> 16u);
                        }
                    }
                    if (QPLC_STS_OK == status) {
                        msk16 = (__mmask16)(msk >> 32u);
                        if (msk16 != 0) {
                            z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                            num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                            x_data = _mm512_cvtepi32_epi8(z_data);
                            msk16 = _bzhi_u32(0xffff, num_data);
                            x_data = _mm_add_epi8(x_data, x_index);
                            if ((dst_ptr + num_data) <= end_ptr) {
                                _mm_mask_storeu_epi8((void*)dst_ptr, msk16, x_data);
                                dst_ptr += num_data;
                            }
                            else {
                                status = QPLC_STS_DST_IS_SHORT_ERR;
                                act_num_elements -= num_elem_rem;
                                act_num_elements += 32;
                                msk16 = (__mmask16)(msk >> 32u);
                            }
                        }
                        if (QPLC_STS_OK == status) {
                            msk16 = (__mmask16)(msk >> 48u);
                            if (msk16 != 0) {
                                z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                                x_data = _mm512_cvtepi32_epi8(z_data);
                                msk16 = _bzhi_u32(0xffff, num_data);
                                x_data = _mm_add_epi8(x_data, x_index);
                                if ((dst_ptr + num_data) <= end_ptr) {
                                    _mm_mask_storeu_epi8((void*)dst_ptr, msk16, x_data);
                                    dst_ptr += num_data;
                                }
                                else {
                                    status = QPLC_STS_DST_IS_SHORT_ERR;
                                    act_num_elements -= num_elem_rem;
                                    act_num_elements += 48;
                                    msk16 = (__mmask16)(msk >> 48u);
                                }
                            }
                        }
                    }
                }
            }
            if (QPLC_STS_DST_IS_SHORT_ERR == status) {
                uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
                uint16_t run_1 = 1;
                if (count_last_bytes) {
                    for (; run_1; run_1 += run_1) {
                        act_num_elements++;
                        if (msk16 & run_1) {
                            if (0 == --count_last_bytes) {
                                run_1 += run_1;
                                break;
                            }
                        }
                    }
                }
                for (; run_1; run_1 += run_1) {
                    if (msk16 & run_1) {
                        break;
                    }
                    act_num_elements++;
                }
            }

        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk16 = _bzhi_u32(0xffff, num_data);
            _mm_mask_storeu_epi8((void *)dst_ptr, msk16, x_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem = num_elements & (STEP_SRC_8U - 1);
        num_elements -= num_elem_rem;
       for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U) {
            z_data = _mm512_loadu_si512((__m512i const *)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const *)(src_ptr + num_elements));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = dst_ptr;
    return status;
}
#endif

#if defined (DEF_MM512v1)

#define STEP_SRC    16

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u, (const uint8_t *src_ptr,
    uint32_t num_elements,
    uint8_t **pp_dst,
    uint32_t dst_length,
    uint32_t *index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint8_t *dst_ptr = (uint8_t *)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m512i         z_data;
    __m512i         z_index;
    __m128i        x_data;
    __mmask16       msk;

    if (UINT8_MAX < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }
    act_num_elements = (UINT8_MAX + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;
    if (dst_length >= act_num_elements) {
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC) {
            x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                z_index = _mm512_add_epi32(_mm512_set1_epi32(index + i),
                    _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
                z_data = _mm512_maskz_compress_epi32(msk, z_index);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk);
                x_data = _mm512_cvtepi32_epi8(z_data);
                msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_data);
                _mm_mask_storeu_epi8((void *)dst_ptr, msk, x_data);
                dst_ptr += num_data;
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_elem_rem);
            x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                z_index = _mm512_add_epi32(_mm512_set1_epi32(index + act_num_elements_align),
                    _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
                z_data = _mm512_maskz_compress_epi32(msk, z_index);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk);
                x_data = _mm512_cvtepi32_epi8(z_data);
                msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_data);
                _mm_mask_storeu_epi8((void *)dst_ptr, msk, x_data);
                dst_ptr += num_data;
            }
        }
    } else {
        uint8_t *end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC) {
            x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                z_index = _mm512_add_epi32(_mm512_set1_epi32(index + i),
                    _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
                z_data = _mm512_maskz_compress_epi32(msk, z_index);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk);
                if ((dst_ptr + num_data) > end_ptr) {
                    num_data = (uint32_t)(end_ptr - dst_ptr);
                    status = QPLC_STS_DST_IS_SHORT_ERR;
                }
                msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_data);
                _mm_mask_storeu_epi8((void *)dst_ptr, msk, x_data);
                dst_ptr += num_data;
            }
            if (status != QPLC_STS_OK)
                break;
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_elem_rem);
            x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
            msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
            if (msk != 0) {
                z_index = _mm512_add_epi32(_mm512_set1_epi32(index + act_num_elements_align),
                    _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15));
                z_data = _mm512_maskz_compress_epi32(msk, z_index);
                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk);
                if ((dst_ptr + num_data) > end_ptr) {
                    num_data = (uint32_t)(end_ptr - dst_ptr);
                    status = QPLC_STS_DST_IS_SHORT_ERR;
                }
                x_data = _mm512_cvtepi32_epi8(z_data);
                msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_data);
                _mm_mask_storeu_epi8((void *)dst_ptr, msk, x_data);
                dst_ptr += num_data;
            }
        }
    }
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        if (act_num_elements < num_elements) {
            src_ptr += act_num_elements;
            num_elements -= act_num_elements;
            num_elem_rem &= (STEP_SRC - 1);
            num_elements -= num_elem_rem;
            for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC) {
                x_data = _mm_loadu_si128((__m128i const *)(src_ptr + i));
                msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
                if (msk != 0) {
                    *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                    status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                    break;
                }
            }
            if ((QPLC_STS_OK == status) && num_elem_rem) {
                msk = _bzhi_u32(((1 << STEP_SRC) - 1), num_elem_rem);
                x_data = _mm_maskz_loadu_epi8(msk, (void const *)(src_ptr + act_num_elements_align));
                msk = _mm_cmpneq_epi8_mask(x_data, _mm_setzero_si128());
                if (msk != 0) {
                    *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                    status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                }
            }
            if (QPLC_STS_OK == status)
                *index_ptr += num_elements + num_elem_rem;
        }
    }
    *pp_dst = dst_ptr;
    return status;
}

#endif

// ********************** 16u ****************************** //
#if defined(DEF_SSE42) || defined(DEF_MM256)
#define DEF_MM256_16U
#endif

#if defined(DEF_MM512v0) || defined(DEF_MM512v1)
#define DEF_MM512_16U
#endif

#if defined(DEF_MM256_16U)

#define STEP_SRC_8U16U    32

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u16u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t** pp_dst,
    uint32_t dst_length,
    uint32_t* index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint16_t* dst_ptr = (uint16_t*)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m256i         y_index0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i         y_index1 = _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15);
    __m256i         y_index2 = _mm256_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23);
    __m256i         y_index3 = _mm256_setr_epi32(24, 25, 26, 27, 28, 29, 30, 31);
    __m256i         y_data;
    __m256i         y_zero = _mm256_setzero_si256();
    __m128i         x_data;
    __m128i         x_index;
    __mmask32       msk;
    __mmask8        msk8;

    /* If Index is great then we do nothing. */
    if (OWN_MAX_16U < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }

    /* We can only do processing when the index is less than 256. */
    act_num_elements = (OWN_MAX_16U + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U16U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    dst_length >>= 1;   /* the length points to number of elements now */

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U16U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi16(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi16(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
            }
        }
    }
    else {
        /* Here we are obliged to check. */
        uint16_t* end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U16U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi16(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                x_index = _mm_set1_epi16(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x0000ff00)) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x00ff0000)) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0xff000000)) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    x_data = _mm256_cvtepi32_epi16(y_data);
                    msk8 = _bzhi_u32(0xff, num_data);
                    x_data = _mm_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk8 = _bzhi_u32(0xff, num_data);
            _mm_mask_storeu_epi16((void*)dst_ptr, msk8, x_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem &= (STEP_SRC_8U16U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U16U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + num_elements));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = (uint8_t*)dst_ptr;
    return status;
}
#endif

#if defined(DEF_MM512_16U)

#define STEP_SRC_8U16U  (64u)

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u16u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t** pp_dst,
    uint32_t dst_length,
    uint32_t* index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint16_t* dst_ptr = (uint16_t*)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m512i         z_index0 = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m512i         z_index1 = _mm512_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    __m512i         z_index2 = _mm512_setr_epi32(32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47);
    __m512i         z_index3 = _mm512_setr_epi32(48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63);
    __m512i         z_data;
    __m512i         z_zero = _mm512_setzero_si512();
    __m256i         x_data;
    __m256i         x_index;
    __mmask64       msk;
    __mmask16       msk16;

    /* If Index is great then we do nothing. */
    if (OWN_MAX_16U < index) {
        status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
        return status;
    }

    /* We can only do processing when the index is less than 256. */
    act_num_elements = (OWN_MAX_16U + 1) - index;
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U16U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;
    dst_length >>= 1;

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U16U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm256_set1_epi16(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm256_set1_epi16(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
    }
    else {
        /* Here we are obliged to check. */
        uint16_t* end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U16U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm256_set1_epi16(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 0u;
                        msk16 = (__mmask16)msk;
                        break;
                    }
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 16u;
                        msk16 = (__mmask16)(msk >> 16u);
                        break;
                    }
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 32u;
                        msk16 = (__mmask16)(msk >> 32u);
                        break;
                    }
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 48u;
                        msk16 = (__mmask16)(msk >> 48u);
                        break;
                    }
                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
            uint16_t run_1 = 1;
            if (count_last_bytes) {
                for (; run_1; run_1 += run_1) {
                    act_num_elements++;
                    if (msk16 & run_1) {
                        if (0 == --count_last_bytes) {
                            run_1 += run_1;
                            break;
                        }
                    }
                }
            }
            for (; run_1; run_1 += run_1) {
                if (msk16 & run_1) {
                    break;
                }
                act_num_elements++;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                x_index = _mm256_set1_epi16(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    x_data = _mm512_cvtepi32_epi16(z_data);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    x_data = _mm256_add_epi16(x_data, x_index);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements -= num_elem_rem;
                        msk16 = (__mmask16)msk;
                    }
                }
                if (QPLC_STS_OK == status) {
                    msk16 = (__mmask16)(msk >> 16u);
                    if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                        z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                        num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                        x_data = _mm512_cvtepi32_epi16(z_data);
                        msk16 = _bzhi_u32(0xffff, num_data);
                        x_data = _mm256_add_epi16(x_data, x_index);
                        if ((dst_ptr + num_data) <= end_ptr) {
                            _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                            dst_ptr += num_data;
                        }
                        else {
                            status = QPLC_STS_DST_IS_SHORT_ERR;
                            act_num_elements -= num_elem_rem;
                            act_num_elements += 16;
                            msk16 = (__mmask16)(msk >> 16u);
                        }
                    }
                    if (QPLC_STS_OK == status) {
                        msk16 = (__mmask16)(msk >> 32u);
                        if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                            z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                            num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                            x_data = _mm512_cvtepi32_epi16(z_data);
                            msk16 = _bzhi_u32(0xffff, num_data);
                            x_data = _mm256_add_epi16(x_data, x_index);
                            if ((dst_ptr + num_data) <= end_ptr) {
                                _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                                dst_ptr += num_data;
                            }
                            else {
                                status = QPLC_STS_DST_IS_SHORT_ERR;
                                act_num_elements -= num_elem_rem;
                                act_num_elements += 32;
                                msk16 = (__mmask16)(msk >> 32u);
                            }
                        }
                        if (QPLC_STS_OK == status) {
                            msk16 = (__mmask16)(msk >> 48u);
                            if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                                z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                                x_data = _mm512_cvtepi32_epi16(z_data);
                                msk16 = _bzhi_u32(0xffff, num_data);
                                x_data = _mm256_add_epi16(x_data, x_index);
                                if ((dst_ptr + num_data) <= end_ptr) {
                                    _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
                                    dst_ptr += num_data;
                                }
                                else {
                                    status = QPLC_STS_DST_IS_SHORT_ERR;
                                    act_num_elements -= num_elem_rem;
                                    act_num_elements += 48;
                                    msk16 = (__mmask16)(msk >> 48u);
                                }
                            }
                        }
                    }
                }
            }
            if (QPLC_STS_DST_IS_SHORT_ERR == status) {
                uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
                uint16_t run_1 = 1;
                if (count_last_bytes) {
                    for (; run_1; run_1 += run_1) {
                        act_num_elements++;
                        if (msk16 & run_1) {
                            if (0 == --count_last_bytes) {
                                run_1 += run_1;
                                break;
                            }
                        }
                    }
                }
                for (; run_1; run_1 += run_1) {
                    if (msk16 & run_1) {
                        break;
                    }
                    act_num_elements++;
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk16 = _bzhi_u32(0xffff, num_data);
            _mm256_mask_storeu_epi16((void*)dst_ptr, msk16, x_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem = num_elements & (STEP_SRC_8U16U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U16U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)((int64_t)(-1)), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + num_elements));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = (uint8_t*)dst_ptr;
    return status;
}
#endif

// ********************** 32u ****************************** //
#if defined(DEF_SSE42) || defined(DEF_MM256)
#define DEF_MM256_32U
#endif

#if defined(DEF_MM512v0) || defined(DEF_MM512v1)
#define DEF_MM512_32U
#endif

#if defined(DEF_MM256_32U)
// 256-bit intrinsics code (the Best)

#define STEP_SRC_8U32U    32

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u32u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t** pp_dst,
    uint32_t dst_length,
    uint32_t* index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint32_t* dst_ptr = (uint32_t*)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m256i         y_index0 = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    __m256i         y_index1 = _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15);
    __m256i         y_index2 = _mm256_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23);
    __m256i         y_index3 = _mm256_setr_epi32(24, 25, 26, 27, 28, 29, 30, 31);
    __m256i         y_data;
    __m256i         y_zero = _mm256_setzero_si256();
    __m256i         y_index;
    __mmask32       msk;
    __mmask8        msk8;

    /* We can only do processing when the index is less than 256. */
    act_num_elements = OWN_MAX_32U;
    if (index)
        act_num_elements -= (index - 1);
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U32U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    dst_length >>= 2;   /* the length points to number of elements now */

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U32U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                y_index = _mm256_set1_epi32(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                y_index = _mm256_set1_epi32(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
            }
        }
    }
    else {
        /* Here we are obliged to check. */
        uint32_t* end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U32U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                y_index = _mm256_set1_epi32(index + i);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x0000ff00) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0x00ff0000) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
                if (msk & 0xff000000) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        break;
                    }
                    _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                    dst_ptr += num_data;
                }
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                y_index = _mm256_set1_epi32(index + act_num_elements_align);
                if (msk & 0x000000ff) {
                    msk8 = (__mmask8)msk;
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                        dst_ptr += num_data;
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x0000ff00)) {
                    msk8 = (__mmask8)(msk >> 8);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                        dst_ptr += num_data;
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0x00ff0000)) {
                    msk8 = (__mmask8)(msk >> 16);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                        dst_ptr += num_data;
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
                if ((QPLC_STS_OK == status) && (msk & 0xff000000)) {
                    msk8 = (__mmask8)(msk >> 24);
                    y_data = _mm256_maskz_compress_epi32(msk8, y_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk8);
                    y_data = _mm256_add_epi32(y_data, y_index);
                    msk8 = _bzhi_u32(0xff, num_data);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
                        dst_ptr += num_data;
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                    }
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk8 = _bzhi_u32(0xff, num_data);
            _mm256_mask_storeu_epi32((void*)dst_ptr, msk8, y_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem &= (STEP_SRC_8U32U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U32U) {
            y_data = _mm256_loadu_si256((__m256i const*)(src_ptr + i));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u32((uint32_t)(-1), num_elem_rem);
            y_data = _mm256_maskz_loadu_epi8(msk, (void const*)(src_ptr + num_elements));
            msk = _mm256_cmpneq_epi8_mask(y_data, y_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = (uint8_t*)dst_ptr;
    return status;
}
#endif

#if defined(DEF_MM512_32U)
// 512-bit intrinsics code (the Best)

#define STEP_SRC_8U32U  (64u)

OWN_OPT_FUN(qplc_status_t, k0_qplc_pack_index_8u32u, (const uint8_t* src_ptr,
    uint32_t num_elements,
    uint8_t** pp_dst,
    uint32_t dst_length,
    uint32_t* index_ptr)) {
    uint32_t        index = *index_ptr;
    qplc_status_t   status = QPLC_STS_OK;
    uint32_t* dst_ptr = (uint32_t*)*pp_dst;
    uint32_t        act_num_elements;
    uint32_t        act_num_elements_align;
    uint32_t        num_elem_rem;
    uint32_t        num_data;
    __m512i         z_index0 = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m512i         z_index1 = _mm512_setr_epi32(16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    __m512i         z_index2 = _mm512_setr_epi32(32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47);
    __m512i         z_index3 = _mm512_setr_epi32(48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63);
    __m512i         z_data;
    __m512i         z_zero = _mm512_setzero_si512();
    __m512i         z_index;
    __mmask64       msk;
    __mmask16       msk16;


    /* We can only do processing when the index is less than 256. */
    act_num_elements = OWN_MAX_32U;
    if (index)
        act_num_elements -= (index - 1);
    if (act_num_elements > num_elements)
        act_num_elements = num_elements;
    num_elem_rem = act_num_elements & (STEP_SRC_8U32U - 1);
    act_num_elements_align = act_num_elements - num_elem_rem;

    dst_length >>= 2;   /* the length points to number of elements now */

    if (dst_length >= act_num_elements) {
        /* If the length of the output buffer is greater than the number of
           actually processed input elements, we can skip checking */
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U32U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                z_index = _mm512_set1_epi32(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (num_elem_rem) {
            msk = _bzhi_u64((uint64_t)(-1), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                z_index = _mm512_set1_epi32(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
            }
        }
    }
    else {
        /* Here we are obliged to check. */
        uint32_t* end_ptr = dst_ptr + dst_length;
        for (uint32_t i = 0u; (i < act_num_elements_align); i += STEP_SRC_8U32U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                z_index = _mm512_set1_epi32(index + i);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 0u;
                        msk16 = (__mmask16)msk;
                        break;
                    }
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 16u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 16u;
                        msk16 = (__mmask16)(msk >> 16u);
                        break;
                    }
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 32u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 32u;
                        msk16 = (__mmask16)(msk >> 32u);
                        break;
                    }
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
                msk16 = (__mmask16)(msk >> 48u);
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    if ((dst_ptr + num_data) > end_ptr) {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements = i + 48u;
                        msk16 = (__mmask16)(msk >> 48u);
                        break;
                    }
                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                    dst_ptr += num_data;
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
            uint16_t run_1 = 1;
            if (count_last_bytes) {
                for (; run_1; run_1 += run_1) {
                    act_num_elements++;
                    if (msk16 & run_1) {
                        if (0 == --count_last_bytes) {
                            run_1 += run_1;
                            break;
                        }
                    }
                }
            }
            for (; run_1; run_1 += run_1) {
                if (msk16 & run_1) {
                    break;
                }
                act_num_elements++;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)(-1), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + act_num_elements_align));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                z_index = _mm512_set1_epi32(index + act_num_elements_align);
                msk16 = (__mmask16)msk;
                if (msk16 != 0) {
                    z_data = _mm512_maskz_compress_epi32(msk16, z_index0);
                    num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                    z_data = _mm512_add_epi32(z_data, z_index);
                    msk16 = _bzhi_u32(0xffff, num_data);
                    if ((dst_ptr + num_data) <= end_ptr) {
                        _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                        dst_ptr += num_data;
                    }
                    else {
                        status = QPLC_STS_DST_IS_SHORT_ERR;
                        act_num_elements -= num_elem_rem;
                        msk16 = (__mmask16)msk;
                    }
                }
                if (QPLC_STS_OK == status) {
                    msk16 = (__mmask16)(msk >> 16u);
                    if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                        z_data = _mm512_maskz_compress_epi32(msk16, z_index1);
                        num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                        z_data = _mm512_add_epi32(z_data, z_index);
                        msk16 = _bzhi_u32(0xffff, num_data);
                        if ((dst_ptr + num_data) <= end_ptr) {
                            _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                            dst_ptr += num_data;
                        }
                        else {
                            status = QPLC_STS_DST_IS_SHORT_ERR;
                            act_num_elements -= num_elem_rem;
                            act_num_elements += 16;
                            msk16 = (__mmask16)(msk >> 16u);
                        }
                    }
                    if (QPLC_STS_OK == status) {
                        msk16 = (__mmask16)(msk >> 32u);
                        if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                            z_data = _mm512_maskz_compress_epi32(msk16, z_index2);
                            num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                            z_data = _mm512_add_epi32(z_data, z_index);
                            msk16 = _bzhi_u32(0xffff, num_data);
                            if ((dst_ptr + num_data) <= end_ptr) {
                                _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                                dst_ptr += num_data;
                            }
                            else {
                                status = QPLC_STS_DST_IS_SHORT_ERR;
                                act_num_elements -= num_elem_rem;
                                act_num_elements += 32;
                                msk16 = (__mmask16)(msk >> 32u);
                            }
                        }
                        if (QPLC_STS_OK == status) {
                            msk16 = (__mmask16)(msk >> 48u);
                            if ((msk16 != 0) && (QPLC_STS_OK == status)) {
                                z_data = _mm512_maskz_compress_epi32(msk16, z_index3);
                                num_data = (uint32_t)_mm_popcnt_u32((uint32_t)msk16);
                                z_data = _mm512_add_epi32(z_data, z_index);
                                msk16 = _bzhi_u32(0xffff, num_data);
                                if ((dst_ptr + num_data) <= end_ptr) {
                                    _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
                                    dst_ptr += num_data;
                                }
                                else {
                                    status = QPLC_STS_DST_IS_SHORT_ERR;
                                    act_num_elements -= num_elem_rem;
                                    act_num_elements += 48;
                                    msk16 = (__mmask16)(msk >> 48u);
                                }
                            }
                        }
                    }
                }
            }
            if (QPLC_STS_DST_IS_SHORT_ERR == status) {
                uint32_t count_last_bytes = (uint32_t)(end_ptr - dst_ptr);
                uint16_t run_1 = 1;
                if (count_last_bytes) {
                    for (; run_1; run_1 += run_1) {
                        act_num_elements++;
                        if (msk16 & run_1) {
                            if (0 == --count_last_bytes) {
                                run_1 += run_1;
                                break;
                            }
                        }
                    }
                }
                for (; run_1; run_1 += run_1) {
                    if (msk16 & run_1) {
                        break;
                    }
                    act_num_elements++;
                }
            }
        }
        if (QPLC_STS_DST_IS_SHORT_ERR == status) {
            num_data = (uint32_t)(end_ptr - dst_ptr);
            msk16 = _bzhi_u32(0xffff, num_data);
            _mm512_mask_storeu_epi32((void*)dst_ptr, msk16, z_data);
            dst_ptr += num_data;
        }
    } // if (dst_length >= act_num_elements) 
    *index_ptr += act_num_elements;
    if ((act_num_elements < num_elements) && (QPLC_STS_OK == status)) {
        src_ptr += act_num_elements;
        num_elements -= act_num_elements;
        num_elem_rem = num_elements & (STEP_SRC_8U32U - 1);
        num_elements -= num_elem_rem;
        for (uint32_t i = 0u; (i < num_elements); i += STEP_SRC_8U32U) {
            z_data = _mm512_loadu_si512((__m512i const*)(src_ptr + i));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += i + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
                break;
            }
        }
        if ((QPLC_STS_OK == status) && num_elem_rem) {
            msk = _bzhi_u64((uint64_t)(-1), num_elem_rem);
            z_data = _mm512_maskz_loadu_epi8(msk, (void const*)(src_ptr + num_elements));
            msk = _mm512_cmpneq_epi8_mask(z_data, z_zero);
            if (msk != 0) {
                *index_ptr += num_elements + _tzcnt_u32((uint32_t)msk);
                status = QPLC_STS_OUTPUT_OVERFLOW_ERR;
            }
        }
        if (QPLC_STS_OK == status)
            *index_ptr += num_elements + num_elem_rem;
    }
    *pp_dst = (uint8_t*)dst_ptr;
    return status;
}
#endif


#endif // OWN_PACK_INDEX_H
