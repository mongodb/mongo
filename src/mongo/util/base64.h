// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <array>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace [[MONGO_MOD_FILE_PRIVATE]] base64_detail {
using namespace std::literals::string_view_literals;

/**
 * Abstract class used to split the translation formats below
 * into something resembling namespaced implementations.
 */
template <typename Mode>
class [[MONGO_MOD_PUBLIC]] Base64Impl {
private:
    Base64Impl() = delete;

public:
    /**
     * Encode a payload to base64.
     */
    static std::string encode(std::string_view in);
    static std::string encode(const void* data, size_t len) {
        return encode(std::string_view(reinterpret_cast<const char*>(data), len));
    }
    static void encode(std::stringstream& ss, std::string_view in);
    static void encode(fmt::memory_buffer& buffer, std::string_view in);

    /**
     * Decode a base64 string to its original payload.
     */
    static std::string decode(std::string_view in);
    static void decode(std::stringstream& ss, std::string_view in);
    static void decode(fmt::memory_buffer& buffer, std::string_view in);

    /**
     * Determines if a given string appears to be valid base64.
     */
    static bool validate(std::string_view s);

    /**
     * Calculate how large a given input would expand to.
     * Effectively: ceil(inLen * 4 / 3)
     */
    static constexpr std::size_t encodedLength(std::size_t inLen) {
        return (inLen + 2) / 3 * 4;
    }
};

constexpr unsigned char kInvalid = ~0;

constexpr std::size_t search(std::string_view table, int c) {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i] == c) {
            return i;
        }
    }

    return kInvalid;
}

template <std::size_t... Cs>
constexpr auto invertTable(std::string_view table, std::index_sequence<Cs...>) {
    return std::array<unsigned char, sizeof...(Cs)>{
        {static_cast<unsigned char>(search(table, Cs))...}};
}

struct Standard {
    static constexpr auto kEncodeTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"sv;
    static constexpr auto kDecodeTable = invertTable(kEncodeTable, std::make_index_sequence<256>{});
    static constexpr bool kTerminatorRequired = true;
};

// base64url encoding is a "url safe" variant of base64.
// '+' is replaced with '-'
// '/' is replaced with '_'
// '=' at the end of the string are optional
struct URL {
    static constexpr auto kEncodeTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"sv;
    static constexpr auto kDecodeTable = invertTable(kEncodeTable, std::make_index_sequence<256>{});
    static constexpr bool kTerminatorRequired = false;
};
}  // namespace base64_detail

using base64 = typename base64_detail::Base64Impl<base64_detail::Standard>;
using base64url = typename base64_detail::Base64Impl<base64_detail::URL>;

}  // namespace mongo
