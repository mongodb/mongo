/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
* @brief Contains implementation of functions for checksum
* @date 03/19/2021
*
*@details Function list :
*               -@ref k0_qplc_crc32_8u
*               -@ref k0_qplc_xor_checksum_8u
*/

//  See details in the article
// "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"


#ifndef OWN_CHECKSUM_H
#define OWN_CHECKSUM_H

#include "own_qplc_defs.h"
#include "own_qplc_data.h"
#include "immintrin.h"

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
// There is the problem with compiler of MSVC2017.
#pragma optimize("", off)
#endif
#endif

OWN_OPT_FUN(uint32_t, k0_qplc_xor_checksum_8u,
    (const uint8_t* src_ptr,
        uint32_t length,
        uint32_t init_xor)) {
        uint32_t checksum = 0;

    if (length > 64) {
        __m512i     zmm_sum = _mm512_loadu_si512((void const*)src_ptr);
        __m256i     ymm0;
        __m128i     xmm0;
        uint64_t    sum0;
        uint64_t    sum1;
        uint32_t    remind = length & 127;
        if (length >= 128) {
            __m512i     zmm_sum_1 = _mm512_loadu_si512((void const*)(src_ptr + 64));
            remind = length & 127;
            length -= 128;
            src_ptr += 128;
            length -= remind;
            for (uint32_t i = 0; i < length; i += 128) {
                zmm_sum   = _mm512_xor_si512(zmm_sum,   _mm512_loadu_si512((void const*)(src_ptr + i)));
                zmm_sum_1 = _mm512_xor_si512(zmm_sum_1, _mm512_loadu_si512((void const*)(src_ptr + i + 64)));
            }
            zmm_sum = _mm512_xor_si512(zmm_sum, zmm_sum_1);
            if (remind & 64) {
                zmm_sum = _mm512_xor_si512(zmm_sum, _mm512_loadu_si512((void const*)(src_ptr + length)));
                length += 64;
                remind &= 63;
            }
            if (remind) {
                __mmask64 msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
                zmm_sum = _mm512_xor_si512(zmm_sum, _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + length)));
            }
        } else {
            remind = length & 63;
            __mmask64 msk64 = (__mmask64)_bzhi_u64((uint64_t)((int64_t)(-1)), remind);
            zmm_sum = _mm512_xor_si512(zmm_sum, _mm512_maskz_loadu_epi8(msk64, (void const*)(src_ptr + 64)));

        }
        ymm0 = _mm512_extracti64x4_epi64(zmm_sum, 1);
        ymm0 = _mm256_xor_si256(ymm0, _mm512_castsi512_si256(zmm_sum));
        xmm0 = _mm256_extracti128_si256(ymm0, 1);
        xmm0 = _mm_xor_si128(xmm0, _mm256_castsi256_si128(ymm0));
        sum1 = _mm_extract_epi64(xmm0, 1);
        sum0 = _mm_cvtsi128_si64(xmm0);
        sum0 ^= sum1;
        checksum = (uint32_t)(sum0 ^ (sum0 >> 32));
        checksum ^= (checksum >> 16);
        checksum = init_xor ^ (checksum & 0xffff);
        return checksum;
    }
    if (length > 32) {
        __mmask32   msk32 = (__mmask32)_bzhi_u32(0xffffffff, (length - 32));
        __m256i     ymm0 = _mm256_loadu_si256((__m256i const*)src_ptr);
        __m256i     ymm1 = _mm256_maskz_loadu_epi8(msk32, (void const*)(src_ptr + 32));
        __m128i     xmm0;
        uint64_t    sum0;
        uint64_t    sum1;

        ymm0 = _mm256_xor_si256(ymm0, ymm1);
        xmm0 = _mm256_extracti128_si256(ymm0, 1);
        xmm0 = _mm_xor_si128(xmm0, _mm256_castsi256_si128(ymm0));
        sum1 = _mm_extract_epi64(xmm0, 1);
        sum0 = _mm_cvtsi128_si64(xmm0);
        sum0 ^= sum1;
        checksum = (uint32_t)(sum0 ^ (sum0 >> 32));
        checksum ^= (checksum >> 16);
        checksum = init_xor ^ (checksum & 0xffff);
        return checksum;
    }

    if (length > 16) {
        __mmask16   msk16 = (__mmask16)_bzhi_u32(0xffff, (length - 16));
        __m128i     xmm0 = _mm_loadu_si128((__m128i const*)src_ptr);
        __m128i     xmm1 = _mm_maskz_loadu_epi8(msk16, (void const*)(src_ptr + 16));
        uint64_t    sum0;
        uint64_t    sum1;

        xmm0 = _mm_xor_si128(xmm0, xmm1);
        sum1 = _mm_extract_epi64(xmm0, 1);
        sum0 = _mm_cvtsi128_si64(xmm0);
        sum0 ^= sum1;
        checksum = (uint32_t)(sum0 ^ (sum0 >> 32));
        checksum ^= (checksum >> 16);
        checksum = init_xor ^ (checksum & 0xffff);
        return checksum;
    }
    if (length > 8) {
        uint64_t remind = 16 - length;
        uint64_t sum0 = *(uint64_t*)src_ptr;
        uint64_t sum1 = *(uint64_t*)(src_ptr + 8 - remind);
        sum1 >>= (remind << 3);
        sum0 ^= sum1;
        checksum = (uint32_t)(sum0 ^ (sum0 >> 32));
        checksum ^= (checksum >> 16);
        checksum = init_xor ^ (checksum & 0xffff);
        return checksum;
    }
    checksum = 0;
    for (uint32_t i = 0; i < (length & ~1); i += 2) {
        checksum ^= (uint32_t)(*(uint16_t*)(src_ptr + i));
    }
    if (length & 1) {
        checksum ^= (uint32_t)src_ptr[length - 1];
    }
    return init_xor ^ checksum ;
}

#if defined _MSC_VER
#if _MSC_VER <= 1916
// if MSVC <= MSVC2017
#pragma optimize("", on)
#endif
#endif


OWN_QPLC_INLINE(void, k0_qplc_crc64_init, (uint64_t polynomial, uint64_t *remainders, uint64_t *barrett)) {
    // 1. calculating lookup table
    uint64_t lookup_table[256];
    lookup_table[0] = 0u;
    lookup_table[1] = polynomial;
    uint64_t crc = polynomial;

    for (uint32_t major_idx = 2u; major_idx <= 128u; major_idx <<= 1u) {
        // calculating powers of 2
        crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        lookup_table[major_idx] = crc;
        // calculating other numbers based on rule:
        // table[a ^ b] = table[a] ^ table[b]
        for (uint32_t minor_idx = 1u; minor_idx < major_idx; ++minor_idx) {
            lookup_table[major_idx + minor_idx] = crc ^ lookup_table[minor_idx];
        }
    }

    // 2. calculating folding constants (x^T mod poly and x^(T + 64) mod poly)
    // and constant for Barrett reduction (floor(x^128 / poly))
    crc = polynomial;
    uint64_t crc64_barrett = 0u;
    for (uint32_t idx = 0; idx < 8u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    *barrett = crc64_barrett;
    remainders[0] = crc; // x^128 mod poly

    for (uint32_t idx = 8; idx < 16u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[1] = crc; // x^192 mod poly

    for (uint32_t idx = 16; idx < 56u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[2] = crc; // x^512 mod poly

    for (uint32_t idx = 56; idx < 64u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[3] = crc; // x^576 mod poly
}

OWN_QPLC_INLINE(void, k0_qplc_crc64_init_no_unroll, (uint64_t polynomial, uint64_t *remainders, uint64_t *barrett)) {
    uint64_t crc = polynomial;

    uint64_t crc64_barrett = 0u;
    for (uint32_t idx = 0; idx < 8u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    *barrett = crc64_barrett;
    remainders[0] = crc; // x^128 mod poly

    for (uint32_t idx = 8; idx < 16u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    remainders[1] = crc; // x^192 mod poly

}

OWN_QPLC_INLINE(void, own_shift_two_lanes, (int offset, __m128i *_xmm0, __m128i *_xmm1)) {
    __m128i xmm0 = *_xmm0;
    __m128i xmm1 = *_xmm1;

    switch (offset) {
    case 15:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 1);
        xmm1 = _mm_srli_si128(xmm1, 1);
        break;
    case 14:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 2);
        xmm1 = _mm_srli_si128(xmm1, 2);
        break;
    case 13:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 3);
        xmm1 = _mm_srli_si128(xmm1, 3);
        break;
    case 12:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 4);
        xmm1 = _mm_srli_si128(xmm1, 4);
        break;
    case 11:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 5);
        xmm1 = _mm_srli_si128(xmm1, 5);
        break;
    case 10:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 6);
        xmm1 = _mm_srli_si128(xmm1, 6);
        break;
    case 9:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 7);
        xmm1 = _mm_srli_si128(xmm1, 7);
        break;
    case 8:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 8);
        xmm1 = _mm_srli_si128(xmm1, 8);
        break;
    case 7:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 9);
        xmm1 = _mm_srli_si128(xmm1, 9);
        break;
    case 6:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 10);
        xmm1 = _mm_srli_si128(xmm1, 10);
        break;
    case 5:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 11);
        xmm1 = _mm_srli_si128(xmm1, 11);
        break;
    case 4:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 12);
        xmm1 = _mm_srli_si128(xmm1, 12);
        break;
    case 3:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 13);
        xmm1 = _mm_srli_si128(xmm1, 13);
        break;
    case 2:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 14);
        xmm1 = _mm_srli_si128(xmm1, 14);
        break;
    case 1:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 15);
        xmm1 = _mm_srli_si128(xmm1, 15);
        break;
    default:
        xmm0 = _mm_alignr_epi8(xmm1, xmm0, 16);
        xmm1 = _mm_srli_si128(xmm1, 16);
    }

    *_xmm0 = xmm0;
    *_xmm1 = xmm1;
}

OWN_QPLC_INLINE(uint64_t, own_get_inversion, (uint64_t polynomial)) {
    polynomial |= (polynomial << 1);
    polynomial |= (polynomial << 2);
    polynomial |= (polynomial << 4);
    polynomial |= (polynomial << 8);
    polynomial |= (polynomial << 16);
    polynomial |= (polynomial << 32);

    return polynomial;
}

#if defined _MSC_VER
#if _MSC_VER > 1916
/* if MSVC > MSVC2017 */
#pragma optimize("", off)
#endif
#endif
OWN_OPT_FUN(uint64_t, k0_qplc_crc64, (const uint8_t *src_ptr,
                                      uint32_t length,
                                      uint64_t polynomial,
                                      uint8_t inversion_flag)) {
    uint64_t crc = 0u;
    uint64_t inversion_mask = 0u;

    if (inversion_flag) {
        inversion_mask = own_get_inversion(polynomial);
        crc = inversion_mask;
    }

    if (length >= 16u) {
        uint64_t crc64_k[4];
        uint64_t crc64_barrett;
        if (length > 512u) {
            k0_qplc_crc64_init(polynomial, crc64_k, &crc64_barrett);
        }
        else {
            k0_qplc_crc64_init_no_unroll(polynomial, crc64_k, &crc64_barrett);
        }
        uint32_t tail = length % 16u;

        __m128i xmm0, xmm1, xmm2, srcmm;
        __m128i polymm = _mm_set1_epi64x(polynomial);
        __m128i barrett = _mm_set1_epi64x(crc64_barrett);
        __m128i k8 = _mm_set1_epi64x(crc64_k[0]);
        __m128i k16 = _mm_set_epi64x(crc64_k[1], crc64_k[0]);
        __m128i inversion = _mm_set_epi64x(inversion_mask, 0);
        __m128i shuffle_le_mask = _mm_set_epi8(
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F);

        xmm0 = _mm_loadu_si128((const __m128i *)src_ptr);
        xmm0 = _mm_shuffle_epi8(xmm0, shuffle_le_mask);
        xmm0 = _mm_xor_si128(xmm0, inversion);
        src_ptr += 16u;

        // 1. fold by 512bit until remaining length < 2 * 512bits.

        if (length > 512u) {
            __m128i xmm3, xmm4, xmm5, xmm6, xmm7;
            __m128i srcmm1, srcmm2, srcmm3;
            __m128i k64 = _mm_set_epi64x(crc64_k[3], crc64_k[2]);

            xmm2 = _mm_loadu_si128((const __m128i *)src_ptr);
            xmm2 = _mm_shuffle_epi8(xmm2, shuffle_le_mask);
            xmm4 = _mm_loadu_si128((const __m128i *)(src_ptr + 16u));
            xmm4 = _mm_shuffle_epi8(xmm4, shuffle_le_mask);
            xmm6 = _mm_loadu_si128((const __m128i *)(src_ptr + 32u));
            xmm6 = _mm_shuffle_epi8(xmm6, shuffle_le_mask);
            src_ptr += 48u;

            while (length >= 128u) {
                srcmm = _mm_loadu_si128((const __m128i *)src_ptr);
                srcmm = _mm_shuffle_epi8(srcmm, shuffle_le_mask);
                srcmm1 = _mm_loadu_si128((const __m128i *)(src_ptr + 16u));
                srcmm1 = _mm_shuffle_epi8(srcmm1, shuffle_le_mask);
                srcmm2 = _mm_loadu_si128((const __m128i *)(src_ptr + 32u));
                srcmm2 = _mm_shuffle_epi8(srcmm2, shuffle_le_mask);
                srcmm3 = _mm_loadu_si128((const __m128i *)(src_ptr + 48u));
                srcmm3 = _mm_shuffle_epi8(srcmm3, shuffle_le_mask);

                xmm1 = xmm0;
                xmm0 = _mm_clmulepi64_si128(xmm0, k64, 0x00);
                xmm1 = _mm_clmulepi64_si128(xmm1, k64, 0x11);
                xmm0 = _mm_xor_si128(xmm0, srcmm);
                xmm0 = _mm_xor_si128(xmm0, xmm1);

                xmm3 = xmm2;
                xmm2 = _mm_clmulepi64_si128(xmm2, k64, 0x00);
                xmm3 = _mm_clmulepi64_si128(xmm3, k64, 0x11);
                xmm2 = _mm_xor_si128(xmm2, srcmm1);
                xmm2 = _mm_xor_si128(xmm2, xmm3);

                xmm5 = xmm4;
                xmm4 = _mm_clmulepi64_si128(xmm4, k64, 0x00);
                xmm5 = _mm_clmulepi64_si128(xmm5, k64, 0x11);
                xmm4 = _mm_xor_si128(xmm4, srcmm2);
                xmm4 = _mm_xor_si128(xmm4, xmm5);

                xmm7 = xmm6;
                xmm6 = _mm_clmulepi64_si128(xmm6, k64, 0x00);
                xmm7 = _mm_clmulepi64_si128(xmm7, k64, 0x11);
                xmm6 = _mm_xor_si128(xmm6, srcmm3);
                xmm6 = _mm_xor_si128(xmm6, xmm7);

                src_ptr += 64u;
                length -= 64u;
            }

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm2);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm4);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm6);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            length -= 48u;
        }

        // 2. fold by 128bit until remaining length < 2 * 128bits.

        while (length >= 32u) {
            xmm1 = xmm0;
            srcmm = _mm_loadu_si128((const __m128i *)src_ptr);
            srcmm = _mm_shuffle_epi8(srcmm, shuffle_le_mask);
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, srcmm);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            src_ptr += 16u;
            length -= 16u;
        }

        /* 3. if remaining length > 128 bits, then pad zeros to the most-significant bit to grow to 256bits length,
         * then fold once to 128 bits. */

        if (tail) {
            __mmask16 tail_mask = OWN_BIT_MASK(tail);
            srcmm = _mm_maskz_loadu_epi8(tail_mask, (__m128i *)src_ptr);
            srcmm = _mm_shuffle_epi8(srcmm, shuffle_le_mask);

            own_shift_two_lanes(tail, &srcmm, &xmm0);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, srcmm);
            xmm0 = _mm_xor_si128(xmm0, xmm1);
        }

        // 4. Apply 64 bits fold to 64 bits + 64 bits crc(64 zero bits)

        xmm1 = _mm_clmulepi64_si128(xmm0, k8, 0x11);
        xmm0 = _mm_slli_si128(xmm0, 8);
        xmm0 = _mm_xor_si128(xmm0, xmm1);

        /* 5. Use Barrett Reduction algorithm to calculate the 64-bit crc.
         * Output: C(x)  = R(x) mod P(x)
         * Step 1: T1(x) = floor(R(x) / x^64)) * u
         * Step 2: T2(x) = floor(T1(x) / x^64)) * P(x)
         * Step 3: C(x)  = R(x) xor T2(x) mod x^64
         * as u and P(x) are 65-bit values, we use clmul + xor for each multiplication */
        xmm1 = _mm_clmulepi64_si128(xmm0, barrett, 0x11);
        xmm1 = _mm_mask_xor_epi64(xmm1, 0x2, xmm1, xmm0);

        xmm2 = _mm_clmulepi64_si128(xmm1, polymm, 0x11);
        xmm2 = _mm_mask_xor_epi64(xmm2, 0x2, xmm2, xmm1);

        xmm0 = _mm_xor_si128(xmm0, xmm2);

        crc ^= _mm_cvtsi128_si64(xmm0);
    }
    else {
        for (uint32_t i = 0; i < length; ++i) {
            crc ^= (uint64_t)(*src_ptr++) << 56u;
            for (uint32_t j = 0; j < 8u; ++j) {
                crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
            }
        }
        crc ^= inversion_mask;
    }

    return crc;
}
#if defined _MSC_VER
#if _MSC_VER > 1916
/* if MSVC > MSVC2017 */
#pragma optimize("", on)
#endif
#endif

OWN_QPLC_INLINE(uint64_t, bit_reflect, (uint64_t x)) {
    uint64_t y;

    y = bit_reverse_table[x >> 56];
    y |= ((uint64_t)bit_reverse_table[(x >> 48) & 0xFF]) << 8;
    y |= ((uint64_t)bit_reverse_table[(x >> 40) & 0xFF]) << 16;
    y |= ((uint64_t)bit_reverse_table[(x >> 32) & 0xFF]) << 24;
    y |= ((uint64_t)bit_reverse_table[(x >> 24) & 0xFF]) << 32;
    y |= ((uint64_t)bit_reverse_table[(x >> 16) & 0xFF]) << 40;
    y |= ((uint64_t)bit_reverse_table[(x >> 8) & 0xFF]) << 48;
    y |= ((uint64_t)bit_reverse_table[(x >> 0) & 0xFF]) << 56;

    return y;
}

OWN_QPLC_INLINE(void, k0_qplc_crc64_init_be, (uint64_t polynomial, uint64_t *remainders, uint64_t *barrett)) {
    // 1. calculating lookup table
    uint64_t lookup_table[256];
    lookup_table[0] = 0u;
    lookup_table[1] = polynomial;
    uint64_t crc = polynomial;

    for (uint32_t major_idx = 2u; major_idx <= 128u; major_idx <<= 1u) {
        // calculating powers of 2
        crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        lookup_table[major_idx] = crc;
        // calculating other numbers based on rule:
        // table[a ^ b] = table[a] ^ table[b]
        for (uint32_t minor_idx = 1u; minor_idx < major_idx; ++minor_idx) {
            lookup_table[major_idx + minor_idx] = crc ^ lookup_table[minor_idx];
        }
    }

    // 2. calculating folding constants (x^T mod poly and x^(T + 64) mod poly)
    // and constant for Barrett reduction (floor(x^128 / poly))
    crc = polynomial;
    uint64_t crc64_barrett = 0u;
    for (uint32_t idx = 0; idx < 7u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    for (uint32_t j = 0; j < 7u; ++j) {
        crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
        crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
    }

    *barrett = (crc64_barrett << 1) ^ (crc >> 63u);
    remainders[0] = bit_reflect(crc); // x^(128-1) mod poly

    for (uint32_t idx = 8; idx < 16u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[1] = bit_reflect(crc); // x^(192-1) mod poly

    for (uint32_t idx = 16; idx < 56u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[2] = bit_reflect(crc); // x^(512-1) mod poly

    for (uint32_t idx = 56; idx < 64u; ++idx) {
        crc = (crc << 8) ^ lookup_table[crc >> 56u];
    }
    remainders[3] = bit_reflect(crc); // x^(576-1) mod poly
}

OWN_QPLC_INLINE(void, k0_qplc_crc64_init_no_unroll_be, (uint64_t polynomial, uint64_t *remainders, uint64_t *barrett)) {
    uint64_t crc = polynomial;

    uint64_t crc64_barrett = 0u;
    for (uint32_t idx = 0; idx < 7u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    for (uint32_t j = 0; j < 7u; ++j) {
        crc64_barrett = (crc64_barrett << 1) ^ (crc >> 63u);
        crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
    }

    *barrett = (crc64_barrett << 1) ^ (crc >> 63u);
    remainders[0] = bit_reflect(crc); // x^(128-1) mod poly

    for (uint32_t idx = 8; idx < 16u; ++idx) {
        for (uint32_t j = 0; j < 8u; ++j) {
            crc = (crc << 1) ^ (-(int64_t)(crc >> 63u) & polynomial);
        }
    }
    remainders[1] = bit_reflect(crc); // x^192 mod poly

}

OWN_QPLC_INLINE(void, own_shift_two_lanes_be, (int offset, __m128i *_xmm0, __m128i *_xmm1)) {
    __m128i xmm0 = *_xmm0;
    __m128i xmm1 = *_xmm1;

    switch (offset) {
    case 15:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 15);
        xmm1 = _mm_slli_si128(xmm1, 1);
        break;
    case 14:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 14);
        xmm1 = _mm_slli_si128(xmm1, 2);
        break;
    case 13:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 13);
        xmm1 = _mm_slli_si128(xmm1, 3);
        break;
    case 12:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 12);
        xmm1 = _mm_slli_si128(xmm1, 4);
        break;
    case 11:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 11);
        xmm1 = _mm_slli_si128(xmm1, 5);
        break;
    case 10:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 10);
        xmm1 = _mm_slli_si128(xmm1, 6);
        break;
    case 9:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 9);
        xmm1 = _mm_slli_si128(xmm1, 7);
        break;
    case 8:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 8);
        xmm1 = _mm_slli_si128(xmm1, 8);
        break;
    case 7:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 7);
        xmm1 = _mm_slli_si128(xmm1, 9);
        break;
    case 6:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 6);
        xmm1 = _mm_slli_si128(xmm1, 10);
        break;
    case 5:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 5);
        xmm1 = _mm_slli_si128(xmm1, 11);
        break;
    case 4:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 4);
        xmm1 = _mm_slli_si128(xmm1, 12);
        break;
    case 3:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 3);
        xmm1 = _mm_slli_si128(xmm1, 13);
        break;
    case 2:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 2);
        xmm1 = _mm_slli_si128(xmm1, 14);
        break;
    case 1:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 1);
        xmm1 = _mm_slli_si128(xmm1, 15);
        break;
    default:
        xmm0 = _mm_alignr_epi8(xmm0, xmm1, 0);
        xmm1 = _mm_slli_si128(xmm1, 16);
    }

    *_xmm0 = xmm0;
    *_xmm1 = xmm1;
}

#if defined _MSC_VER
#if _MSC_VER > 1916
/* if MSVC > MSVC2017 */
#pragma optimize("", off)
#endif
#endif
OWN_OPT_FUN(uint64_t, k0_qplc_crc64_be, (const uint8_t *src_ptr,
                                         uint32_t length,
                                         uint64_t polynomial,
                                         uint8_t inversion_flag)) {
    uint64_t crc = 0u;
    uint64_t inversion_mask = 0u;

    if (inversion_flag) {
        inversion_mask = own_get_inversion(polynomial);
        inversion_mask = bit_reflect(inversion_mask);
        crc = inversion_mask;
    }

    if (length >= 16u) {
        uint64_t crc64_k[4];
        uint64_t crc64_barrett;
        if (length > 512u) {
            k0_qplc_crc64_init_be(polynomial, crc64_k, &crc64_barrett);
        }
        else {
            k0_qplc_crc64_init_no_unroll_be(polynomial, crc64_k, &crc64_barrett);
        }
        uint32_t tail = length % 16u;

        __m128i xmm0, xmm1, xmm2, srcmm;
        uint8_t poly_ending = polynomial & 1u;
        __m128i polymm = _mm_set1_epi64x(bit_reflect(polynomial) << 1);
        __m128i barrett = _mm_set1_epi64x((bit_reflect(crc64_barrett) << 1) | 1);
        __m128i k8 = _mm_set1_epi64x(crc64_k[0]);
        __m128i k16 = _mm_set_epi64x(crc64_k[0], crc64_k[1]);
        __m128i inversion = _mm_set_epi64x(0, inversion_mask);

        xmm0 = _mm_loadu_si128((const __m128i *)src_ptr);
        xmm0 = _mm_xor_si128(xmm0, inversion);
        src_ptr += 16u;

        // 1. fold by 512bit until remaining length < 2 * 512bits.

        if (length > 512u) {
            __m128i xmm3, xmm4, xmm5, xmm6, xmm7;
            __m128i srcmm1, srcmm2, srcmm3;
            __m128i k64 = _mm_set_epi64x(crc64_k[2], crc64_k[3]);

            xmm2 = _mm_loadu_si128((const __m128i *)src_ptr);
            xmm4 = _mm_loadu_si128((const __m128i *)(src_ptr + 16u));
            xmm6 = _mm_loadu_si128((const __m128i *)(src_ptr + 32u));
            src_ptr += 48u;

            while (length >= 128u) {
                srcmm = _mm_loadu_si128((const __m128i *)src_ptr);
                srcmm1 = _mm_loadu_si128((const __m128i *)(src_ptr + 16u));
                srcmm2 = _mm_loadu_si128((const __m128i *)(src_ptr + 32u));
                srcmm3 = _mm_loadu_si128((const __m128i *)(src_ptr + 48u));

                xmm1 = xmm0;
                xmm0 = _mm_clmulepi64_si128(xmm0, k64, 0x00);
                xmm1 = _mm_clmulepi64_si128(xmm1, k64, 0x11);
                xmm0 = _mm_xor_si128(xmm0, srcmm);
                xmm0 = _mm_xor_si128(xmm0, xmm1);

                xmm3 = xmm2;
                xmm2 = _mm_clmulepi64_si128(xmm2, k64, 0x00);
                xmm3 = _mm_clmulepi64_si128(xmm3, k64, 0x11);
                xmm2 = _mm_xor_si128(xmm2, srcmm1);
                xmm2 = _mm_xor_si128(xmm2, xmm3);

                xmm5 = xmm4;
                xmm4 = _mm_clmulepi64_si128(xmm4, k64, 0x00);
                xmm5 = _mm_clmulepi64_si128(xmm5, k64, 0x11);
                xmm4 = _mm_xor_si128(xmm4, srcmm2);
                xmm4 = _mm_xor_si128(xmm4, xmm5);

                xmm7 = xmm6;
                xmm6 = _mm_clmulepi64_si128(xmm6, k64, 0x00);
                xmm7 = _mm_clmulepi64_si128(xmm7, k64, 0x11);
                xmm6 = _mm_xor_si128(xmm6, srcmm3);
                xmm6 = _mm_xor_si128(xmm6, xmm7);

                src_ptr += 64u;
                length -= 64u;
            }

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm2);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm4);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, xmm6);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            length -= 48u;
        }

        // 2. fold by 128bit until remaining length < 2 * 128bits.

        while (length >= 32u) {
            xmm1 = xmm0;
            srcmm = _mm_loadu_si128((const __m128i *)src_ptr);
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, srcmm);
            xmm0 = _mm_xor_si128(xmm0, xmm1);

            src_ptr += 16u;
            length -= 16u;
        }

        /* 3. if remaining length > 128 bits, then pad zeros to the most-significant bit to grow to 256bits length,
         * then fold once to 128 bits. */

        if (tail) {
            __mmask16 tail_mask = OWN_BIT_MASK(tail);
            srcmm = _mm_maskz_loadu_epi8(tail_mask, (__m128i *)src_ptr);

            own_shift_two_lanes_be(tail, &srcmm, &xmm0);

            xmm1 = xmm0;
            xmm0 = _mm_clmulepi64_si128(xmm0, k16, 0x00);
            xmm1 = _mm_clmulepi64_si128(xmm1, k16, 0x11);
            xmm0 = _mm_xor_si128(xmm0, srcmm);
            xmm0 = _mm_xor_si128(xmm0, xmm1);
        }

        // 4. Apply 64 bits fold to 64 bits + 64 bits crc(64 zero bits)

        xmm1 = _mm_clmulepi64_si128(xmm0, k8, 0x00);
        xmm0 = _mm_srli_si128(xmm0, 8);
        xmm0 = _mm_xor_si128(xmm0, xmm1);

        /* 5. Use Barrett Reduction algorithm to calculate the 64-bit crc.
         * Output: C(x)  = R(x)' mod P(x)'
         * Step 1: T1(x)' = (R(x)' mod x^64) * u'
         * Step 2: T2(x)' = (T1(x)' mod x^64) * P(x)'
         * Step 3: C(x)  = R(x)' xor T2(x)' mod x^64
         * as u and P(x) are 65-bit values, we use clmul + xor for each multiplication */
        xmm1 = _mm_clmulepi64_si128(xmm0, barrett, 0x00);
        xmm2 = _mm_clmulepi64_si128(xmm1, polymm, 0x00);
        if (poly_ending) {
            xmm2 = _mm_xor_si128(xmm2, _mm_slli_si128(xmm1, 8));
        }
        xmm0 = _mm_xor_si128(xmm0, xmm2);

        crc ^= _mm_extract_epi64(xmm0, 0x1);
    }
    else {
        polynomial = bit_reflect(polynomial);
        for (uint32_t i = 0; i < length; ++i) {
            crc ^= (uint64_t)(*src_ptr++);
            for (uint32_t j = 0; j < 8u; ++j) {
                crc = (crc >> 1) ^ (-(int64_t)(crc & 1u) & polynomial);
            }
        }
        crc ^= inversion_mask;
    }

    return crc;
}
#if defined _MSC_VER
#if _MSC_VER > 1916
/* if MSVC > MSVC2017 */
#pragma optimize("", on)
#endif
#endif

#endif // OWN_CHECKSUM_H
