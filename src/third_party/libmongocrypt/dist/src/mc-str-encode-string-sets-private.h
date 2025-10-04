/*
 * Copyright 2024-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MONGOCRYPT_STR_ENCODE_STRING_SETS_PRIVATE_H
#define MONGOCRYPT_STR_ENCODE_STRING_SETS_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt.h"

// Represents a valid unicode string with the bad character 0xFF appended to the end. This is our base string which
// we build substring trees on. Stores all the valid code points in the string, plus one code point for 0xFF.
// Exposed for testing.
typedef struct {
    _mongocrypt_buffer_t buf;
    uint32_t *codepoint_offsets;
    uint32_t codepoint_len;
} mc_utf8_string_with_bad_char_t;

// Initialize by copying buffer into data and adding the bad character.
mc_utf8_string_with_bad_char_t *mc_utf8_string_with_bad_char_from_buffer(const char *buf, uint32_t len);

void mc_utf8_string_with_bad_char_destroy(mc_utf8_string_with_bad_char_t *utf8);

// Set of affixes of a shared base string. Does not do any duplicate prevention.
typedef struct _mc_affix_set_t mc_affix_set_t;

// Initialize affix set from base string and number of entries (this must be known as a prior).
mc_affix_set_t *mc_affix_set_new(const mc_utf8_string_with_bad_char_t *base_string, uint32_t n_indices);

void mc_affix_set_destroy(mc_affix_set_t *set);

// Insert affix into set. base_start/end_idx are codepoint indices. base_end_idx is exclusive. Returns true if
// inserted, false otherwise.
bool mc_affix_set_insert(mc_affix_set_t *set, uint32_t base_start_idx, uint32_t base_end_idx);

// Insert the base string count times into the set. Treated as a special case, since this is the only affix that
// will appear multiple times. Returns true if inserted, false otherwise.
bool mc_affix_set_insert_base_string(mc_affix_set_t *set, uint32_t count);

// Iterator on affix set.
typedef struct {
    mc_affix_set_t *set;
    uint32_t cur_idx;
} mc_affix_set_iter_t;

// Point the iterator to the first affix of the given set.
void mc_affix_set_iter_init(mc_affix_set_iter_t *it, mc_affix_set_t *set);

// Get the next affix, its length in bytes, and its count. Returns false if the set does not have a next element, true
// otherwise.
bool mc_affix_set_iter_next(mc_affix_set_iter_t *it, const char **str, uint32_t *byte_len, uint32_t *count);

// Set of substrings of a shared base string. Prevents duplicates.
typedef struct _mc_substring_set_t mc_substring_set_t;

mc_substring_set_t *mc_substring_set_new(const mc_utf8_string_with_bad_char_t *base_string);

void mc_substring_set_destroy(mc_substring_set_t *set);

// Insert the base string count times into the set. Treated as a special case, since this is the only substring that
// will appear multiple times. Always inserts successfully.
void mc_substring_set_increment_fake_string(mc_substring_set_t *set, uint32_t count);

// Insert substring into set. base_start/end_idx are codepoint indices. base_end_idx is exclusive. Returns true if
// inserted, false otherwise.
bool mc_substring_set_insert(mc_substring_set_t *set, uint32_t base_start_idx, uint32_t base_end_idx);

// Iterator on substring set.
typedef struct {
    mc_substring_set_t *set;
    void *cur_node;
    uint32_t cur_idx;
} mc_substring_set_iter_t;

// Point the iterator to the first substring of the given set.
void mc_substring_set_iter_init(mc_substring_set_iter_t *it, mc_substring_set_t *set);

// Get the next substring, its length in bytes, and its count. Returns false if the set does not have a next element,
// true otherwise.
bool mc_substring_set_iter_next(mc_substring_set_iter_t *it, const char **str, uint32_t *byte_len, uint32_t *count);

#endif
