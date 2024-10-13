/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

/**
 * This header would be part of str.h, but is separated to break an include cycle with
 * bson/util/builder.h
 */

#include <cstring>

#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo::str {
/**
 * Throws if sd contains any bytes equal to '\0' within its range.
 *
 * Note: When a StringData is constructed from a C string or std::string, the final '\0' byte is NOT
 * considered in range and so will not cause this to throw.
 */
constexpr inline void uassertNoEmbeddedNulBytes(StringData sd) {
    uassert(9527900, "illegal embedded NUL byte", sd.find('\0') == std::string::npos);
}

/**
 * Copies the contents of sd to dest and appends a NUL byte.
 *
 * Throws if sd already contains a NUL byte.
 * Returns a pointer to the next byte to write to (equivalently, the byte after the appended NUL).
 */
constexpr inline char* copyAsCString(char* dest, StringData sd) {
    uassertNoEmbeddedNulBytes(sd);
    dest += sd.copy(dest, sd.size());
    *dest++ = '\0';
    return dest;
}
}  // namespace mongo::str
