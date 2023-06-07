/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

 /**
  * @brief Contains implementation of functions for Intel® Query Processing Library (Intel® QPL)
  * memory group functions
  *
  * @date 07/06/2020
  *
  * @details Function list:
  *          - @ref k0_qplc_zero_8u
  *          - @ref k0_qplc_copy_8u
  */

#ifndef OWN_MEMOP_H
#define OWN_MEMOP_H

#include "own_qplc_defs.h"
#include "qplc_memop.h"

// ********************** Zero ****************************** //

OWN_QPLC_INLINE(void, own_zero_8u, (uint8_t* dst_ptr, uint32_t length))
{
    uint32_t length_64u = length / sizeof(uint64_t);

    uint64_t* data_64u_ptr = (uint64_t*)dst_ptr;

    // todo: create pragma macros: unroll WIN/LIN
    while (length_64u >= 4) {
        data_64u_ptr[0] = 0u;
        data_64u_ptr[1] = 0u;
        data_64u_ptr[2] = 0u;
        data_64u_ptr[3] = 0u;

        length_64u -= 4;
        data_64u_ptr += 4;
    }

    // todo: Use masks
    for (uint32_t i = 0; i < length_64u; i++) {
        *data_64u_ptr++ = 0u;
    }

    uint32_t remaining_bytes = length % sizeof(uint64_t);

    dst_ptr = (uint8_t*)data_64u_ptr;

    while (remaining_bytes >= 2) {
        dst_ptr[0] = 0u;
        dst_ptr[1] = 0u;

        remaining_bytes -= 2u;
        dst_ptr += 2;
    }

    if (remaining_bytes) {
        *dst_ptr = 0u;
    }
}

OWN_QPLC_INLINE(void, k0_qplc_zero_8u_unaligned, (uint8_t* dst_ptr, uint32_t len))
{
    uint32_t length_512u = len / sizeof(__m512i);
    while (length_512u > 0) {
        _mm512_storeu_si512(dst_ptr, _mm512_setzero_si512());
        dst_ptr += 64u;
        --length_512u;
    }

    uint32_t remaining_bytes = len % sizeof(__m512i);
    own_zero_8u(dst_ptr, remaining_bytes);
}

OWN_QPLC_INLINE(uint32_t, own_get_align, (uint64_t ptr))
{
    uint64_t aligned_ptr = (ptr + 63u) & ~63LLu;
    return (uint32_t)(aligned_ptr - ptr);
}

OWN_QPLC_INLINE(void, k0_qplc_zero_8u_tail, (uint8_t* dst_ptr, uint32_t len))
{
    __mmask64 store_mask = (1LLu << len) - 1u;
    _mm512_mask_storeu_epi8(dst_ptr, store_mask, _mm512_setzero_si512());
}

OWN_OPT_FUN(void, k0_qplc_zero_8u, (uint8_t* dst_ptr, uint32_t length))
{
    if (length < 4096u) {
        k0_qplc_zero_8u_unaligned(dst_ptr, length);
        return;
    }

    uint32_t align = own_get_align((uint64_t)dst_ptr);
    k0_qplc_zero_8u_tail(dst_ptr, align);
    length -= align;
    dst_ptr += align;

    uint32_t length_512u = length / sizeof(__m512i);

    while (length_512u > 0) {
        _mm512_store_si512(dst_ptr, _mm512_setzero_si512());
        dst_ptr += 64u;
        --length_512u;
    }

    uint32_t remaining_bytes = length % sizeof(__m512i);
    k0_qplc_zero_8u_tail(dst_ptr, remaining_bytes);
}

OWN_QPLC_INLINE(void, own_copy_8u_unrolled, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length)) {
    const uint64_t *src_64u_ptr = (uint64_t *)src_ptr;
    uint64_t *dst_64u_ptr = (uint64_t *)dst_ptr;

    uint32_t length_64u = length / sizeof(uint64_t);
    uint32_t tail_start = length_64u * sizeof(uint64_t);

    while (length_64u > 3u) {
        dst_64u_ptr[0] = src_64u_ptr[0];
        dst_64u_ptr[1] = src_64u_ptr[1];
        dst_64u_ptr[2] = src_64u_ptr[2];
        dst_64u_ptr[3] = src_64u_ptr[3];

        dst_64u_ptr += 4u;
        src_64u_ptr += 4u;
        length_64u -= 4u;
    }

    for (uint32_t i = 0u; i < length_64u; ++i) {
        dst_64u_ptr[i] = src_64u_ptr[i];
    }

    for (uint32_t i = tail_start; i < length; ++i) {
        dst_ptr[i] = src_ptr[i];
    }
}

OWN_OPT_FUN(void, k0_qplc_copy_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr, uint32_t length)) {
    if (length < 64u) {
        own_copy_8u_unrolled(src_ptr, dst_ptr, length);
        return;
    }

    // SIMD512 intrinsic code here is slower than AVX2 intrinsic code
    // TODO: add version with stream for big sizes and calculate cache
    uint32_t length256u = length / sizeof(__m256i);
    uint32_t tail = length % sizeof(__m256i);
    while (length256u > 3u) {
        __m256i zmm0 = _mm256_loadu_si256((const __m256i *)src_ptr);
        __m256i zmm1 = _mm256_loadu_si256((const __m256i *)(src_ptr + 32u));
        __m256i zmm2 = _mm256_loadu_si256((const __m256i *)(src_ptr + 64u));
        __m256i zmm3 = _mm256_loadu_si256((const __m256i *)(src_ptr + 96u));
        _mm256_storeu_si256((__m256i *)dst_ptr, zmm0);
        _mm256_storeu_si256((__m256i *)(dst_ptr + 32u), zmm1);
        _mm256_storeu_si256((__m256i *)(dst_ptr + 64u), zmm2);
        _mm256_storeu_si256((__m256i *)(dst_ptr + 96u), zmm3);
        src_ptr += 128u;
        dst_ptr += 128u;
        length256u -= 4;
    }

    while (length256u > 0u) {
        __m256i zmm0 = _mm256_loadu_si256((const __m256i *)src_ptr);
        _mm256_storeu_si256((__m256i *)dst_ptr, zmm0);
        src_ptr += 32u;
        dst_ptr += 32u;
        --length256u;
    }

    own_copy_8u_unrolled(src_ptr, dst_ptr, tail);
}

OWN_OPT_FUN(void, k0_qplc_move_8u, (const uint8_t* src_ptr, uint8_t* dst_ptr, uint32_t length)) {
    __mmask64   msk64;

    if (length <= 64) {
        if (length) {
            msk64 = (uint64_t)((int64_t)(-1)) >> (64 - length);
            _mm512_mask_storeu_epi8((void*)dst_ptr, msk64, _mm512_maskz_loadu_epi8(msk64, (void const*)src_ptr));
        }
    }
    else {
        uint32_t rem;
        if (OWN_QPLC_UINT_PTR(src_ptr) < OWN_QPLC_UINT_PTR(dst_ptr)) {
            src_ptr += length;
            dst_ptr += length;
            rem = 63 & (uint32_t)((uint64_t)dst_ptr);
            if (rem) {
                src_ptr -= rem;
                dst_ptr -= rem;
                length -= rem;
                msk64 = (uint64_t)((int64_t)(-1)) >> (64 - rem);
                _mm512_mask_storeu_epi8((void*)dst_ptr, msk64, _mm512_maskz_loadu_epi8(msk64, (void const*)src_ptr));
            }
            rem = length & 63;
            length -= rem;
            src_ptr -= length;
            dst_ptr -= length;
            for (int64_t indx = (int64_t)length - 64; indx >= 0; indx -= 64) {
                _mm512_store_si512((void*)(dst_ptr + indx), _mm512_loadu_si512((void const*)(src_ptr + indx)));
            }
            if (rem) {
                msk64 = (uint64_t)((int64_t)(-1)) << (64 - rem);
                _mm512_mask_storeu_epi8((void*)(dst_ptr - 64), msk64, _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr - 64)));
            }
        }
        else {
            rem = 63 & (uint32_t)((uint64_t)dst_ptr);
            if (rem) {
                msk64 = (uint64_t)((int64_t)(-1)) >> rem;
                rem = 64 - rem;
                _mm512_mask_storeu_epi8((void*)dst_ptr, msk64, _mm512_maskz_loadu_epi8(msk64, (void const*)src_ptr));
                src_ptr += rem;
                dst_ptr += rem;
                length -= rem;
            }
            rem = length & 63;
            length -= rem;
            for (uint32_t indx = 0; indx < length; indx += 64) {
                _mm512_store_si512((void*)(dst_ptr + indx), _mm512_loadu_si512((void const*)(src_ptr + indx)));
            }
            if (rem) {
                msk64 = (uint64_t)((int64_t)(-1)) >> (64 - rem);
                _mm512_mask_storeu_epi8((void*)(dst_ptr + length), msk64, _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length)));
            }
        }
    }
}

#endif // OWN_MEMOP_H
