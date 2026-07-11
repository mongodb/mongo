// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
