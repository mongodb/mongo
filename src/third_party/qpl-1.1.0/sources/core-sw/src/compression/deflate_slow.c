/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "stdbool.h"

#include "igzip_lib.h"

#include "own_qplc_defs.h"

#include "deflate_hash_table.h"
#include "qplc_checksum.h"
#include "crc.h"

#include "deflate_slow_utils.h"
#include "deflate_slow_matcher.h"
#include "deflate_defs.h"


#if PLATFORM >= K0

#include "opt/qplc_deflate_slow_k0.h"

#endif

#if PLATFORM < K0
uint32_t encode_literals(uint8_t *current_ptr,
                         const uint8_t *upper_bound_ptr,
                         const uint8_t *lower_bound_ptr,
                         deflate_hash_table_t *hash_table_ptr,
                         struct isal_hufftables *huffman_table_ptr,
                         struct BitBuf2 *bit_writer_ptr,
                         bool safe) {
    uint32_t bytes_processed = 0;

    if (true == safe) {
        while (current_ptr < upper_bound_ptr) {
            // Variables
            const uint32_t bytes_for_hash = QPL_MIN(OWN_BYTES_FOR_HASH_CALCULATION,
                                                    (uint32_t) (upper_bound_ptr - current_ptr));
            const uint32_t hash_value     = crc32_gzip_refl(0u, current_ptr, bytes_for_hash) &
                                            hash_table_ptr->hash_mask;

            uint64_t literal_code        = 0u;
            uint32_t literal_code_length = 0u;

            // Getting information from Huffman table
            get_literal_code(huffman_table_ptr,
                             *current_ptr,
                             &literal_code,
                             &literal_code_length);

            // Writing to the output
            write_bits(bit_writer_ptr, literal_code, literal_code_length);

            // Updating hash-table
            own_deflate_hash_table_update(hash_table_ptr,
                                          (uint32_t) (current_ptr - lower_bound_ptr),
                                          hash_value);

            // Going to next iteration
            current_ptr++;
            bytes_processed++;
        }
    } else {
        while (current_ptr < upper_bound_ptr) {
            // Variables
            const uint32_t hash_value = crc32_gzip_refl(0u, current_ptr, OWN_BYTES_FOR_HASH_CALCULATION) &
                                        hash_table_ptr->hash_mask;

            uint64_t literal_code        = 0u;
            uint32_t literal_code_length = 0u;

            // Getting information from Huffman table
            get_literal_code(huffman_table_ptr,
                             *current_ptr,
                             &literal_code,
                             &literal_code_length);

            // Writing to the output
            write_bits(bit_writer_ptr, literal_code, literal_code_length);

            // Updating hash-table
            own_deflate_hash_table_update(hash_table_ptr,
                                          (uint32_t) (current_ptr - lower_bound_ptr),
                                          hash_value);

            // Going to next iteration
            current_ptr++;
            bytes_processed++;
        }
    }
    return bytes_processed;
}

uint32_t encode_match(uint8_t *current_ptr,
                      const uint8_t *lower_bound_ptr,
                      deflate_hash_table_t *hash_table_ptr,
                      const deflate_match_t match,
                      struct isal_hufftables *huffman_table_ptr,
                      struct BitBuf2 *bit_writer_ptr) {
    uint32_t bytes_processed       = 0;
    uint32_t total_bytes_processed = 0;

    // Processing missed literals
    bytes_processed = encode_literals(current_ptr,
                                      match.match_source_ptr,
                                      lower_bound_ptr,
                                      hash_table_ptr,
                                      huffman_table_ptr,
                                      bit_writer_ptr,
                                      false);

    current_ptr           += bytes_processed;
    total_bytes_processed += bytes_processed;

    // Variables
    uint64_t match_code         = 0u;
    uint64_t offset_code        = 0u;
    uint32_t match_code_length  = 0u;
    uint32_t offset_code_length = 0u;

    // Getting information from Huffman table
    get_match_length_code(huffman_table_ptr, match.length, &match_code, &match_code_length);
    get_offset_code(huffman_table_ptr, match.offset, &offset_code, &offset_code_length);

    // Combining two codes
    match_code |= offset_code << match_code_length;

    // Writing to the output
    write_bits(bit_writer_ptr, match_code, match_code_length + offset_code_length);

    // Switching to next position
    bytes_processed = update_missed_literals(current_ptr, current_ptr + match.length, lower_bound_ptr, hash_table_ptr);

    total_bytes_processed += bytes_processed;

    return total_bytes_processed;
}

#endif


OWN_QPLC_FUN(uint32_t,slow_deflate_body,(uint8_t *current_ptr,
                           const uint8_t *const lower_bound_ptr,
                           const uint8_t *const upper_bound_ptr,
                           deflate_hash_table_t   *hash_table_ptr,
                           struct isal_hufftables *huffman_tables_ptr,
                           struct BitBuf2 *bit_writer_ptr)) {

#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_slow_deflate_body)(current_ptr,
        lower_bound_ptr, upper_bound_ptr, hash_table_ptr,
        huffman_tables_ptr, bit_writer_ptr);
#else

    uint32_t     total_bytes_processed = 0;
    uint32_t     bytes_processed       = 0;

    // Main cycle
    while (current_ptr < (upper_bound_ptr - OWN_MINIMAL_MATCH_LENGTH) && !is_full(bit_writer_ptr)) {
        // Variables 
        const deflate_match_t longest_match = get_lazy_best_match(hash_table_ptr,
                                                                  lower_bound_ptr,
                                                                  current_ptr,
                                                                  upper_bound_ptr - OWN_MINIMAL_MATCH_LENGTH);

        if (longest_match.length >= OWN_MINIMAL_MATCH_LENGTH) {
            bytes_processed = encode_match(current_ptr,
                                           lower_bound_ptr,
                                           hash_table_ptr,
                                           longest_match,
                                           huffman_tables_ptr,
                                           bit_writer_ptr);
        } else {
            bytes_processed = encode_literals(current_ptr,
                                              current_ptr + 1,
                                              lower_bound_ptr,
                                              hash_table_ptr,
                                              huffman_tables_ptr,
                                              bit_writer_ptr,
                                              false);
        }
        current_ptr           += bytes_processed;
        total_bytes_processed += bytes_processed;
    }

    // Processing last bytes
    if (!is_full(bit_writer_ptr)) {
        total_bytes_processed += encode_literals(current_ptr,
                                                 upper_bound_ptr,
                                                 lower_bound_ptr,
                                                 hash_table_ptr,
                                                 huffman_tables_ptr,
                                                 bit_writer_ptr,
                                                 true);
    }

    return total_bytes_processed;
#endif
}
