// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/hex.h"

#include "mongo/base/error_codes.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
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

constexpr std::array<uint8_t, 256> kDecodeLookupTable = [] {
    std::array<uint8_t, 256> arr;
    arr.fill(255);
    for (unsigned char c = '0'; c <= '9'; ++c)
        arr[c] = c - '0';
    for (unsigned char c = 'a'; c <= 'f'; ++c)
        arr[c] = 10 + c - 'a';
    for (unsigned char c = 'A'; c <= 'F'; ++c)
        arr[c] = 10 + c - 'A';
    return arr;
}();

/**
 * Function that throws the 'FailedToParse' exception for invalid inputs.
 * Intentionally out-of-line here and not inlined as a simple 'uassert()' inside '_decode()' for
 * performance reasons. Re-inling the function into '_decode()' may result in a performance
 * degradation on some platforms.
 */
MONGO_COMPILER_NORETURN void _throwInvalidDigit(unsigned char c0, unsigned char c1) {
    uasserted(ErrorCodes::FailedToParse,
              fmt::format("The characters {:#02x} {:#02x} failed to parse from hex.", c0, c1));
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
 * Looks up the assigned numeric value in the decode table for character 'c'. Returns 255 for
 * characters that cannot be translated.
 */
uint8_t _decodeCharacter(unsigned char c) {
    return kDecodeLookupTable[static_cast<uint8_t>(c)];
}

/**
 * Decodes the hex-encoded input string 's' into a raw string, two bytes at a time.
 * Only safe to call if the length of the input string is a multiple of 2.
 */
void _decode(std::string_view s, const auto& f) {
    for (auto p = s.begin(); p != s.end(); p += 2) {
        auto c0 = static_cast<unsigned char>(p[0]);
        auto c1 = static_cast<unsigned char>(p[1]);
        uint8_t hi = _decodeCharacter(c0);
        uint8_t lo = _decodeCharacter(c1);
        if (MONGO_unlikely((hi | lo) >= 16)) {
            _throwInvalidDigit(c0, c1);
        }
        f((hi << 4) | lo);
    }
}

}  // namespace

namespace hexblob {

unsigned char decodeDigit(unsigned char c) {
    uint8_t decoded = _decodeCharacter(c);
    uassert(ErrorCodes::FailedToParse,
            fmt::format("The character \\x{:02x} failed to parse from hex.", c),
            decoded <= 15);
    return decoded;
}

unsigned char decodePair(std::string_view s) {
    uassert(ErrorCodes::FailedToParse, "Need two hex digits", s.size() == 2);
    unsigned char ret = 0;
    _decode(s, [&](unsigned char c) { ret = c; });
    return ret;
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
