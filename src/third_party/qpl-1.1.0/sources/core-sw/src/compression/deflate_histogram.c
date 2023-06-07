/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file own_deflate_histogram.c
 * @brief contains implementation of @link own_deflate_histogram @endlink service functions
 */

/* ------ Includes ------ */

#include "stdint.h"
#include "deflate_histogram.h"
#include "own_qplc_defs.h"

#define OWN_BUILD_MASK(number_of_bits) ((1u << (number_of_bits)) - 1u) /**< Builds bit mask for given number of bits */

/* ------ Internal functions implementation ------ */

#if PLATFORM < K0

static inline uint32_t own_get_match_length_table_index(const uint32_t match_length) {
    // Based on tables on page 11 in RFC 1951
    if (match_length < 11) {
        return 257 + match_length - 3;
    } else if (match_length < 19) {
        return 261 + (match_length - 3) / 2;
    } else if (match_length < 35) {
        return 265 + (match_length - 3) / 4;
    } else if (match_length < 67) {
        return 269 + (match_length - 3) / 8;
    } else if (match_length < 131) {
        return 273 + (match_length - 3) / 16;
    } else if (match_length < 258) {
        return 277 + (match_length - 3) / 32;
    } else {
        return 285;
    }
}

static inline uint32_t own_get_offset_table_index(const uint32_t offset) {
    if (offset <= 2) {
        return offset - 1;
    } else if (offset <= 4) {
        return 0 + (offset - 1) / 1;
    } else if (offset <= 8) {
        return 2 + (offset - 1) / 2;
    } else if (offset <= 16) {
        return 4 + (offset - 1) / 4;
    } else if (offset <= 32) {
        return 6 + (offset - 1) / 8;
    } else if (offset <= 64) {
        return 8 + (offset - 1) / 16;
    } else if (offset <= 128) {
        return 10 + (offset - 1) / 32;
    } else if (offset <= 256) {
        return 12 + (offset - 1) / 64;
    } else if (offset <= 512) {
        return 14 + (offset - 1) / 128;
    } else if (offset <= 1024) {
        return 16 + (offset - 1) / 256;
    } else if (offset <= 2048) {
        return 18 + (offset - 1) / 512;
    } else if (offset <= 4096) {
        return 20 + (offset - 1) / 1024;
    } else if (offset <= 8192) {
        return 22 + (offset - 1) / 2048;
    } else if (offset <= 16384) {
        return 24 + (offset - 1) / 4096;
    } else if (offset <= 32768) {
        return 26 + (offset - 1) / 8192;
    } else {
        // ~0 is an invalid distance code
        return ~0u;
    }
}

#endif

/* ------ Own functions implementation ------ */

OWN_QPLC_FUN(void, deflate_histogram_reset, (deflate_histogram_t *const histogram_ptr)) {
    // TODO remove constant mask and table size
    // TODO change number of attempts
    // TODO make the logic of an assignment dependent on compression level

    // Simple assignment
    histogram_ptr->table.hash_mask  = OWN_BUILD_MASK(12u);
    histogram_ptr->table.attempts   = 4096u;
    histogram_ptr->table.good_match = 32u;
    histogram_ptr->table.nice_match = 258u;
    histogram_ptr->table.lazy_match = 258u;

    CALL_CORE_FUN(deflate_hash_table_reset(&histogram_ptr->table));

    // Setting end of block
    histogram_ptr->literals_matches[256u] = 1u;
}


#if PLATFORM < K0

void deflate_histogram_update_match(deflate_histogram_t *const histogram_ptr, const deflate_match_t match) {
    // Histogram update
    histogram_ptr->literals_matches[own_get_match_length_table_index(match.length)]++;
    histogram_ptr->offsets[own_get_offset_table_index(match.offset)]++;
}

void deflate_histogram_get_statistics(const deflate_histogram_t *deflate_histogram_ptr,
                                      uint32_t *literal_length_histogram_ptr,
                                      uint32_t *offsets_histogram_ptr) {
    for (uint32_t i = 0u; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        literal_length_histogram_ptr[i] = deflate_histogram_ptr->literals_matches[i];
    }

    for (uint32_t i = 0u; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        offsets_histogram_ptr[i] = deflate_histogram_ptr->offsets[i];
    }
}

void deflate_histogram_set_statistics(deflate_histogram_t *deflate_histogram_ptr,
                                      const uint32_t *literal_length_histogram_ptr,
                                      const uint32_t *offsets_histogram_ptr) {
    for (uint32_t i = 0u; i < QPLC_DEFLATE_LL_TABLE_SIZE; i++) {
        deflate_histogram_ptr->literals_matches[i] = literal_length_histogram_ptr[i];
    }

    for (uint32_t i = 0u; i < QPLC_DEFLATE_D_TABLE_SIZE; i++) {
        deflate_histogram_ptr->offsets[i] = offsets_histogram_ptr[i];
    }
}

#endif
