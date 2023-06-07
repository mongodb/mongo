/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "stdint.h"

#include "deflate_hash_table.h"
#include "qplc_checksum.h"
#include "crc.h"

#include "deflate_slow_utils.h"

#include "igzip_lib.h"
#include "encode_df.h"

#if PLATFORM >= K0

#include "opt/qplc_deflate_slow_utils_k0.h"

#endif

#if PLATFORM < K0

static inline uint32_t bsr(uint32_t val) {
    uint32_t msb = 0;
    for (msb = 0; val > 0; val >>= 1) {
        msb++;
    }
    return msb;
}

static inline uint32_t count_significant_bits(uint32_t value) {
    // Variables
    uint32_t significant_bits = 0;

    // Main cycle
    while (value > 0) {
        significant_bits++;
        value >>= 1u;
    }

    return significant_bits;
}

static inline void compute_offset_code(const struct isal_hufftables *huffman_table_ptr,
                                       uint16_t offset,
                                       uint64_t *const code_ptr,
                                       uint32_t *const code_length_ptr) {
    // Variables
    uint32_t significant_bits;
    uint32_t number_of_extra_bits;
    uint32_t extra_bits;
    uint32_t symbol;
    uint32_t length;
    uint32_t code;

    offset -= 1u;
    significant_bits = count_significant_bits(offset);

    number_of_extra_bits = significant_bits - 2u;
    extra_bits           = offset & ((1u << number_of_extra_bits) - 1u);
    offset >>= number_of_extra_bits;
    symbol               = offset + 2 * number_of_extra_bits;

    // Extracting information from table
    code   = huffman_table_ptr->dcodes[symbol];
    length = huffman_table_ptr->dcodes_sizes[symbol];

    // Return of the calculated results
    *code_ptr        = code | (extra_bits << length);
    *code_length_ptr = length + number_of_extra_bits;
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
#if defined(DEFLATE_WINDOW_SIZE_ABOVE_4K)
        } else if (offset <= 8192) {
            return 22 + (offset - 1) / 2048;
        } else if (offset <= 16384) {
            return 24 + (offset - 1) / 4096;
        } else if (offset <= 32768) {
            return 26 + (offset - 1) / 8192;
#endif
    } else {
        // ~0 is an invalid distance code
        return ~0u;
    }
}

static void compute_distance_icf_code(uint32_t distance, uint32_t *code, uint32_t *extra_bits) {
    uint32_t msb;
    uint32_t num_extra_bits;

    distance -= 1;
    msb = bsr(distance);
    assert(msb >= 1);
    num_extra_bits = msb - 2;
    *extra_bits = distance & ((1 << num_extra_bits) - 1);
    distance >>= num_extra_bits;
    *code = distance + 2 * num_extra_bits;
    assert(*code < 30);
}

uint32_t update_missed_literals(uint8_t *current_ptr,
                                const uint8_t *upper_bound_ptr,
                                const uint8_t *lower_bound_ptr,
                                deflate_hash_table_t *hash_table_ptr) {
    uint32_t bytes_processed = 0;

    while (current_ptr < upper_bound_ptr) {
        // Variables
        const uint32_t hash_value = crc32_gzip_refl(0u,
                                                    current_ptr,
                                                    OWN_BYTES_FOR_HASH_CALCULATION) & hash_table_ptr->hash_mask;

        // Updating hash table
        own_deflate_hash_table_update(hash_table_ptr,
                                      (uint32_t) (current_ptr - lower_bound_ptr),
                                      hash_value);

        // End of iteration
        current_ptr++;
        bytes_processed++;
    }

    return bytes_processed;
}

void get_distance_icf_code(uint32_t distance, uint32_t *code, uint32_t *extra_bits) {
    assert(distance >= 1);
    assert(distance <= 32768);
    if (distance <= 2) {
        *code       = distance - 1;
        *extra_bits = 0;
    } else {
        compute_distance_icf_code(distance, code, extra_bits);
    }
}

void write_deflate_icf(struct deflate_icf *icf,
                       uint32_t lit_len,
                       uint32_t lit_dist,
                       uint32_t extra_bits) {
    *(uint32_t *) icf = lit_len |
                        (lit_dist << LIT_LEN_BIT_COUNT) |
                        (extra_bits << (LIT_LEN_BIT_COUNT + DIST_LIT_BIT_COUNT));
}

void update_histogram_for_literal(isal_mod_hist *const histogram_ptr, const uint8_t literal) {
    histogram_ptr->ll_hist[literal]++;
}

void update_histogram_for_match(isal_mod_hist *const histogram_ptr, const deflate_match_t match) {
    histogram_ptr->ll_hist[match.length + LEN_OFFSET]++;
    histogram_ptr->d_hist[own_get_offset_table_index(match.offset)]++;
}

void get_match_length_code(const struct isal_hufftables *const huffman_table_ptr,
                           const uint32_t match_length,
                           uint64_t *const code_ptr,
                           uint32_t *const code_length_ptr) {
    const uint64_t match_length_info = huffman_table_ptr->len_table[match_length - 3u];

    *code_ptr        = match_length_info >> 5u;
    *code_length_ptr = match_length_info & 0x1Fu;
}

void get_offset_code(const struct isal_hufftables *const huffman_table_ptr,
                     uint32_t offset,
                     uint64_t *const code_ptr,
                     uint32_t *const code_length_ptr) {
    if (offset <= IGZIP_DIST_TABLE_SIZE) {
        const uint64_t offset_info = huffman_table_ptr->dist_table[offset - 1];

        *code_ptr        = offset_info >> 5u;
        *code_length_ptr = offset_info & 0x1Fu;
    } else {
        compute_offset_code(huffman_table_ptr, offset, code_ptr, code_length_ptr);
    }
}

void get_literal_code(const struct isal_hufftables *const huffman_table_ptr,
                      const uint32_t literal,
                      uint64_t *const code_ptr,
                      uint32_t *const code_length_ptr) {
    *code_ptr        = huffman_table_ptr->lit_table[literal];
    *code_length_ptr = huffman_table_ptr->lit_table_sizes[literal];
}

#endif

OWN_QPLC_FUN(void, setup_dictionary, (uint8_t * dictionary_ptr,
        uint32_t dictionary_size,
        deflate_hash_table_t * hash_table_ptr)) {
#if PLATFORM >= K0
    CALL_OPT_FUNCTION(k0_setup_dictionary)(dictionary_ptr, dictionary_size, hash_table_ptr);
    return;
#else

    uint8_t *current_ptr = dictionary_ptr;

    for (uint32_t index = 0; index < dictionary_size; index++) {
        // Variables

        uint32_t hash_value = 0u;
        hash_value = crc32_gzip_refl(0u,
                                     current_ptr,
                                     OWN_BYTES_FOR_HASH_CALCULATION) & hash_table_ptr->hash_mask;
        // Updating hash table
        own_deflate_hash_table_update(hash_table_ptr,
                                      index - dictionary_size,
                                      hash_value);

        // End of iteration
        current_ptr++;
    }

#endif
}
