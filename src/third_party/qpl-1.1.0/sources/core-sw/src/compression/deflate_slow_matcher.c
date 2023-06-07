/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "deflate_hash_table.h"
#include "qplc_checksum.h"
#include "crc.h"

#include "deflate_defs.h"

#if PLATFORM < K0

static inline uint32_t compare_strings(const uint8_t *const first_ptr,
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

static inline deflate_match_t get_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                             const uint8_t *const lower_bound_ptr,
                                             const uint8_t *const string_ptr,
                                             const uint8_t *const upper_bound_ptr) {
    // Variables
    const uint32_t
            hash_value = crc32_gzip_refl(0, string_ptr, OWN_BYTES_FOR_HASH_CALCULATION) & hash_table_ptr->hash_mask;

    uint32_t index                      = hash_table_ptr->hash_table_ptr[hash_value];
    uint8_t  *current_match_ptr         = (uint8_t *) (lower_bound_ptr + index);
    uint32_t match_length               = 0u;
    uint32_t attempt_number             = 0u;
#ifdef SCORE_FUNCTION
    uint32_t match_score                = 0u;
#endif
    uint32_t current_number_of_attempts = hash_table_ptr->attempts;

    deflate_match_t best_match = {match_length,
                                  (uint32_t) (current_match_ptr - lower_bound_ptr),
                                  (uint32_t) (string_ptr - current_match_ptr),
                                  hash_value,
                                  (uint8_t *) string_ptr};

    if (index == OWN_UNINITIALIZED_INDEX || best_match.offset > OWN_MAXIMAL_OFFSET) {
        // This was the first time we have faced this hash value
        return best_match;
    }

    // Main cycle
    match_length = compare_strings(current_match_ptr, string_ptr, upper_bound_ptr);
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
        if ((string_ptr - (lower_bound_ptr + index)) > OWN_MAXIMAL_OFFSET) {
            break;
        }

        current_match_ptr = (uint8_t *) (lower_bound_ptr + index);
        match_length      = compare_strings(current_match_ptr, string_ptr, upper_bound_ptr);
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

deflate_match_t get_lazy_best_match(const deflate_hash_table_t *const hash_table_ptr,
                                    const uint8_t *const lower_bound_ptr,
                                    const uint8_t *const string_ptr,
                                    const uint8_t *const upper_bound_ptr) {
    // Variables
    const uint8_t *current_ptr = string_ptr + 1u;

    // Getting initial matches for "lazy matching" logic from Zlib
    deflate_match_t longest_match      = get_best_match(hash_table_ptr, lower_bound_ptr, string_ptr, upper_bound_ptr);
    deflate_match_t next_longest_match = get_best_match(hash_table_ptr, lower_bound_ptr, current_ptr, upper_bound_ptr);

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
        next_longest_match = get_best_match(hash_table_ptr, lower_bound_ptr, current_ptr, upper_bound_ptr);
    }

    return longest_match;
}

#endif
