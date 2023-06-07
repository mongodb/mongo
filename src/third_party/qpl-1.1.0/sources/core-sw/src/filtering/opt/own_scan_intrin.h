/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OWN_SCAN_INTRIN_H
#define OWN_SCAN_INTRIN_H

#include <stdint.h>
#include "immintrin.h"

static inline __m512i own_unpack_1u_kernel(uint8_t *src_ptr, uint32_t start_bit) {
    __m512i srcmm0, srcmm1;
    srcmm0 = _mm512_loadu_si512(src_ptr);
    srcmm1 = _mm512_loadu_si512(src_ptr + 1u);

    srcmm0 = _mm512_srli_epi16(srcmm0, start_bit);     // a - c - e - g - i - k - m - o -
    srcmm1 = _mm512_slli_epi16(srcmm1, 8 - start_bit); // - b - d - f - h - j - l - n - p

    srcmm0 = _mm512_mask_mov_epi8(srcmm0, 0xAAAAAAAAAAAAAAAA, srcmm1); // even from srcmm0, odd from srcmm1
    return srcmm0;
}

// ------ EQ ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are equal)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  start_bit
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_EQ_1u_kernel(uint8_t *src_ptr,
                                         uint8_t *dst_ptr,
                                         uint32_t start_bit,
                                         __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = own_unpack_1u_kernel(src_ptr, start_bit);

    srcmm = _mm512_ternarylogic_epi32(srcmm, srcmm, broadcasted_value, 0x99); // 1001

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 1u elements with the given 1u value (whether they are equal)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_aligned_EQ_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_ternarylogic_epi32(srcmm, srcmm, broadcasted_value, 0x99); // 1001

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_EQ_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_EQ);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_EQ_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_EQ);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_EQ_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_EQ);
}

// ------ GE ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are greater or equal)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_GE_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_ternarylogic_epi32(srcmm, srcmm, broadcasted_value, 0xDD); // 1101

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are greater or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_GE_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_NLT);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are greater or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_GE_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_NLT);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are greater or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_GE_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_NLT);
}

// ------ GT ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are greater)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_GT_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_andnot_si512(broadcasted_value, srcmm); // 0100

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are greater)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_GT_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_NLE);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are greater)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_GT_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_NLE);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are greater)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_GT_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_NLE);
}

// ------ LE ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are lesser or equal)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_LE_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_ternarylogic_epi32(srcmm, srcmm, broadcasted_value, 0xBB); // 1011

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are lesser or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_LE_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_LE);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are lesser or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_LE_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_LE);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are lesser or equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_LE_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_LE);
}

// ------ LT ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are lesser)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  start_bit
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_LT_1u_kernel(uint8_t *src_ptr,
                                         uint8_t *dst_ptr,
                                         uint32_t start_bit,
                                         __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = own_unpack_1u_kernel(src_ptr, start_bit);

    srcmm = _mm512_andnot_si512(srcmm, broadcasted_value); // 0010

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 1u elements with the given 1u value (whether they are lesser)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_LT_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_andnot_si512(srcmm, broadcasted_value); // 0010

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are lesser)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_LT_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_LT);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are lesser)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_LT_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_LT);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are lesser)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_LT_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_LT);
}

// ------ NE ------

/**
 * @brief Compare 1u elements with the given 1u value (whether they are not equal)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_NE_1u_kernel(uint8_t *src_ptr, uint8_t *dst_ptr, __m512i broadcasted_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_xor_si512(srcmm, broadcasted_value); // 0110

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with the given 8u value (whether they are not equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 8u value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_NE_8u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu8_mask(srcmm, broadcasted_value, _MM_CMPINT_NE);
}

/**
 * @brief Compare 16u elements with the given 16u value (whether they are not equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 16u value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_NE_16u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu16_mask(srcmm, broadcasted_value, _MM_CMPINT_NE);
}

/**
 * @brief Compare 32u elements with the given 32u value (whether they are not equal)
 *
 * @param[in]  srcmm              __m512i input register
 * @param[in]  broadcasted_value  __m512i register with broadcasted 32u value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_NE_32u_kernel(__m512i srcmm, __m512i broadcasted_value) {
    return _mm512_cmp_epu32_mask(srcmm, broadcasted_value, _MM_CMPINT_NE);
}

// ------ REQ ------

/**
 * @brief Compare 1u elements with two given 1u values (whether they are in range)
 *
 * @param[in]  src_ptr            Pointer on the source array
 * @param[in]  dst_ptr            Pointer on the destination array
 * @param[in]  broadcasted_value  __m512i register with broadcasted 1u value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_REQ_1u_kernel(uint8_t *src_ptr,
                                                   uint8_t *dst_ptr,
                                                   __m512i lesser_value,
                                                   __m512i greater_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_ternarylogic_epi32(srcmm, lesser_value, greater_value, 0xA3); // 10100011

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with two given 8u values (whether they are in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 8u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 8u greater value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_REQ_8u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask64 mask_GE = _mm512_cmp_epu8_mask(srcmm, lesser_value, _MM_CMPINT_NLT);
    __mmask64 mask_LE = _mm512_cmp_epu8_mask(srcmm, greater_value, _MM_CMPINT_LE);
    return mask_GE & mask_LE;
}

/**
 * @brief Compare 16u elements with two given 16u values (whether they are in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 16u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 16u greater value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_REQ_16u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask32 mask_GE = _mm512_cmp_epu16_mask(srcmm, lesser_value, _MM_CMPINT_NLT);
    __mmask32 mask_LE = _mm512_cmp_epu16_mask(srcmm, greater_value, _MM_CMPINT_LE);
    return mask_GE & mask_LE;
}

/**
 * @brief Compare 32u elements with two given 32u values (whether they are in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 32u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 32u greater value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_REQ_32u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask16 mask_GE = _mm512_cmp_epu32_mask(srcmm, lesser_value, _MM_CMPINT_NLT);
    __mmask16 mask_LE = _mm512_cmp_epu32_mask(srcmm, greater_value, _MM_CMPINT_LE);
    return mask_GE & mask_LE;
}

// ------ RNE ------

/**
 * @brief Compare 1u elements with two given 1u values (whether they are not in range)
 *
 * @param[in]  src_ptr        Pointer on the source array
 * @param[in]  dst_ptr        Pointer on the destination array
 * @param[in]  lesser_value   __m512i register with broadcasted 1u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 1u greater value to compare with
 *
 * @return    void
 */
static inline void own_scan_pipeline_RNE_1u_kernel(uint8_t *src_ptr,
                                                   uint8_t *dst_ptr,
                                                   __m512i lesser_value,
                                                   __m512i greater_value) {
    __m512i srcmm;
    srcmm = _mm512_loadu_si512(src_ptr);

    srcmm = _mm512_ternarylogic_epi32(srcmm, lesser_value, greater_value, 0x18); // 00011000

    _mm512_storeu_si512(dst_ptr, srcmm);
}

/**
 * @brief Compare 8u elements with two given 8u values (whether they are not in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 8u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 8u greater value to compare with
 *
 * @return  64-bit cmp mask
 */
static inline __mmask64 own_scan_RNE_8u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask64 mask_LT = _mm512_cmp_epu8_mask(srcmm, lesser_value, _MM_CMPINT_LT);
    __mmask64 mask_GT = _mm512_cmp_epu8_mask(srcmm, greater_value, _MM_CMPINT_NLE);
    return mask_LT | mask_GT;
}

/**
 * @brief Compare 16u elements with two given 16u values (whether they are not in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 16u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 16u greater value to compare with
 *
 * @return  32-bit cmp mask
 */
static inline __mmask32 own_scan_RNE_16u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask32 mask_LT = _mm512_cmp_epu16_mask(srcmm, lesser_value, _MM_CMPINT_LT);
    __mmask32 mask_GT = _mm512_cmp_epu16_mask(srcmm, greater_value, _MM_CMPINT_NLE);
    return mask_LT | mask_GT;
}

/**
 * @brief Compare 32u elements with two given 32u values (whether they are not in range between these values)
 *
 * @param[in]  srcmm          __m512i input register
 * @param[in]  lesser_value   __m512i register with broadcasted 32u lesser value to compare with
 * @param[in]  greater_value  __m512i register with broadcasted 32u greater value to compare with
 *
 * @return  16-bit cmp mask
 */
static inline __mmask16 own_scan_RNE_32u_kernel(__m512i srcmm, __m512i lesser_value, __m512i greater_value) {
    __mmask16 mask_LT = _mm512_cmp_epu32_mask(srcmm, lesser_value, _MM_CMPINT_LT);
    __mmask16 mask_GT = _mm512_cmp_epu32_mask(srcmm, greater_value, _MM_CMPINT_NLE);
    return mask_LT | mask_GT;
}
#endif // OWN_SCAN_INTRIN_H
