/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of all functions for scan analytics operation
 * @date 08/02/2020
 *
 * @details Function list:
 *          - @ref k0_qplc_scan_lt_8u
 *          - @ref k0_qplc_scan_lt_16u8u
 *          - @ref k0_qplc_scan_lt_32u8u
 *          - @ref k0_qplc_scan_le_8u
 *          - @ref k0_qplc_scan_le_16u8u
 *          - @ref k0_qplc_scan_le_32u8u
 *          - @ref k0_qplc_scan_gt_8u
 *          - @ref k0_qplc_scan_gt_16u8u
 *          - @ref k0_qplc_scan_gt_32u8u
 *          - @ref k0_qplc_scan_ge_8u
 *          - @ref k0_qplc_scan_ge_16u8u
 *          - @ref k0_qplc_scan_ge_32u8u
 *          - @ref k0_qplc_scan_eq_8u
 *          - @ref k0_qplc_scan_eq_16u8u
 *          - @ref k0_qplc_scan_eq_32u8u
 *          - @ref k0_qplc_scan_ne_8u
 *          - @ref k0_qplc_scan_ne_16u8u
 *          - @ref k0_qplc_scan_ne_32u8u
 *          - @ref k0_qplc_scan_range_8u
 *          - @ref k0_qplc_scan_range_16u8u
 *          - @ref k0_qplc_scan_range_32u8u
 *          - @ref k0_qplc_scan_not_range_8u
 *          - @ref k0_qplc_scan_not_range_16u8u
 *          - @ref k0_qplc_scan_not_range_32u8u
 *
 */

#ifndef SCAN_OPT_H
#define SCAN_OPT_H

#include "own_qplc_defs.h"
#include "own_scan_intrin.h"

OWN_OPT_FUN(void, k0_qplc_scan_lt_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_LT_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] < low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_lt_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;
    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_LT_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] < low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_lt_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_LT_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] < low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_eq_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {

    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_EQ_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_eq_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_EQ_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] == low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_eq_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_EQ_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] == low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ne_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_NE_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] == low_value) ? 0u : 1u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ne_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_NE_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] == low_value) ? 0u : 1u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ne_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_NE_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] == low_value) ? 0u : 1u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_le_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {

    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_LE_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] <= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_le_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_LE_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] <= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_le_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_LE_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] <= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_gt_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_GT_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] > low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_gt_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_GT_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] > low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_gt_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_GT_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] > low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ge_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_value = _mm512_set1_epi8(low_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_GE_8u_kernel(srcmm, broadcasted_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_ptr[idx] >= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ge_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_value = _mm512_set1_epi16(low_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_GE_16u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_16_ptr[idx] >= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_ge_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value)) {
    uint32_t idx;

    uint32_t length16 = length & (-16);
    uint32_t tail = length - length16;
    __m512i  broadcasted_value = _mm512_set1_epi32(low_value);

    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_GE_32u_kernel(srcmm, broadcasted_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = (src_32_ptr[idx] >= low_value) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_range_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_low_value = _mm512_set1_epi8(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi8(high_value);
    
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_REQ_8u_kernel(srcmm, broadcasted_low_value, broadcasted_high_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_range_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_low_value = _mm512_set1_epi16(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi16(high_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_REQ_16u_kernel(srcmm,
            broadcasted_low_value,
            broadcasted_high_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_16_ptr[idx] >= low_value) && (src_16_ptr[idx] <= high_value)) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_range_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t idx;

    uint32_t      length16 = length & (-16);
    uint32_t      tail = length - length16;
    __m512i       broadcasted_low_value = _mm512_set1_epi32(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi32(high_value);
    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_REQ_32u_kernel(srcmm,
            broadcasted_low_value,
            broadcasted_high_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_32_ptr[idx] >= low_value) && (src_32_ptr[idx] <= high_value)) ? 1u : 0u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_not_range_8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t      length64 = length & (-64);
    uint32_t      tail = length - length64;
    __m512i       broadcasted_low_value = _mm512_set1_epi8(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi8(high_value);
    for (uint32_t i = 0u; i < length64; i += 64u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = own_scan_RNE_8u_kernel(srcmm, broadcasted_low_value, broadcasted_high_value);
        __m512i dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_storeu_si512(dst_ptr, dstmm);

        src_ptr += 64u;
        dst_ptr += 64u;
    }

    uint32_t idx;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_ptr[idx] >= low_value) && (src_ptr[idx] <= high_value)) ? 0u : 1u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_not_range_16u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t idx;

    uint32_t      length32 = length & (-32);
    uint32_t      tail = length - length32;
    __m512i       broadcasted_low_value = _mm512_set1_epi16(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi16(high_value);
    for (uint32_t i = 0u; i < length32; i += 32u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_RNE_16u_kernel(srcmm,
            broadcasted_low_value,
            broadcasted_high_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x00000000FFFFFFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 32u;
    }

    uint16_t *src_16_ptr = (uint16_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_16_ptr[idx] >= low_value) && (src_16_ptr[idx] <= high_value)) ? 0u : 1u;
    }
}

OWN_OPT_FUN(void, k0_qplc_scan_not_range_32u8u, (const uint8_t *src_ptr, uint8_t *dst_ptr,
    uint32_t length,
    uint32_t low_value,
    uint32_t high_value)) {
    uint32_t idx;

    uint32_t      length16 = length & (-16);
    uint32_t      tail = length - length16;
    __m512i       broadcasted_low_value = _mm512_set1_epi32(low_value);
    __m512i       broadcasted_high_value = _mm512_set1_epi32(high_value);
    for (uint32_t i = 0u; i < length16; i += 16u) {
        __m512i   srcmm = _mm512_loadu_si512(src_ptr);
        __mmask64 scan_mask = (__mmask64)own_scan_RNE_32u_kernel(srcmm,
            broadcasted_low_value,
            broadcasted_high_value);
        __m512i   dstmm = _mm512_movm_epi8(scan_mask);
        dstmm = _mm512_abs_epi8(dstmm);
        _mm512_mask_storeu_epi8(dst_ptr, 0x000000000000FFFF, dstmm);

        src_ptr += 64u;
        dst_ptr += 16u;
    }

    uint32_t *src_32_ptr = (uint32_t *)src_ptr;
    for (idx = 0u; idx < tail; idx++) {
        dst_ptr[idx] = ((src_32_ptr[idx] >= low_value) && (src_32_ptr[idx] <= high_value)) ? 0u : 1u;
    }
}

#endif // SCAN_OPT_H
