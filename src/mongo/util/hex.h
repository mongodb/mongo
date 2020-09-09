/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <string>
#include <type_traits>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

/**
 * A hex blob is a data interchange format, not meant to be
 * convenient to read. The functions in the hexblob namespace are
 * specifically to support it, rather than to serve more general
 * hexadecimal encoding for diagnostics.
 *
 * A hex blob is a packed run of hex digit pairs with no punctuation
 * or breaks between the encoded bytes. Upper case is produced by
 * encoders, but upper or lower case digits are accepted by the
 * decoders.
 */
namespace hexblob {

/**
 * Decodes hex digit `c` (upper or lower case).
 * Throws `FailedToParse` on failure.
 */
unsigned char decodeDigit(unsigned char c);

/**
 * Decodes hex digit pair `c` (upper or lower case).
 * Throws `FailedToParse` on failure.
 */
unsigned char decodePair(StringData c);

/**
 * Returns true if `s` is a valid encoded hex blob.
 */
bool validate(StringData s);

/**
 * Returns `data` rendered as a concatenation of uppercase hex digit pairs,
 * with no separation between bytes.
 */
std::string encode(StringData data);

/** Raw memory `encode` */
inline std::string encode(const void* data, size_t len) {
    return encode(StringData(reinterpret_cast<const char*>(data), len));
}

/** Same as `encode`, but with lowercase hex digits. */
std::string encodeLower(StringData data);

/** Raw memory `encodeLower` */
inline std::string encodeLower(const void* data, size_t len) {
    return encodeLower(StringData(reinterpret_cast<const char*>(data), len));
}

/**
 * Decodes hex blob `s`, appending its decoded bytes to `buf`.
 * Throws `FailedToParse` if `s` is not a valid hex blob encoding.
 */
void decode(StringData s, BufBuilder* buf);

/** Overload that returns the decoded hex blob as a `std::string`. */
std::string decode(StringData s);

}  // namespace hexblob

/**
 * Returns a dump of the buffer as lower case hex digit pairs separated by spaces.
 * Requires `len < 1000000`.
 */
std::string hexdump(StringData data);

/** Raw memory `hexdump`. */
inline std::string hexdump(const void* data, size_t len) {
    return hexdump(StringData(reinterpret_cast<const char*>(data), len));
}

/** Render `val` in upper case hex, zero-padded to its full width. */
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::string zeroPaddedHex(T val) {
    return format(FMT_STRING("{:0{}X}"), std::make_unsigned_t<T>(val), 2 * sizeof(val));
}

/** Render the unsigned equivalent of `val` in upper case hex. */
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::string unsignedHex(T val) {
    return format(FMT_STRING("{:X}"), std::make_unsigned_t<T>(val));
}

}  // namespace mongo
