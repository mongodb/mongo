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

#include "mc-str-encode-string-sets-private.h"
#include "mc-text-search-str-encode-private.h"
#include "mongocrypt-buffer-private.h"
#include "mongocrypt.h"
#include "unicode/fold.h"
#include <bson/bson.h>
#include <stdint.h>

// 16MiB - maximum length in bytes of a string to be encoded.
#define MAX_ENCODE_BYTE_LEN 16777216
// Number of bytes which are added to the base string before encryption.
#define OVERHEAD_BYTES 5

static mc_affix_set_t *generate_prefix_or_suffix_tree(const mc_utf8_string_with_bad_char_t *base_str,
                                                      uint32_t unfolded_byte_len,
                                                      uint32_t lb,
                                                      uint32_t ub,
                                                      bool is_prefix) {
    BSON_ASSERT_PARAM(base_str);
    // We encrypt (unfolded string + 5 bytes of extra BSON info) with a 16-byte block cipher.
    uint32_t encrypted_len = 16 * (uint32_t)((unfolded_byte_len + OVERHEAD_BYTES + 15) / 16);
    // Max len of a string that has this encrypted len.
    uint32_t padded_len = encrypted_len - OVERHEAD_BYTES;
    if (padded_len < lb) {
        // No valid substrings, return empty tree
        return NULL;
    }

    // Total number of substrings
    uint32_t msize = BSON_MIN(padded_len, ub) - lb + 1;
    uint32_t folded_codepoint_len = base_str->codepoint_len - 1; // remove one codepoint for 0xFF
    uint32_t real_max_len = BSON_MIN(folded_codepoint_len, ub);
    // Number of actual substrings, excluding padding
    uint32_t real_substrings = real_max_len >= lb ? real_max_len - lb + 1 : 0;
    // If real_substrings and msize differ, we need to insert padding, so allocate one extra slot.
    uint32_t set_size = real_substrings == msize ? real_substrings : real_substrings + 1;
    mc_affix_set_t *set = mc_affix_set_new(base_str, set_size);
    uint32_t n_inserted = 0;
    for (uint32_t i = lb; i < real_max_len + 1; i++, n_inserted++) {
        if (is_prefix) {
            // [0, lb), [0, lb + 1), ..., [0, min(len, ub))
            BSON_ASSERT(mc_affix_set_insert(set, 0, i));
        } else {
            // [len - lb, len), [len - lb - 1, len), ..., [max(0, len - ub), len)
            BSON_ASSERT(mc_affix_set_insert(set, folded_codepoint_len - i, folded_codepoint_len));
        }
    }
    if (msize != real_substrings) {
        // Insert padding to get to msize
        BSON_ASSERT(mc_affix_set_insert_base_string(set, msize - real_substrings));
        n_inserted++;
    }
    BSON_ASSERT(n_inserted == set_size);
    return set;
}

static mc_affix_set_t *generate_suffix_tree(const mc_utf8_string_with_bad_char_t *base_str,
                                            uint32_t unfolded_byte_len,
                                            const mc_FLE2SuffixInsertSpec_t *spec) {
    BSON_ASSERT_PARAM(base_str);
    BSON_ASSERT_PARAM(spec);
    return generate_prefix_or_suffix_tree(base_str, unfolded_byte_len, spec->lb, spec->ub, false);
}

static mc_affix_set_t *generate_prefix_tree(const mc_utf8_string_with_bad_char_t *base_str,
                                            uint32_t unfolded_byte_len,
                                            const mc_FLE2PrefixInsertSpec_t *spec) {
    BSON_ASSERT_PARAM(base_str);
    BSON_ASSERT_PARAM(spec);
    return generate_prefix_or_suffix_tree(base_str, unfolded_byte_len, spec->lb, spec->ub, true);
}

static uint32_t calc_number_of_substrings(uint32_t strlen, uint32_t lb, uint32_t ub) {
    // There are len - i + 1 substrings of length i in a length len string.
    // Therefore, the total number of substrings with length between lb and ub
    // is the sum of the integers inclusive between A = len - ub + 1 and B = len - lb + 1,
    // A <= B. This has a closed form: (A + B)(B - A + 1)/2.
    if (lb > strlen) {
        return 0;
    }
    uint32_t largest_substr = BSON_MIN(strlen, ub);
    uint32_t largest_substr_count = strlen - largest_substr + 1;
    uint32_t smallest_substr_count = strlen - lb + 1;
    return (largest_substr_count + smallest_substr_count) * (smallest_substr_count - largest_substr_count + 1) / 2;
}

static mc_substring_set_t *generate_substring_tree(const mc_utf8_string_with_bad_char_t *base_str,
                                                   uint32_t unfolded_byte_len,
                                                   const mc_FLE2SubstringInsertSpec_t *spec) {
    BSON_ASSERT_PARAM(base_str);
    BSON_ASSERT_PARAM(spec);
    // We encrypt (unfolded string + 5 bytes of extra BSON info) with a 16-byte block cipher.
    uint32_t encrypted_len = 16 * (uint32_t)((unfolded_byte_len + OVERHEAD_BYTES + 15) / 16);
    // Max len of a string that has this encrypted len.
    uint32_t padded_len = encrypted_len - OVERHEAD_BYTES;
    if (padded_len < spec->lb) {
        // No valid substrings, return empty tree
        return NULL;
    }

    // If you are following along with the OST paper, a slightly different calculation of msize is used. The following
    // justifies why that calculation and this calculation are equivalent.
    // At this point, it is established that:
    //     beta <= mlen
    //     lb <= padded_len
    //     lb <= ub <= mlen
    //
    // So, the following formula for msize in the OST paper:
    //     maxkgram_1 = sum_(j=lb, ub, (mlen - j + 1))
    //     maxkgram_2 = sum_(j=lb, min(ub, padded_len), (padded_len - j + 1))
    //     msize      = min(maxkgram_1, maxkgram_2)
    // can be simplified to:
    //     msize      = sum_(j=lb, min(ub, padded_len), (min(mlen, padded_len) - j + 1))
    //
    // because if padded_len <= ub, then it follows that padded_len <= ub <= mlen, and so
    //     maxkgram_1 = sum_(j=lb, ub, (mlen - j + 1))          # as above
    //     maxkgram_2 = sum_(j=lb, padded_len, (padded_len - j + 1))    # less or equal to maxkgram_1
    //     msize      = maxkgram_2
    // and if padded_len > ub, then it follows that:
    //     maxkgram_1 = sum_(j=lb, ub, (mlen - j + 1))          # as above
    //     maxkgram_2 = sum_(j=lb, ub, (padded_len - j + 1))        # same sum bounds as maxkgram_1
    //     msize      = sum_(j=lb, ub, (min(mlen, padded_len) - j + 1))
    // in both cases, msize can be rewritten as:
    //     msize      = sum_(j=lb, min(ub, padded_len), (min(mlen, padded_len) - j + 1))

    uint32_t folded_codepoint_len = base_str->codepoint_len - 1;
    // If mlen < padded_len, we only need to pad to mlen
    padded_len = BSON_MIN(spec->mlen, padded_len);
    // Total number of substrings -- i.e. the number of valid substrings IF the string spanned the full padded length
    uint32_t msize = calc_number_of_substrings(padded_len, spec->lb, spec->ub);
    uint32_t n_real_substrings = 0;
    mc_substring_set_t *set = mc_substring_set_new(base_str);
    // If folded len < LB, there are no real substrings, so we can skip (avoiding underflow via folded len - LB)
    if (folded_codepoint_len >= spec->lb) {
        for (uint32_t i = 0; i < folded_codepoint_len - spec->lb + 1; i++) {
            for (uint32_t j = i + spec->lb; j < BSON_MIN(folded_codepoint_len, i + spec->ub) + 1; j++) {
                // Only count successful, i.e. non-duplicate inserts
                if (mc_substring_set_insert(set, i, j)) {
                    n_real_substrings++;
                }
            }
        }
    }
    if (msize != n_real_substrings) {
        // Insert msize - n_real_substrings padding
        BSON_ASSERT(msize > n_real_substrings);
        mc_substring_set_increment_fake_string(set, msize - n_real_substrings);
    }
    return set;
}

static uint32_t mc_get_utf8_codepoint_length(const char *buf, uint32_t len) {
    BSON_ASSERT_PARAM(buf);
    const char *cur = buf;
    const char *end = buf + len;
    uint32_t codepoint_len = 0;
    while (cur < end) {
        cur = bson_utf8_next_char(cur);
        codepoint_len++;
    }
    return codepoint_len;
}

mc_str_encode_sets_t *mc_text_search_str_encode(const mc_FLE2TextSearchInsertSpec_t *spec,
                                                mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(spec);
    if (spec->len > MAX_ENCODE_BYTE_LEN) {
        CLIENT_ERR("StrEncode: String passed in was too long: String was %u bytes, but max is %u bytes",
                   spec->len,
                   MAX_ENCODE_BYTE_LEN);
        return NULL;
    }

    if (!bson_utf8_validate(spec->v, spec->len, false /* allow_null */)) {
        CLIENT_ERR("StrEncode: String passed in was not valid UTF-8");
        return NULL;
    }

    mc_utf8_string_with_bad_char_t *base_string;
    if (spec->casef || spec->diacf) {
        char *folded_str;
        size_t folded_str_bytes_len;
        if (!unicode_fold(spec->v,
                          spec->len,
                          (spec->casef * kUnicodeFoldToLower) | (spec->diacf * kUnicodeFoldRemoveDiacritics),
                          &folded_str,
                          &folded_str_bytes_len,
                          status)) {
            return NULL;
        }
        base_string = mc_utf8_string_with_bad_char_from_buffer(folded_str, (uint32_t)folded_str_bytes_len);
        bson_free(folded_str);
    } else {
        base_string = mc_utf8_string_with_bad_char_from_buffer(spec->v, spec->len);
    }

    mc_str_encode_sets_t *sets = bson_malloc0(sizeof(mc_str_encode_sets_t));
    // Base string is the folded string plus the 0xFF character
    sets->base_string = base_string;
    if (spec->suffix.set) {
        sets->suffix_set = generate_suffix_tree(sets->base_string, spec->len, &spec->suffix.value);
    }
    if (spec->prefix.set) {
        sets->prefix_set = generate_prefix_tree(sets->base_string, spec->len, &spec->prefix.value);
    }
    if (spec->substr.set) {
        uint32_t unfolded_codepoint_len = mc_get_utf8_codepoint_length(spec->v, spec->len);
        if (unfolded_codepoint_len > spec->substr.value.mlen) {
            CLIENT_ERR("StrEncode: String passed in was longer than the maximum length for substring indexing -- "
                       "String len: %u, max len: %u",
                       unfolded_codepoint_len,
                       spec->substr.value.mlen);
            mc_str_encode_sets_destroy(sets);
            return NULL;
        }
        sets->substring_set = generate_substring_tree(sets->base_string, spec->len, &spec->substr.value);
    }
    // Exact string is always equal to the base string up until the bad character
    _mongocrypt_buffer_from_data(&sets->exact, sets->base_string->buf.data, (uint32_t)sets->base_string->buf.len - 1);
    return sets;
}

void mc_str_encode_sets_destroy(mc_str_encode_sets_t *sets) {
    if (!sets) {
        return;
    }
    mc_utf8_string_with_bad_char_destroy(sets->base_string);
    mc_affix_set_destroy(sets->suffix_set);
    mc_affix_set_destroy(sets->prefix_set);
    mc_substring_set_destroy(sets->substring_set);
    bson_free(sets);
}