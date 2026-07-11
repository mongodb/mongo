// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <cstdlib>

#ifdef _WIN32
static inline long long strtoll(const char* nptr, char** endptr, int base) {
    return _strtoi64(nptr, endptr, base);
}

static inline unsigned long long strtoull(const char* nptr, char** endptr, int base) {
    return _strtoui64(nptr, endptr, base);
}
#endif  // defined(_WIN32)
