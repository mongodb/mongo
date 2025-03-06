/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 *
 *    THIS IS A GENERATED FILE, DO NOT MODIFY.
 */

#include "fold.h"
#include "../mc-range-edge-generation-private.h" // For mc_count_leading_zeros_u64
#include "../mongocrypt-private.h"

static bool append_utf8_codepoint(bson_unichar_t codepoint, char **output_it, mongocrypt_status_t *status) {
    if (codepoint <= 0x7f /* max 1-byte codepoint */) {
        *(*output_it)++ = (char)codepoint;
    } else if (codepoint <= 0x7ff /* max 2-byte codepoint*/) {
        *(*output_it)++ = (char)((codepoint >> (6 * 1)) | 0xc0); // 2 leading 1s.
        *(*output_it)++ = (char)(((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    } else if (codepoint <= 0xffff /* max 3-byte codepoint*/) {
        *(*output_it)++ = (char)((codepoint >> (6 * 2)) | 0xe0); // 3 leading 1s.
        *(*output_it)++ = (char)(((codepoint >> (6 * 1)) & 0x3f) | 0x80);
        *(*output_it)++ = (char)(((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    } else {
        if (codepoint > 0x10FFFF) {
            CLIENT_ERR("append_utf8_codepoint: codepoint was out of range for UTF-8");
            return false;
        }
        *(*output_it)++ = (char)((codepoint >> (6 * 3)) | 0xf0); // 4 leading 1s.
        *(*output_it)++ = (char)(((codepoint >> (6 * 2)) & 0x3f) | 0x80);
        *(*output_it)++ = (char)(((codepoint >> (6 * 1)) & 0x3f) | 0x80);
        *(*output_it)++ = (char)(((codepoint >> (6 * 0)) & 0x3f) | 0x80);
    }
    return true;
}

// C translation of mongo::unicode::String::caseFoldAndStripDiacritics.
bool unicode_fold(const char *str,
                  size_t len,
                  unicode_fold_options_t options,
                  char **out_str,
                  size_t *out_len,
                  mongocrypt_status_t *status) {
    BSON_ASSERT_PARAM(str);
    BSON_ASSERT_PARAM(out_str);
    BSON_ASSERT_PARAM(out_len);
    BSON_ASSERT_PARAM(status);

    if (!(options & (kUnicodeFoldRemoveDiacritics | kUnicodeFoldToLower))) {
        CLIENT_ERR("unicode_fold: Either case or diacritic folding must be enabled");
        return false;
    }
    // Allocate space for possible growth. Folding characters may result in longer UTF-8 sequences.
    // 2x is an upper bound. With current fold maps, the largest growth is a 2-byte sequence mapping to a 3-byte
    // sequence.
    *out_str = bson_malloc(2 * len + 1);
    const char *input_it = str;
    const char *end_it = str + len;
    char *output_it = *out_str;
    while (input_it < end_it) {
        const uint8_t first_byte = (uint8_t)*input_it++;
        bson_unichar_t codepoint = 0;
        if (first_byte <= 0x7f) {
            // ASCII special case. Can use faster operations.
            if ((options & kUnicodeFoldToLower) && (first_byte >= 'A' && first_byte <= 'Z')) {
                codepoint = first_byte | 0x20; // Set the ascii lowercase bit on the character.
            } else {
                // ASCII has two pure diacritics that should be skipped, and no characters that
                // change when removing diacritics.
                if ((options & kUnicodeFoldRemoveDiacritics) && (first_byte == '^' || first_byte == '`')) {
                    continue;
                }
                codepoint = first_byte;
            }
        } else {
            // Multi-byte character
            size_t leading_ones = mc_count_leading_zeros_u64(~(((uint64_t)first_byte) << (64 - 8)));

            // Only checking enough to ensure that this code doesn't crash in the face of malformed
            // utf-8. We make no guarantees about what results will be returned in this case.
            if (!(leading_ones > 1 && leading_ones <= 4 && input_it + (leading_ones - 1) <= end_it)) {
                CLIENT_ERR("unicode_fold: Text contains invalid UTF-8");
                bson_free(*out_str);
                return false;
            }

            codepoint = (bson_unichar_t)(first_byte & (0xff >> leading_ones)); // mask off the size indicator.
            for (size_t sub_byte_index = 1; sub_byte_index < leading_ones; sub_byte_index++) {
                const uint8_t sub_byte = (uint8_t)*input_it++;
                codepoint <<= 6;
                codepoint |= sub_byte & 0x3f; // mask off continuation bits.
            }

            if (options & kUnicodeFoldToLower) {
                bson_unichar_t new_cp = unicode_codepoint_to_lower(codepoint);
                codepoint = new_cp;
            }

            if ((options & kUnicodeFoldRemoveDiacritics)) {
                codepoint = unicode_codepoint_remove_diacritics(codepoint);
                if (!codepoint) {
                    continue; // codepoint is a pure diacritic.
                }
            }
        }

        if (!append_utf8_codepoint(codepoint, &output_it, status)) {
            bson_free(*out_str);
            return false;
        }
    }

    // Null terminate
    *output_it = '\0';
    *out_len = (size_t)(output_it - *out_str);
    return true;
}
