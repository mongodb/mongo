// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/strnlen.h"

#include "mongo/config.h"  // IWYU pragma: keep

#ifndef MONGO_CONFIG_HAVE_STRNLEN

namespace mongo {

size_t strnlen(const char* s, size_t maxlen) {
    for (size_t i = 0; i < maxlen; ++i) {
        if (s[i] == '\0') {
            return i;
        }
    }
    return maxlen;
}

}  // namespace mongo

#endif
