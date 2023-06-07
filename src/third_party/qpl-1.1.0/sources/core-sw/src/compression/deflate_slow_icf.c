/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "stdbool.h"

#include "igzip_lib.h"
#include "encode_df.h"

#include "own_qplc_defs.h"

#include "deflate_defs.h"
#include "qplc_checksum.h"
#include "crc.h"

#include "deflate_slow_utils.h"
#include "deflate_slow_matcher.h"
#include "deflate_defs.h"

typedef struct deflate_icf_stream deflate_icf_stream;

#if PLATFORM >= K0

#include "opt/qplc_deflate_slow_icf_k0.h"

#endif

#if PLATFORM < K0

uint32_t process_literals(uint8_t *current_ptr,
                          const uint8_t *upper_bound_ptr,
                          const uint8_t *lower_bound_ptr,
                          deflate_hash_table_t *hash_table_ptr,
                          isal_mod_hist *histogram_ptr,
                          deflate_icf_stream *icf_stream_ptr,
                          bool safe) {
    uint32_t bytes_processed = 0;

    // Main cycle
    if (true == safe) {
        while (current_ptr < upper_bound_ptr && icf_stream_ptr->next_ptr < icf_stream_ptr->end_ptr - 1) {
            // Variables
            const uint32_t bytes_for_hash = QPL_MIN(OWN_BYTES_FOR_HASH_CALCULATION,
                                                    (uint32_t) (upper_bound_ptr - current_ptr));

            const uint32_t hash_value = crc32_gzip_refl(0u, current_ptr, bytes_for_hash) &
                                        hash_table_ptr->hash_mask;

            // Updating histogram
            update_histogram_for_literal(histogram_ptr, *current_ptr);

            // Updating hash table
            own_deflate_hash_table_update(hash_table_ptr,
                                          (uint32_t) (current_ptr - lower_bound_ptr),
                                          hash_value);

            // Writing icf
            write_deflate_icf(icf_stream_ptr->next_ptr, *current_ptr, LITERAL_DISTANCE_IN_ICF, 0);
            icf_stream_ptr->next_ptr++;

            // End of iteration
            current_ptr++;
            bytes_processed++;
        }
    } else {
        while (current_ptr < upper_bound_ptr && icf_stream_ptr->next_ptr < icf_stream_ptr->end_ptr - 1) {
            // Variables
            const uint32_t hash_value = crc32_gzip_refl(0u, current_ptr, OWN_BYTES_FOR_HASH_CALCULATION) &
                                        hash_table_ptr->hash_mask;

            // Updating histogram
            update_histogram_for_literal(histogram_ptr, *current_ptr);

            // Updating hash table
            own_deflate_hash_table_update(hash_table_ptr,
                                          (uint32_t) (current_ptr - lower_bound_ptr),
                                          hash_value);

            // Writing icf
            write_deflate_icf(icf_stream_ptr->next_ptr, *current_ptr, LITERAL_DISTANCE_IN_ICF, 0);
            icf_stream_ptr->next_ptr++;

            // End of iteration
            current_ptr++;
            bytes_processed++;
        }
    }

    return bytes_processed;
}

uint32_t process_match(uint8_t *current_ptr,
                       const uint8_t *lower_bound_ptr,
                       deflate_hash_table_t *hash_table_ptr,
                       isal_mod_hist *histogram_ptr,
                       const deflate_match_t match,
                       deflate_icf_stream *icf_stream_ptr) {
    uint32_t bytes_processed       = 0;
    uint32_t total_bytes_processed = 0;

    // Processing missed literals
    bytes_processed = process_literals(current_ptr,
                                       match.match_source_ptr,
                                       lower_bound_ptr,
                                       hash_table_ptr,
                                       histogram_ptr,
                                       icf_stream_ptr,
                                       false);

    current_ptr += bytes_processed;
    total_bytes_processed += bytes_processed;

    if (icf_stream_ptr->next_ptr < icf_stream_ptr->end_ptr - 1) {
        // Variables
        const uint8_t *const next_current_ptr = current_ptr + match.length;

        // Updating histogram for match
        update_histogram_for_match(histogram_ptr, match);

        // Writing icf
        uint32_t distance   = 0;
        uint32_t extra_bits = 0;
        get_distance_icf_code(match.offset, &distance, &extra_bits);
        write_deflate_icf(icf_stream_ptr->next_ptr, match.length + LEN_OFFSET, distance, extra_bits);
        icf_stream_ptr->next_ptr++;

        // Updating hash table for each missing literals
        bytes_processed = update_missed_literals(current_ptr,
                                                 next_current_ptr,
                                                 lower_bound_ptr,
                                                 hash_table_ptr);

        total_bytes_processed += bytes_processed;
    }
    return total_bytes_processed;
}

#endif

OWN_QPLC_FUN(uint32_t, slow_deflate_icf_body, (uint8_t * current_ptr,
        const uint8_t        *const lower_bound_ptr,
        const uint8_t        *const upper_bound_ptr,
        deflate_hash_table_t *hash_table_ptr,
        isal_mod_hist        *histogram_ptr,
        deflate_icf_stream   *icf_stream_ptr)) {

#if PLATFORM >= K0
    return CALL_OPT_FUNCTION(k0_slow_deflate_icf_body)(current_ptr,
        lower_bound_ptr, upper_bound_ptr, hash_table_ptr,
        histogram_ptr, icf_stream_ptr);
#else
    uint32_t total_bytes_processed = 0;
    uint32_t bytes_processed       = 0;

    // Main cycle
    while (current_ptr < (upper_bound_ptr - OWN_MINIMAL_MATCH_LENGTH) &&
           icf_stream_ptr->next_ptr < icf_stream_ptr->end_ptr - 1) {
        // Variables
        const deflate_match_t longest_match = get_lazy_best_match(hash_table_ptr,
                                                                  lower_bound_ptr,
                                                                  current_ptr,
                                                                  upper_bound_ptr - OWN_MINIMAL_MATCH_LENGTH);

        if (longest_match.length >= OWN_MINIMAL_MATCH_LENGTH) {
            bytes_processed = process_match(current_ptr,
                                            lower_bound_ptr,
                                            hash_table_ptr,
                                            histogram_ptr,
                                            longest_match,
                                            icf_stream_ptr);
        } else {
            bytes_processed = process_literals(current_ptr,
                                               current_ptr + 1,
                                               lower_bound_ptr,
                                               hash_table_ptr,
                                               histogram_ptr,
                                               icf_stream_ptr,
                                               false);
        }
        current_ptr += bytes_processed;
        total_bytes_processed += bytes_processed;
    }

    // Processing last bytes
    total_bytes_processed += process_literals(current_ptr,
                                              upper_bound_ptr,
                                              lower_bound_ptr,
                                              hash_table_ptr,
                                              histogram_ptr,
                                              icf_stream_ptr,
                                              true);

    return total_bytes_processed;
#endif
}
