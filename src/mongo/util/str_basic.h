// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * This header would be part of str.h, but is separated to break an include cycle with
 * bson/util/builder.h
 */

#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"

#include <cstring>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo::str {
/**
 * Throws if sd contains any bytes equal to '\0' within its range.
 *
 * Note: When a std::string_view is constructed from a C string or std::string, the final '\0' byte
 * is NOT considered in range and so will not cause this to throw.
 */
constexpr inline void uassertNoEmbeddedNulBytes(std::string_view sd) {
    uassert(9527900, "illegal embedded NUL byte", sd.find('\0') == std::string::npos);
}

/**
 * Copies the contents of sd to dest and appends a NUL byte.
 *
 * Throws if sd already contains a NUL byte.
 * Returns a pointer to the next byte to write to (equivalently, the byte after the appended NUL).
 */
constexpr inline char* copyAsCString(char* dest, std::string_view sd) {
    uassertNoEmbeddedNulBytes(sd);
    dest += sd.copy(dest, sd.size());
    *dest++ = '\0';
    return dest;
}


/** Utility function for a common use of isDigit() */
constexpr bool isAllDigits(std::string_view sd) noexcept {
    return std::all_of(sd.begin(), sd.end(), [](char c) { return ctype::isDigit(c); });
}

}  // namespace mongo::str
