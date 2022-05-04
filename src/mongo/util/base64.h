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
#include <sstream>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
namespace base64_detail {

/**
 * Abstract class used to split the translation formats below
 * into something resembling namespaced implementations.
 */
template <typename Mode>
class Base64Impl {
private:
    Base64Impl() = delete;

public:
    /**
     * Encode a payload to base64.
     */
    static std::string encode(StringData in);
    static std::string encode(const void* data, size_t len) {
        return encode(StringData(reinterpret_cast<const char*>(data), len));
    }
    static void encode(std::stringstream& ss, StringData in);
    static void encode(fmt::memory_buffer& buffer, StringData in);

    /**
     * Decode a base64 string to its original payload.
     */
    static std::string decode(StringData in);
    static void decode(std::stringstream& ss, StringData in);
    static void decode(fmt::memory_buffer& buffer, StringData in);

    /**
     * Determines if a given string appears to be valid base64.
     */
    static bool validate(StringData s);

    /**
     * Calculate how large a given input would expand to.
     * Effectively: ceil(inLen * 4 / 3)
     */
    static constexpr std::size_t encodedLength(std::size_t inLen) {
        return (inLen + 2) / 3 * 4;
    }
};

constexpr unsigned char kInvalid = ~0;

constexpr std::size_t search(StringData table, int c) {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i] == c) {
            return i;
        }
    }

    return kInvalid;
}

template <std::size_t... Cs>
constexpr auto invertTable(StringData table, std::index_sequence<Cs...>) {
    return std::array<unsigned char, sizeof...(Cs)>{
        {static_cast<unsigned char>(search(table, Cs))...}};
}

struct Standard {
    static constexpr auto kEncodeTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"_sd;
    static constexpr auto kDecodeTable = invertTable(kEncodeTable, std::make_index_sequence<256>{});
    static constexpr bool kTerminatorRequired = true;
};

// base64url encoding is a "url safe" variant of base64.
// '+' is replaced with '-'
// '/' is replaced with '_'
// '=' at the end of the string are optional
struct URL {
    static constexpr auto kEncodeTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"_sd;
    static constexpr auto kDecodeTable = invertTable(kEncodeTable, std::make_index_sequence<256>{});
    static constexpr bool kTerminatorRequired = false;
};
}  // namespace base64_detail

using base64 = typename base64_detail::Base64Impl<base64_detail::Standard>;
using base64url = typename base64_detail::Base64Impl<base64_detail::URL>;

}  // namespace mongo
