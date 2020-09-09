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

#include "mongo/util/hex.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <iterator>
#include <string>

#include "mongo/base/error_codes.h"

namespace mongo {

namespace {

using namespace fmt::literals;

constexpr StringData kHexUpper = "0123456789ABCDEF"_sd;
constexpr StringData kHexLower = "0123456789abcdef"_sd;

std::string _hexPack(StringData data, StringData hexchars) {
    std::string out;
    out.reserve(2 * data.size());
    for (auto c : data) {
        out.append({hexchars[(c & 0xF0) >> 4], hexchars[(c & 0x0F)]});
    }
    return out;
}

template <typename F>
void _decode(StringData s, const F& f) {
    uassert(ErrorCodes::FailedToParse, "Hex blob with odd digit count", s.size() % 2 == 0);
    for (std::size_t i = 0; i != s.size(); i += 2)
        f(hexblob::decodePair(s.substr(i, 2)));
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
              "The character \\x{:02x} failed to parse from hex."_format(c));
}

unsigned char decodePair(StringData c) {
    uassert(ErrorCodes::FailedToParse, "Need two hex digits", c.size() == 2);
    return (decodeDigit(c[0]) << 4) | decodeDigit(c[1]);
}

bool validate(StringData s) {
    // There must be an even number of characters, since each pair encodes a single byte.
    return s.size() % 2 == 0 &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string encode(StringData data) {
    return _hexPack(data, kHexUpper);
}

std::string encodeLower(StringData data) {
    return _hexPack(data, kHexLower);
}

void decode(StringData s, BufBuilder* buf) {
    _decode(s, [&](unsigned char c) { buf->appendChar(c); });
}

std::string decode(StringData s) {
    std::string r;
    r.reserve(s.size() / 2);
    _decode(s, [&](unsigned char c) { r.push_back(c); });
    return r;
}

}  // namespace hexblob

std::string hexdump(StringData data) {
    verify(data.size() < 1000000);
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

}  // namespace mongo
