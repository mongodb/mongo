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

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <ostream>
#include <type_traits>
#include <utility>

#include "mongo/base/string_data.h"

namespace mongo {

/**
 * A string-valued "structural type".
 *
 * Can be used to enable string values as template parameters.
 * Example:
 *
 *     template <FixedString name, typename Rep>
 *     struct UnitQuantity {
 *         constexpr static StringData unitName() const {
 *             return name;
 *         }
 *         std::string toString() const {
 *             return fmt::format("{} {}", value, unitName());
 *         }
 *         Rep value;
 *     };
 *     ...
 *     using Degrees = UnitQuantity<"degrees", double>;
 *     using Radians = UnitQuantity<"radians", double>;
 *     using Bytes = UnitQuantity<"bytes", uint64_t>;
 *     ...
 *     ASSERT_EQ(Degrees{180}.toString(), "180 degrees");
 *     ASSERT_EQ(Radians{3.14}.toString(), "3.14 radians");
 *
 * `FixedString` objects can be constructed by concatenation with `operator+`:
 *
 *     constexpr FixedString megaPrefix{"mega"};
 *     using Megabytes = UnitQuantity<megaPrefix + Bytes::unitName(), uint64_t>;
 *     ASSERT_EQ(Megabytes{123}.toString(), "123 megabytes");
 *
 * Based on a minimal interpretation of a proposed `std::fixed_string`.
 * See https://wg21.link/p3094.
 *
 * The stored string value is NUL terminated, and may contain embedded NULs.
 * N is the string value's length (not counting the NUL terminator).
 */
template <size_t N>
class FixedString {
public:
    // clang-format off

    /** Create from a list of characters. */
    template <typename... Chars>
        requires (sizeof...(Chars) == N) &&
                 (... && std::is_convertible_v<char, Chars>) &&
                 (... && !std::is_pointer_v<Chars>)
    constexpr explicit FixedString(Chars... chars) noexcept
        : _data{chars...} {}

    // clang-format on

    /**
     * Implicitly convert from a string literal.
     * This is the recommended way to make these.
     */
    explicit(false) consteval FixedString(char const (&str)[N + 1]) noexcept {
        std::copy(std::begin(str), std::end(str), _data.begin());
    }

    /** Implicitly convert to StringData. */
    explicit(false) constexpr operator StringData() const noexcept {
        return {_data.data(), N};
    }

    constexpr const char* data() const noexcept {
        return _data.data();
    }

    constexpr const char* c_str() const noexcept {
        return _data.data();
    }

    constexpr size_t size() const noexcept {
        return N;
    }

    // Pseudo-private. Must be public as a requirement to be a structural type.
    std::array<char, N + 1> _data;
};

template <size_t AN, size_t BN>
constexpr bool operator==(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} == StringData{b};
}

template <size_t AN, size_t BN>
constexpr bool operator!=(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} != StringData{b};
}

template <size_t AN, size_t BN>
constexpr bool operator<(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} < StringData{b};
}

template <size_t AN, size_t BN>
constexpr bool operator>(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} > StringData{b};
}

template <size_t AN, size_t BN>
constexpr bool operator<=(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} <= StringData{b};
}

template <size_t AN, size_t BN>
constexpr bool operator>=(const FixedString<AN>& a, const FixedString<BN>& b) {
    return StringData{a} >= StringData{b};
}

template <size_t N>
constexpr std::ostream& operator<<(std::ostream& os, const FixedString<N>& s) {
    return os << StringData{s};
}

// clang-format off
/** Deduce N from character list count. */
template <typename... Chars>
    requires(...&& std::is_convertible_v<Chars, char>)
FixedString(Chars... chars) -> FixedString<sizeof...(chars)>;
// clang-format on

/** Deduce N from string literal array size. */
template <size_t N>
FixedString(const char (&str)[N]) -> FixedString<N - 1>;

/** Concatenation. */
template <size_t AN, size_t BN>
consteval FixedString<AN + BN> operator+(const FixedString<AN>& a, const FixedString<BN>& b) {
    char data[AN + BN + 1];
    char* w = data;
    w = std::copy_n(a.data(), a.size(), w);
    w = std::copy_n(b.data(), b.size(), w);
    *w++ = 0;
    return data;
}

template <size_t N, size_t SN>
consteval FixedString<N + SN - 1> operator+(const FixedString<N>& a, const char (&s)[SN]) {
    return a + FixedString{s};
}

template <size_t N, size_t SN>
consteval FixedString<SN - 1 + N> operator+(const char (&s)[SN], const FixedString<N>& a) {
    return FixedString{s} + a;
}

template <size_t N>
consteval FixedString<N + 1> operator+(const FixedString<N>& a, char ch) {
    return a + FixedString{ch};
}

template <size_t N>
consteval FixedString<1 + N> operator+(char ch, const FixedString<N>& a) {
    return FixedString{ch} + a;
}

}  // namespace mongo
