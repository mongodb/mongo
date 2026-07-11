// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
unsigned char decodePair(std::string_view c);

/**
 * Returns true if `s` is a valid encoded hex blob.
 */
bool validate(std::string_view s);

/**
 * Returns `data` rendered as a concatenation of uppercase hex digit pairs,
 * with no separation between bytes.
 */
std::string encode(std::string_view data);

/** Raw memory `encode` */
inline std::string encode(const void* data, size_t len) {
    return encode(std::string_view(reinterpret_cast<const char*>(data), len));
}

/** Same as `encode`, but with lowercase hex digits. */
std::string encodeLower(std::string_view data);

/** Raw memory `encodeLower` */
inline std::string encodeLower(const void* data, size_t len) {
    return encodeLower(std::string_view(reinterpret_cast<const char*>(data), len));
}

/**
 * Decodes hex blob `s`, appending its decoded bytes to `buf`.
 * Throws `FailedToParse` if `s` is not a valid hex blob encoding.
 */
void decode(std::string_view s, BufBuilder* buf);

/** Overload that returns the decoded hex blob as a `std::string`. */
std::string decode(std::string_view s);

/**
 * Same as 'decode(std::string_view)', but slightly faster by omitting the size check on the input
 * value. Should only ever be called with trusted inputs that are known to have a multiple-of-2
 * length, otherwise the behavior is undefined.
 */
std::string decodeFromValidSizedInput(std::string_view s);

}  // namespace hexblob

static const size_t kHexDumpMaxSize = 1000000;

/**
 * Returns a dump of the buffer as lower case hex digit pairs separated by spaces.
 * Requires `len < kHexDumpMaxSize`.
 */
std::string hexdump(std::string_view data);

/** Raw memory `hexdump`. */
inline std::string hexdump(const void* data, size_t len) {
    return hexdump(std::string_view(reinterpret_cast<const char*>(data), len));
}

/** Render `val` in upper case hex, zero-padded to its full width. */
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::string zeroPaddedHex(T val) {
    return fmt::format("{:0{}X}", std::make_unsigned_t<T>(val), 2 * sizeof(val));
}

/** Render the unsigned equivalent of `val` in upper case hex. */
template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
std::string unsignedHex(T val) {
    return fmt::format("{:X}", std::make_unsigned_t<T>(val));
}

/**
 * Wraps a buffer of known size so its hex dump can be written to a stream without dynamic
 * allocation.
 */
class StreamableHexdump {
public:
    StreamableHexdump(const void* data, size_t size)
        : _data{reinterpret_cast<const unsigned char*>(data)}, _size(size) {}

    friend std::ostream& operator<<(std::ostream& os, const StreamableHexdump& dump) {
        return dump._streamTo(os);
    }

private:
    std::ostream& _streamTo(std::ostream& os) const;

    const unsigned char* _data;
    size_t _size;
};

// Template removed as it could lead to unexpected behavior with reference types
// Users should construct StreamableHexdump directly with appropriate data and size

}  // namespace mongo
