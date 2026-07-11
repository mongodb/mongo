// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/hex.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

constexpr std::string_view kHexUpper = "0123456789ABCDEF"sv;
constexpr std::string_view kHexLower = "0123456789abcdef"sv;

using EncodeLookupTable = std::array<std::array<char, 2>, 256>;

consteval EncodeLookupTable generateHexDumpTable(std::string_view hexDigits) {
    std::array<std::array<char, 2>, 256> arr;
    for (size_t i = 0; i < arr.size(); ++i) {
        arr[i][0] = hexDigits[(i >> 4) & 0xf];
        arr[i][1] = hexDigits[(i >> 0) & 0xf];
    }
    return arr;
}

/**
 * Encodes the raw input string 'data' to hex, two bytes at a time. The resulting string will be
 * exactly twice as long as the input string.
 */
std::string _hexPack(std::string_view data, const EncodeLookupTable& table) {
    std::string out(2 * data.size(), '\0');
    auto p = out.begin();
    for (auto c : data) {
        auto lookup = table[static_cast<unsigned char>(c)];
        *p++ = lookup[0];
        *p++ = lookup[1];
    }
    return out;
}

/**
 * Decodes the hex-encoded input string 's' into a raw string, two bytes at a time.
 * Only safe to call if the length of the input string is a multiple of 2.
 */
template <typename F>
void _decode(std::string_view s, const F& f) {
    for (auto p = s.begin(); p != s.end();) {
        auto hi = hexblob::decodeDigit(*p++);
        auto lo = hexblob::decodeDigit(*p++);
        f((hi << 4) | lo);
    }
}

}  // namespace

namespace hexblob {

unsigned char decodeDigit(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    uasserted(ErrorCodes::FailedToParse,
              fmt::format("The character \\x{:02x} failed to parse from hex.", c));
}

unsigned char decodePair(std::string_view c) {
    uassert(ErrorCodes::FailedToParse, "Need two hex digits", c.size() == 2);
    return (decodeDigit(c[0]) << 4) | decodeDigit(c[1]);
}

bool validate(std::string_view s) {
    // There must be an even number of characters, since each pair encodes a single byte.
    return s.size() % 2 == 0 &&
        std::all_of(s.begin(), s.end(), [](auto c) { return ctype::isXdigit(c); });
}

std::string encode(std::string_view data) {
    static constexpr EncodeLookupTable lookupTable = generateHexDumpTable(kHexUpper);
    return _hexPack(data, lookupTable);
}

std::string encodeLower(std::string_view data) {
    static constexpr EncodeLookupTable lookupTable = generateHexDumpTable(kHexLower);
    return _hexPack(data, lookupTable);
}

void decode(std::string_view s, BufBuilder* buf) {
    uassert(ErrorCodes::FailedToParse, "Hex blob with odd digit count", s.size() % 2 == 0);
    _decode(s, [&](unsigned char c) { buf->appendChar(c); });
}

std::string decode(std::string_view s) {
    uassert(ErrorCodes::FailedToParse, "Hex blob with odd digit count", s.size() % 2 == 0);
    return decodeFromValidSizedInput(s);
}

std::string decodeFromValidSizedInput(std::string_view s) {
    std::string r(s.size() / 2, '\0');
    size_t i = 0;
    _decode(s, [&](unsigned char c) { r[i++] = c; });
    return r;
}

}  // namespace hexblob

std::string hexdump(std::string_view data) {
    tassert(7781000, "Data length exceeds maximum buffer size", data.size() < kHexDumpMaxSize);
    std::string out;
    out.reserve(3 * data.size());
    char sep = 0;
    for (auto c : data) {
        if (sep)
            out.push_back(sep);
        out.append({kHexLower[(c & 0xF0) >> 4], kHexLower[(c & 0x0F)]});
        sep = ' ';
    }
    return out;
}

std::ostream& StreamableHexdump::_streamTo(std::ostream& os) const {
    std::string_view sep;
    for (auto p = _data; p != _data + _size; ++p) {
        os << sep << kHexLower[(*p >> 4) & 0x0f] << kHexLower[*p & 0x0f];
        sep = " "sv;
    }
    return os;
}

}  // namespace mongo
