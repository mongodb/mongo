/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @file own_deflate_job.c
 * @brief contains implementation of @link own_deflate_job @endlink service functions
 */

/* ------ Includes ------ */

#include "deflate_defs.h"
#include "bit_writer.h"
#include "deflate_hash_table.h"
#include "deflate_histogram.h"
#include "own_deflate_job.h"
#include "qplc_compression_consts.h"

#include "crc.h"

#define OWN_CRC32(buf, len, init_crc, ...) ((0, ##__VA_ARGS__) ? crc32_iscsi(buf, len, init_crc) : crc32_gzip_refl(init_crc, buf, len))

/* ------ Own functions implementation ------ */

void own_deflate_job_switch_to_next(own_deflate_job *const job_ptr, const uint32_t range) {
    job_ptr->current_ptr += range;
    job_ptr->bytes_read += range;
}

static inline void own_deflate_job_update_missed_literals(own_deflate_job *const job_ptr,
                                                          const uint8_t *upper_bound_ptr) {
    while (job_ptr->current_ptr < upper_bound_ptr) {
        // Variables
        const uint32_t hash_value = OWN_CRC32(job_ptr->current_ptr,
                                              QPLC_DEFLATE_BYTES_FOR_HASH_CALCULATION,
                                              0u) & job_ptr->histogram_ptr->table.hash_mask;

        // Updating hash table
        own_deflate_hash_table_update(&job_ptr->histogram_ptr->table,
                                      (uint32_t) (job_ptr->current_ptr - job_ptr->lower_bound_ptr),
                                      hash_value);

        // End of iteration
        own_deflate_job_switch_to_next(job_ptr, 1u);
    }
}

/* ------ Own functions implementation ------ */

void own_deflate_job_reset_input(own_deflate_job *const job_ptr,
                                 const uint8_t *const source_ptr,
                                 const uint32_t input_bytes) {
    job_ptr->lower_bound_ptr = (uint8_t *) (source_ptr);
    job_ptr->current_ptr     = (uint8_t *) (source_ptr);
    job_ptr->upper_bound_ptr = (uint8_t *) (source_ptr + input_bytes);
    job_ptr->input_bytes     = input_bytes;
    job_ptr->bytes_read      = 0;
}

void own_deflate_job_perform(own_deflate_job *const job_ptr,
                             const own_deflate_match_finder match_finder,
                             const own_deflate_literals_handler literals_handler,
                             const own_deflate_matches_handler matches_handler,
                             const own_deflate_job_predicate predicate,
                             const own_deflate_job_callback callback) {
    // Variables
    const uint8_t *const upper_bound_ptr = job_ptr->upper_bound_ptr;
    const uint8_t *const lower_bound_ptr = job_ptr->lower_bound_ptr;

    // Main cycle
    while (job_ptr->current_ptr < (upper_bound_ptr - QPLC_DEFLATE_MINIMAL_MATCH_LENGTH) &&
           predicate(job_ptr)) {
        // Variables
        const deflate_match_t longest_match = match_finder(&job_ptr->histogram_ptr->table,
                                                           lower_bound_ptr,
                                                           job_ptr->current_ptr,
                                                           upper_bound_ptr - QPLC_DEFLATE_MINIMAL_MATCH_LENGTH);

        if (longest_match.length >= QPLC_DEFLATE_MINIMAL_MATCH_LENGTH) {
            matches_handler(job_ptr, longest_match, literals_handler);
        } else {
            literals_handler(job_ptr, job_ptr->current_ptr + 1u, false);
        }
    }

    // Processing last bytes
    if (predicate(job_ptr)) {
        literals_handler(job_ptr, upper_bound_ptr, true);
    }

    // Performing last logic
    callback(job_ptr);
}

void own_deflate_job_process_literals_no_instructions(own_deflate_job *const job_ptr,
                                                      const uint8_t *const upper_bound_ptr,
                                                      bool safe) {
    // Main cycle
    if (true == safe) {
        while (job_ptr->current_ptr < upper_bound_ptr) {
            // Variables
            const uint32_t bytes_for_hash = QPL_MIN(QPLC_DEFLATE_BYTES_FOR_HASH_CALCULATION,
                                                    (uint32_t) (upper_bound_ptr - job_ptr->current_ptr));

            const uint32_t hash_value = OWN_CRC32(job_ptr->current_ptr, bytes_for_hash, 0u) &
                                        job_ptr->histogram_ptr->table.hash_mask;

            // Updating histogram
            job_ptr->histogram_ptr->literals_matches[*job_ptr->current_ptr]++;

            // Updating hash table
            own_deflate_hash_table_update(&job_ptr->histogram_ptr->table,
                                          (uint32_t) (job_ptr->current_ptr - job_ptr->lower_bound_ptr),
                                          hash_value);

            // End of iteration
            own_deflate_job_switch_to_next(job_ptr, 1u);
        }
    } else {
        while (job_ptr->current_ptr < upper_bound_ptr) {
            // Variables
            const uint32_t hash_value = OWN_CRC32(job_ptr->current_ptr,
                                                  QPLC_DEFLATE_BYTES_FOR_HASH_CALCULATION,
                                                  0u) & job_ptr->histogram_ptr->table.hash_mask;

            // Updating histogram
            job_ptr->histogram_ptr->literals_matches[*job_ptr->current_ptr]++;

            // Updating hash table
            own_deflate_hash_table_update(&job_ptr->histogram_ptr->table,
                                          (uint32_t) (job_ptr->current_ptr - job_ptr->lower_bound_ptr),
                                          hash_value);

            // End of iteration
            own_deflate_job_switch_to_next(job_ptr, 1u);
        }
    }
}

void own_deflate_job_process_match_no_instructions(own_deflate_job *const job_ptr,
                                                   const deflate_match_t match,
                                                   const own_deflate_literals_handler literals_handler) {
    // Processing missed literals
    literals_handler(job_ptr, match.match_source_ptr, false);

    // Variables
    const uint8_t *const next_current_ptr = job_ptr->current_ptr + match.length;

    deflate_histogram_update_match(job_ptr->histogram_ptr, match);

    // Updating hash table for each missing literals
    own_deflate_job_update_missed_literals(job_ptr, next_current_ptr);
}

void own_initialize_deflate_job(own_deflate_job *const job_ptr,
                                const uint8_t *const source_ptr,
                                const uint32_t input_bytes,
                                const uint8_t *const output_ptr,
                                const uint32_t output_bytes,
                                const own_current_status block_status,
                                const qpl_statistics_mode statistics_mode) {
    if (initial_status == job_ptr->job_status) {
        bit_writer_init(&job_ptr->bit_writer);
    }

    own_deflate_job_reset_input(job_ptr, source_ptr, input_bytes);
    bit_writer_set_buffer(&job_ptr->bit_writer, output_ptr, output_bytes);

    // Simple assignment
    job_ptr->block_status    = block_status;
    job_ptr->job_status      = running_status;
    job_ptr->statistics_mode = statistics_mode;
}
