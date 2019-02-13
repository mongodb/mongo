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

#include <algorithm>
#include <cctype>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
// can't use hex namespace because it conflicts with hex iostream function
inline StatusWith<char> fromHex(char c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "The character " << c << " failed to parse from hex.");
}
inline StatusWith<char> fromHex(const char* c) {
    if (fromHex(c[0]).isOK() && fromHex(c[1]).isOK()) {
        return (char)((fromHex(c[0]).getValue() << 4) | fromHex(c[1]).getValue());
    }
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "The character " << c[0] << c[1]
                                << " failed to parse from hex.");
}
inline StatusWith<char> fromHex(StringData c) {
    if (fromHex(c[0]).isOK() && fromHex(c[1]).isOK()) {
        return (char)((fromHex(c[0]).getValue() << 4) | fromHex(c[1]).getValue());
    }
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "The character " << c[0] << c[1]
                                << " failed to parse from hex.");
}

/**
 * Decodes 'hexString' into raw bytes, appended to the out parameter 'buf'. Callers must first
 * ensure that 'hexString' is a valid hex encoding.
 */
inline void fromHexString(StringData hexString, BufBuilder* buf) {
    invariant(hexString.size() % 2 == 0);
    // Combine every pair of two characters into one byte.
    for (std::size_t i = 0; i < hexString.size(); i += 2) {
        buf->appendChar(uassertStatusOK(fromHex(StringData(&hexString.rawData()[i], 2))));
    }
}

/**
 * Returns true if 'hexString' is a valid hexadecimal encoding.
 */
inline bool isValidHex(StringData hexString) {
    // There must be an even number of characters, since each pair encodes a single byte.
    return hexString.size() % 2 == 0 &&
        std::all_of(hexString.begin(), hexString.end(), [](char c) { return std::isxdigit(c); });
}

inline std::string toHex(const void* inRaw, int len) {
    static const char hexchars[] = "0123456789ABCDEF";

    StringBuilder out;
    const char* in = reinterpret_cast<const char*>(inRaw);
    for (int i = 0; i < len; ++i) {
        char c = in[i];
        char hi = hexchars[(c & 0xF0) >> 4];
        char lo = hexchars[(c & 0x0F)];

        out << hi << lo;
    }

    return out.str();
}

template <typename T>
std::string integerToHex(T val);

inline std::string toHexLower(const void* inRaw, int len) {
    static const char hexchars[] = "0123456789abcdef";

    StringBuilder out;
    const char* in = reinterpret_cast<const char*>(inRaw);
    for (int i = 0; i < len; ++i) {
        char c = in[i];
        char hi = hexchars[(c & 0xF0) >> 4];
        char lo = hexchars[(c & 0x0F)];

        out << hi << lo;
    }

    return out.str();
}

/**
 * Returns the hex value with a fixed width of 8 chatacters.
 */
std::string unsignedIntToFixedLengthHex(uint32_t val);

/* @return a dump of the buffer as hex byte ascii output */
std::string hexdump(const char* data, unsigned len);
}
