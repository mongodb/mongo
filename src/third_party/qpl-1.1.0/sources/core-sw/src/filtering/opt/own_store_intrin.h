/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OWN_STORE_INTRIN_H
#define OWN_STORE_INTRIN_H

#include <stdint.h>
#include "immintrin.h"

// !!!
// haven't got test on these yet. So it might work incorrectly.
// !!!

// ------ LE mask ------

/**
 * @brief Store 64-bit mask in dst (dst is aligned on 64 bits)
 *
 * @param[in]  dst_ptr  pointer to the dst
 * @param[in]  data     64-bit element to store
 *
 * @return  void
 */
static inline void own_store_8u_LE_kernel(uint8_t *dst_ptr, __mmask64 data) {
    *(uint64_t *) dst_ptr = data;
}

/**
 * @brief Store 32-bit mask in dst (dst is aligned on 32 bits)
 *
 * @param[in]  dst_ptr  pointer to the dst
 * @param[in]  data     32-bit element to store
 *
 * @return  void
 */
static inline void own_store_16u_LE_kernel(uint8_t *dst_ptr, __mmask32 data) {
    *(uint32_t *) dst_ptr = data;
}

/**
 * @brief Store 16-bit mask in dst (dst is aligned on 16 bits)
 *
 * @param[in]  dst_ptr  pointer to the dst
 * @param[in]  data     16-bit element to store
 *
 * @return  void
 */

static inline void own_store_32u_LE_kernel(uint8_t *dst_ptr, __mmask16 data) {
    *(uint16_t *) dst_ptr = data;
}

// ------ LE mask tail ------

/**
 * @brief Store (length) bits from 64-bit mask in dst (dst might not be aligned)
 *
 * @param[in]  dst_ptr  Pointer to the dst. First byte of dst contains 0 to 7 bits that must be not overwrited
 * @param[in]  data     64-bit element to store
 * @param[in]  align    Align of dst_ptr (in bits). Can be from 0 to 63
 * @param[in]  length   Number of bits to store. If length > (64 - align), only (64 - align) bits will be stored.
 *
 * @return  void
 */
static inline void own_store_8u_LE_tail_kernel(uint8_t *dst_ptr, __mmask64 data, int align, int length) {
    int      dst_align  = align & 0x7;
    uint64_t align_mask = (1u << dst_align) - 1; // align 1s, other are 0s
    uint64_t data_mask  = ((1u << length) - 1) << dst_align; // align 0s, then length 1s, other are 0s

    for (int i = 0; i < dst_align + length; i += 8) {
        uint8_t dst = *(dst_ptr + i / 8);
        dst &= (align_mask) & 0xFF;
        dst |= ((data << dst_align) & data_mask) & 0xFF;
        *dst_ptr = dst & 0xFF;
        align_mask >>= 8;
        data_mask >>= 8;
    }
}

/**
 * @brief Store (length) bits from 32-bit mask in dst (dst might not be aligned)
 *
 * @param[in]  dst_ptr  Pointer to the dst. First byte of dst contains 0 to 7 bits that must be not overwrited
 * @param[in]  data     32-bit element to store
 * @param[in]  align    Align of dst_ptr (in bits). Can be from 0 to 31
 * @param[in]  length   Number of bits to store. If length > (32 - align), only (32 - align) bits will be stored.
 *
 * @return  void
 */
static inline void own_store_16u_LE_tail_kernel(uint8_t *dst_ptr, __mmask32 data, int align, int length) {
    int      dst_align  = align & 0x7;
    uint32_t align_mask = (1u << dst_align) - 1; // align 1s, other are 0s
    uint32_t data_mask  = ((1u << length) - 1) << dst_align; // align 0s, then length 1s, other are 0s

    for (int i = 0; i < dst_align + length; i += 8) {
        uint8_t dst = *(dst_ptr + i / 8);
        dst &= (align_mask) & 0xFF;
        dst |= ((data << dst_align) & data_mask) & 0xFF;
        *dst_ptr = dst & 0xFF;
        align_mask >>= 8;
        data_mask >>= 8;
    }
}

/**
 * @brief Store (length) bits from 16-bit mask in dst (dst might not be aligned)
 *
 * @param[in]  dst_ptr  Pointer to the dst. First byte of dst contains 0 to 7 bits that must be not overwrited
 * @param[in]  data     16-bit element to store
 * @param[in]  align    Align of dst_ptr (in bits). Can be from 0 to 15
 * @param[in]  length   Number of bits to store. If length > (16 - align), only (16 - align) bits will be stored.
 *
 * @return  void
 */

static inline void own_store_32u_LE_tail_kernel(uint8_t *dst_ptr, __mmask16 data, int align, int length) {
    int      dst_align  = align & 0x7;
    uint16_t align_mask = (1u << dst_align) - 1; // align 1s, other are 0s
    uint16_t data_mask  = ((1u << length) - 1) << dst_align; // align 0s, then length 1s, other are 0s

    for (int i = 0; i < dst_align + length; i += 8) {
        uint8_t dst = *(dst_ptr + i / 8);
        dst &= (align_mask) & 0xFF;
        dst |= ((data << dst_align) & data_mask) & 0xFF;
        *dst_ptr = dst & 0xFF;
        align_mask >>= 8;
        data_mask >>= 8;
    }
}

// ------ LE indexes ------

#if 0
/**
 * @brief Store in dst 32u indexes of suitable elements based on 64-bit mask (dst is aligned on 32 bits)
 *
 * @param[in]  dst_ptr       pointer to the dst
 * @param[in]  data          64-bit mask which shows which elements are suitable
 * @param[in]  global_index  32u number that will be added to the indexes of elements (which are from 0 to 63)
 * @param[in]  base_indexes  __m512i register which contains 32u ordered elements: 0, 1, 2, 3, ..., 14, 15.
 *
 * @return  number of bytes it has taken in dst to store the indexes
 */
static inline int own_store_32uIdx_8u_LE_kernel(uint8_t *dst_ptr, __mmask64 data, uint32_t global_index, __m512i base_indexes)
{
    
    __mmask16 compress_mask, store_mask;
    uint8_t *store_dst_ptr;
    __m512i indexes;
    int num_indexes2store;
    int dst_offset;

    // First 16 bits
    {
        compress_mask = data & 0xFFFF;
        store_dst_ptr = dst_ptr;
        indexes = _mm512_maskz_add_epi8(data, base_indexes, _mm512_set1_epi32(global_index));
        num_indexes2store = __popcnt16(compress_mask);
        store_mask = (0x1 << num_indexes2store) - 1;
        dst_offset = num_indexes2store * sizeof(global_index);

        // Compress indexes of the suitable elements and skip others
        indexes = _mm512_castps_si512(_mm512_maskz_compress_ps(compress_mask, _mm512_castsi512_ps(indexes)));

        // We assume that dst is aligned on 32 bits
        _mm512_mask_storeu_epi32(store_dst_ptr, store_mask, indexes);
    }

    // Second 16 bits
    {
        compress_mask = (data >> 16) & 0xFFFF;
        store_dst_ptr = dst_ptr + dst_offset;
        indexes = _mm512_maskz_add_epi8(data, base_indexes, _mm512_set1_epi32(16));
        num_indexes2store = __popcnt16(compress_mask);
        store_mask = (0x1 << num_indexes2store) - 1;
        dst_offset += num_indexes2store * sizeof(global_index);

        indexes = _mm512_castps_si512(_mm512_maskz_compress_ps(compress_mask, _mm512_castsi512_ps(indexes)));

        _mm512_mask_storeu_epi32(store_dst_ptr, store_mask, indexes);
    }

    // Third 16 bits
    {
        compress_mask = (data >> 32) & 0xFFFF;
        store_dst_ptr = dst_ptr + dst_offset;
        indexes = _mm512_maskz_add_epi8(data, base_indexes, _mm512_set1_epi32(16));
        num_indexes2store = __popcnt16(compress_mask);
        store_mask = (0x1 << num_indexes2store) - 1;
        dst_offset += num_indexes2store * sizeof(global_index);

        indexes = _mm512_castps_si512(_mm512_maskz_compress_ps(compress_mask, _mm512_castsi512_ps(indexes)));

        _mm512_mask_storeu_epi32(store_dst_ptr, store_mask, indexes);
    }

    // Fourth 16 bits
    {
        compress_mask = (data >> 48) & 0xFFFF;
        store_dst_ptr = dst_ptr + dst_offset;
        indexes = _mm512_maskz_add_epi8(data, base_indexes, _mm512_set1_epi32(16));
        num_indexes2store = __popcnt16(compress_mask);
        store_mask = (0x1 << num_indexes2store) - 1;
        dst_offset += num_indexes2store * sizeof(global_index);

        indexes = _mm512_castps_si512(_mm512_maskz_compress_ps(compress_mask, _mm512_castsi512_ps(indexes)));

        _mm512_mask_storeu_epi32(store_dst_ptr, store_mask, indexes);
    }

    return dst_offset;
}
#endif
#endif // OWN_STORE_INTRIN_H
