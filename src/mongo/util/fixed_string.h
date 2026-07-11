// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * A string-valued "structural type".
 *
 * Can be used to enable string values as template parameters.
 * Example:
 *
 *     template <FixedString name, typename Rep>
 *     struct UnitQuantity {
 *         constexpr static std::string_view unitName() const {
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
    /** Create from an argument pack of characters. */
    template <typename... Chars>
    requires(sizeof...(Chars) == N) &&                //
        (... && std::convertible_to<Chars, char>) &&  //
        (... && !std::is_pointer_v<Chars>)            //
    consteval explicit FixedString(Chars... chars) noexcept : _data{chars...} {}

    /**
     * Implicitly convert from a string literal.
     * This is the recommended way to make these.
     */
    explicit(false) consteval FixedString(char const (&str)[N + 1]) noexcept {
        std::copy(std::begin(str), std::end(str), _data.begin());
    }

    /** Implicitly convert to std::string_view. */
    explicit(false) constexpr operator std::string_view() const noexcept {
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

    template <size_t BN>
    friend constexpr bool operator==(const FixedString& a, const FixedString<BN>& b) {
        return std::string_view{a} == std::string_view{b};
    }

    template <size_t BN>
    friend constexpr auto operator<=>(const FixedString& a, const FixedString<BN>& b) {
        return std::string_view{a} <=> std::string_view{b};
    }

    friend constexpr std::ostream& operator<<(std::ostream& os, const FixedString& s) {
        return os << std::string_view{s};
    }

    /** Concatenation. */
    template <size_t BN>
    friend consteval FixedString<N + BN> operator+(const FixedString& a, const FixedString<BN>& b) {
        char data[N + BN + 1];
        char* w = data;
        w = std::copy_n(a.data(), a.size(), w);
        w = std::copy_n(b.data(), b.size(), w);
        *w++ = 0;
        return data;
    }

    template <size_t SN>
    friend consteval FixedString<N + SN - 1> operator+(const FixedString& a, const char (&s)[SN]) {
        return a + FixedString{s};
    }

    template <size_t SN>
    friend consteval FixedString<SN - 1 + N> operator+(const char (&s)[SN], const FixedString& a) {
        return FixedString{s} + a;
    }

    friend consteval FixedString<N + 1> operator+(const FixedString& a, char ch) {
        return a + FixedString<1>{ch};
    }

    friend consteval FixedString<1 + N> operator+(char ch, const FixedString& a) {
        return FixedString<1>{ch} + a;
    }

    // Pseudo-private. Must be public as a requirement to be a structural type.
    [[MONGO_MOD_FILE_PRIVATE]] std::array<char, N + 1> _data;
};

/** Deduce N from character argument pack count. */
template <typename... Chars>
requires(... && std::convertible_to<Chars, char>)
FixedString(Chars... chars) -> FixedString<sizeof...(chars)>;

/** Deduce N from string literal array size. */
template <size_t N>
FixedString(const char (&str)[N]) -> FixedString<N - 1>;

}  // namespace mongo
