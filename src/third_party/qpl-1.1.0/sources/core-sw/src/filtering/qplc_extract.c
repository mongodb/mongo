/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @brief Contains implementation of all functions for extract analytics operation
 * @date 07/20/2020
 *
 * @details Function list:
 *          - @ref qplc_extract_8u_i
 *          - @ref qplc_extract_16u_i
 *          - @ref qplc_extract_32u_i
 *          - @ref qplc_extract_8u
 *          - @ref qplc_extract_16u
 *          - @ref qplc_extract_32u
 */

#include "own_qplc_defs.h"
#include "qplc_memop.h"

OWN_QPLC_FUN(uint32_t, qplc_extract_8u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t * index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t start;
    uint32_t stop;
    uint8_t  *src_ptr = (uint8_t *) src_dst_ptr;
    uint8_t  *dst_ptr = (uint8_t *) src_dst_ptr;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    if (0u != start) {
        src_ptr += start;
        CALL_CORE_FUN(qplc_move_8u)(src_ptr, dst_ptr, (stop - start));
    }
    *index_ptr += length;
    return (stop - start);
}

OWN_QPLC_FUN(uint32_t, qplc_extract_16u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t * index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t start;
    uint32_t stop;
    uint16_t *src_ptr = (uint16_t *) src_dst_ptr;
    uint16_t *dst_ptr = (uint16_t *) src_dst_ptr;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    if (0u != start) {
        src_ptr += start;
        CALL_CORE_FUN(qplc_move_16u)(src_ptr, dst_ptr, (stop - start));
    }
    *index_ptr += length;
    return (stop - start);
}

OWN_QPLC_FUN(uint32_t, qplc_extract_32u_i, (uint8_t * src_dst_ptr,
        uint32_t length,
        uint32_t * index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t start;
    uint32_t stop;
    uint32_t *src_ptr = (uint32_t *) src_dst_ptr;
    uint32_t *dst_ptr = (uint32_t *) src_dst_ptr;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    if (0u != start) {
        src_ptr += start;
        CALL_CORE_FUN(qplc_move_32u)(src_ptr, dst_ptr, (stop - start));
    }
    *index_ptr += length;
    return (stop - start);
}

/******** out-of-place scan functions ********/

OWN_QPLC_FUN(uint32_t, qplc_extract_8u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t start;
    uint32_t stop;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    src_ptr += start;
    CALL_CORE_FUN(qplc_move_8u)(src_ptr, dst_ptr, (stop - start));
    *index_ptr += length;
    return (stop - start);
}

OWN_QPLC_FUN(uint32_t, qplc_extract_16u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t       start;
    uint32_t       stop;
    const uint16_t *src_16u_ptr = (uint16_t *) src_ptr;
    uint16_t       *dst_16u_ptr = (uint16_t *) dst_ptr;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    src_16u_ptr += start;
    CALL_CORE_FUN(qplc_move_16u)(src_16u_ptr, dst_16u_ptr, (stop - start));
    *index_ptr += length;
    return (stop - start);
}

OWN_QPLC_FUN(uint32_t, qplc_extract_32u, (const uint8_t *src_ptr,
        uint8_t *dst_ptr,
        uint32_t length,
        uint32_t *index_ptr,
        uint32_t low_value,
        uint32_t high_value)) {
    uint32_t       start;
    uint32_t       stop;
    const uint32_t *src_32u_ptr = (uint32_t *) src_ptr;
    uint32_t       *dst_32u_ptr = (uint32_t *) dst_ptr;

    if ((*index_ptr + length) < low_value) {
        *index_ptr += length;
        return 0u;
    }
    if (*index_ptr > high_value) {
        return 0u;
    }

    start = (*index_ptr < low_value) ? (low_value - *index_ptr) : 0u;
    stop  = ((*index_ptr + length) > high_value) ? (high_value + 1u - *index_ptr) : length;

    src_32u_ptr += start;
    CALL_CORE_FUN(qplc_move_32u)(src_32u_ptr, dst_32u_ptr, (stop - start));
    *index_ptr += length;
    return (stop - start);
}
