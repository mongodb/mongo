/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

/**
 * @brief Implementation of high level deflate compression
 */

/* ------ Includes ------ */

#include "crc.h"
#include "deflate_defs.h"
#include "bit_writer.h"
#include "deflate_hash_table.h"
#include "deflate_histogram.h"
#include "own_deflate_job.h"
#include "qplc_compression_consts.h"

#define OWN_CRC32(buf, len, init_crc, ...) ((0, ##__VA_ARGS__) ? crc32_iscsi(buf, len, init_crc) : crc32_gzip_refl(init_crc, buf, len))

/* ------ Internal functions API ------ */

/**
 * @brief Finds the longest match using "lazy" logic from Zlib
 *
 * @param[in]  hash_table_ptr   pointer to @link own_deflate_hash_table @endlink that should be used for search
 * @param[in]  lower_bound_ptr  pointer to the lower boundary of the search
 * @param[in]  string_ptr       pointer to the string that could be matched previously
 * @param[in]  upper_bound_ptr  pointer to the upper boundary of the search
 *
 * @return @link own_match @endlink with information about the best match using "lazy" logic from Zlib
 */
static inline deflate_match_t own_get_lazy_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                                      const uint8_t *const lower_bound_ptr,
                                                      const uint8_t *const string_ptr,
                                                      const uint8_t *const upper_bound_ptr);

/**
 * @brief Finds the longest match using two hash-tables with hash-chain approach
 *
 * @param[in]  hash_table_ptr   pointer to @link own_deflate_hash_table @endlink that should be used for search
 * @param[in]  lower_bound_ptr  pointer to the lower boundary of the search
 * @param[in]  string_ptr       pointer to the string that could be matched previously
 * @param[in]  upper_bound_ptr  pointer to the upper boundary of the search
 *
 * @return @link own_match @endlink with information about the best match using hash-chain approach
 */
static inline deflate_match_t own_get_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                                 const uint8_t *const lower_bound_ptr,
                                                 const uint8_t *const string_ptr,
                                                 const uint8_t *const upper_bound_ptr);

/**
 * @brief Compares two given strings and returns length of the match
 *
 * @param[in]  first_ptr        pointer to the first string (it should be less than second_ptr)
 * @param[in]  second_ptr       pointer to the second string
 * @param[in]  upper_bound_ptr  pointer to the upper boundary of the comparing
 *
 * @return length of the match
 */
static inline uint32_t own_compare_strings(const uint8_t *const first_ptr,
                                           const uint8_t *const second_ptr,
                                           const uint8_t *const upper_bound_ptr);

#ifdef SCORE_FUNCTION

static inline uint32_t own_score_function(uint32_t length, uint32_t offset);

static inline uint32_t own_get_offset_extra_bits(uint32_t offset);

#endif

/* ------ Internal functions implementation ------ */

static inline deflate_match_t own_get_lazy_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                                      const uint8_t *const lower_bound_ptr,
                                                      const uint8_t *const string_ptr,
                                                      const uint8_t *const upper_bound_ptr) {
    // Variables
    const uint8_t *current_ptr = string_ptr + 1u;

    // Getting initial matches for "lazy matching" logic from Zlib
    deflate_match_t longest_match      = own_get_best_match(hash_table_ptr, lower_bound_ptr, string_ptr, upper_bound_ptr);
    deflate_match_t next_longest_match = own_get_best_match(hash_table_ptr, lower_bound_ptr, current_ptr, upper_bound_ptr);

    // Searching for the longest match
#ifdef SCORE_FUNCTION
    while (longest_match.score < next_longest_match.score &&
           longest_match.length < hash_table_ptr->lazy_match &&
           current_ptr < upper_bound_ptr) {
#else
    while (longest_match.length < next_longest_match.length &&
           longest_match.length < hash_table_ptr->lazy_match &&
           current_ptr < upper_bound_ptr) {
#endif
        // Shifting to next 4 bytes
        current_ptr++;

        // Getting next successful match
        longest_match      = next_longest_match;
        next_longest_match = own_get_best_match(hash_table_ptr, lower_bound_ptr, current_ptr, upper_bound_ptr);
    }

    return longest_match;
}

// TODO logic should be simplified
static inline deflate_match_t own_get_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                                 const uint8_t *const lower_bound_ptr,
                                                 const uint8_t *const string_ptr,
                                                 const uint8_t *const upper_bound_ptr) {
    // Variables
    const uint32_t hash_value = OWN_CRC32(string_ptr, QPLC_DEFLATE_BYTES_FOR_HASH_CALCULATION, 0) & hash_table_ptr->hash_mask;

    uint32_t        index                      = hash_table_ptr->hash_table_ptr[hash_value];
    uint8_t         *current_match_ptr         = (uint8_t *) (lower_bound_ptr + index);
    uint32_t        match_length               = 0u;
    uint32_t        attempt_number             = 0u;
#ifdef SCORE_FUNCTION
    uint32_t        match_score                = 0u;
#endif
    uint32_t        current_number_of_attempts = hash_table_ptr->attempts;
    deflate_match_t best_match                 = {
            .index = (uint32_t) (current_match_ptr - lower_bound_ptr),
            .length = match_length,
            .offset = (uint32_t) (string_ptr - current_match_ptr),
            .hash = hash_value,
#ifdef SCORE_FUNCTION
            .score = match_score,
#endif
            .match_source_ptr = (uint8_t *) string_ptr
    };

    if (index == OWN_UNINITIALIZED_INDEX || best_match.offset > QPLC_DEFLATE_MAXIMAL_OFFSET) {
        // This was the first time we have faced this hash value
        return best_match;
    }

    // Main cycle
    match_length = own_compare_strings(current_match_ptr, string_ptr, upper_bound_ptr);
#ifdef SCORE_FUNCTION
    match_score = own_score_function(match_length, string_ptr - current_match_ptr);
#endif

    best_match.index  = index;
    best_match.length = match_length;
    best_match.offset = (uint32_t) (string_ptr - current_match_ptr);
#ifdef SCORE_FUNCTION
    best_match.score = match_score;
#endif

    // Perform a "good match" logic from Zlib
    if (match_length > hash_table_ptr->good_match) {
        /*
         * If new match length is longer than good match parameter
         * we have to decrease the depth of the search in 4 times
         */
        current_number_of_attempts >>= 2u;
    }

    index = hash_table_ptr->hash_story_ptr[index & hash_table_ptr->hash_mask];

    while (index != OWN_UNINITIALIZED_INDEX &&
           attempt_number < current_number_of_attempts) {
        if ((string_ptr - (lower_bound_ptr + index)) > QPLC_DEFLATE_MAXIMAL_OFFSET) {
            break;
        }

        current_match_ptr = (uint8_t *) (lower_bound_ptr + index);
        match_length      = own_compare_strings(current_match_ptr, string_ptr, upper_bound_ptr);
#ifdef SCORE_FUNCTION
        match_score = own_score_function(match_length, string_ptr - current_match_ptr);
#endif

        // Update the best match if all conditions are met
#ifdef SCORE_FUNCTION
        if (best_match.score < match_score) {
#else
        if (best_match.length < match_length) {
#endif
            best_match.index  = index;
            best_match.length = match_length;
#ifdef SCORE_FUNCTION
            best_match.score = match_score;
#endif
            best_match.offset = (uint32_t) (string_ptr - current_match_ptr);

            if (best_match.length >= 258u) {
                break;
            }
        }

        // Going to next iteration
        index = hash_table_ptr->hash_story_ptr[index & hash_table_ptr->hash_mask];
        attempt_number++;
    }

    return best_match;
}

static inline uint32_t own_compare_strings(const uint8_t *const first_ptr,
                                           const uint8_t *const second_ptr,
                                           const uint8_t *const upper_bound_ptr) {
    // Variables
    uint32_t match_length = 0u;

    // Main cycle
    if (first_ptr >= second_ptr) {
        return match_length;
    }

    while ((first_ptr[match_length] == second_ptr[match_length]) &&
        (second_ptr + match_length < upper_bound_ptr) &&
        match_length <= 257u) {
        match_length++;
    }

    return match_length;
}

/**
 * Simple stub callback for @link own_deflate_job @endlink
 */
#define OWN_STUB_CALLBACK own_deflate_job_callback_stub

/**
 * Simple stub callback for @link own_deflate_job @endlink
 */
#define OWN_STUB_PREDICATE own_deflate_job_predicate_stub

void own_deflate_job_callback_stub(own_deflate_job *UNREFERENCED_PARAMETER(job_ptr)) {
    // This is just a stub
}

uint8_t own_deflate_job_predicate_stub(own_deflate_job *const UNREFERENCED_PARAMETER(job_ptr)) {
    // This is just a stub
    return 1u;
}

void own_update_deflate_histogram_high_level(own_deflate_job *deflate_job_ptr) {
    own_deflate_job_perform(deflate_job_ptr,
                            own_get_lazy_best_match,
                            own_deflate_job_process_literals_no_instructions,
                            own_deflate_job_process_match_no_instructions,
                            OWN_STUB_PREDICATE,
                            OWN_STUB_CALLBACK);
}

#ifdef SCORE_FUNCTION

static inline uint32_t own_score_function(const uint32_t length, const uint32_t offset)
{
    const uint32_t score = length * 8u - own_get_offset_extra_bits(offset);

    return length == 0u ? 0u : score;
}

static int bsr(int val)
{
    if (val == 0) return 0;
    return (31 - __builtin_clz(val));
}

static inline uint32_t own_get_offset_extra_bits(const uint32_t offset)
{
    return offset < 5u ? 0u : (bsr(offset - 1u) - 1u);
}

#endif
