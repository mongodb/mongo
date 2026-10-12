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
 */

#ifndef UNICODE_FOLD_H
#define UNICODE_FOLD_H

#include "../mongocrypt-status-private.h"

#include <bson/bson.h>

#include <stddef.h>

bson_unichar_t unicode_codepoint_to_lower(bson_unichar_t codepoint);
bson_unichar_t unicode_codepoint_remove_diacritics(bson_unichar_t codepoint);

typedef enum {
    kUnicodeFoldNone = 0,
    kUnicodeFoldToLower = 1 << 0,
    kUnicodeFoldRemoveDiacritics = 1 << 1
} unicode_fold_options_t;

// Fold unicode string str of length len according to options. len should not include the null terminator, if it exists.
// Returns true if successful, and returns the null-terminated folded string and its byte length, excluding the null
// terminator, as out_str and out_len. On failure, returns false and sets status accordingly.
bool unicode_fold(const char *str,
                  size_t len,
                  unicode_fold_options_t options,
                  char **out_str,
                  size_t *out_len,
                  mongocrypt_status_t *status);

#endif
