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
#include <cstring>
#include <fmt/format.h>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>  // NOLINT
#include <type_traits>

#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/ctype.h"
#include "mongo/util/debug_util.h"

namespace mongo {

// Set to 1 if XCode supports `std::string_view` c++20 features.
#define MONGO_STRING_DATA_CXX20 0

/**
 * A StringData object refers to an array of `char` without owning it.
 * The most common usage is as a function argument.
 *
 * Implements the API of `std::string_view`, and is implemented
 * by forwarding member functions to the internal `string_view` member.
 * Ultimately this class will be removed and we'll just use `std::string_view`
 * directly, so we must restrict it to the feature set of `std::string_view`
 * supported by all supported platforms. They are not all the same.
 *
 * The iterator is not always a raw pointer. On Windows, it is a class,
 * which enables useful integrity checks in debug builds.
 *
 * XCode is basically implementing at the C++17 level. There's no `operator<=>`,
 * no range constructors, and no `contains` member.
 *
 * The string data to which StringData refers must outlive it.
 *
 * StringData is not null-terminated.
 *
 * StringData should almost always be passed by value.
 *
 * See https://en.cppreference.com/w/cpp/string/basic_string_view.
 */
class MONGO_GSL_POINTER StringData {
public:
    using value_type = std::string_view::value_type;
    using const_pointer = std::string_view::const_pointer;
    using pointer = std::string_view::pointer;
    using const_reference = std::string_view::const_reference;
    using reference = std::string_view::reference;
    using const_iterator = std::string_view::const_iterator;
    using iterator = std::string_view::iterator;
    using const_reverse_iterator = std::string_view::const_reverse_iterator;
    using reverse_iterator = std::string_view::reverse_iterator;
    using size_type = std::string_view::size_type;
    using difference_type = std::string_view::difference_type;

    static constexpr inline size_type npos = std::string_view::npos;

    constexpr StringData() = default;

    StringData(std::nullptr_t) = delete;  // C++23, but harmless

    /**
     * Used where string length is not known in advance.
     * 'c' must be null or point to a null-terminated string.
     */
    StringData(const char* c)
        : StringData{std::string_view{c, (c && c[0] != '\0') ? std::strlen(c) : 0}} {}

    StringData(const std::string& s) : StringData{std::string_view{s}} {}

    /**
     * Constructs a StringData with an explicit length. 'c' must
     * either be nullptr (in which case len must be zero), or be a
     * pointer into a character array. The StringData will refer to
     * the first 'len' characters starting at 'c'. The range of
     * characters in the half-open interval `[c, c + len)` must be valid.
     */
    constexpr StringData(const char* c, size_type len) : StringData(std::string_view{c, len}) {
        if constexpr (kDebugBuild) {
            if (MONGO_unlikely(!data() && (size() != 0))) {
                invariant(0, "StringData(nullptr,len) requires len==0");
            }
        }
    }

#if MONGO_STRING_DATA_CXX20
    /**
     * Constructs a StringData with iterator range [first, last). `first` points to the beginning of
     * the string. `last` points to the position past the end of the string.
     *
     * We template the second parameter to ensure if StringData is called with literal 0 in the
     * second parameter, the (const char*, size_t) constructor is chosen instead.
     *
     * `std::string_view` already does advanced concepts checks on these arguments, so we
     * use `std::is_constructible` to just accept whatever `std::string_view` accepts.
     */
    template <typename It,
              typename End,
              std::enable_if_t<std::is_constructible_v<std::string_view, It, End> &&
                                   !std::is_convertible_v<End, size_type>,
                               int> = 0>
    constexpr StringData(It first, End last) : _sv{first, last} {}
#endif  // MONGO_STRING_DATA_CXX20

    explicit operator std::string() const {
        return std::string{_sv};
    }

    explicit constexpr operator std::string_view() const noexcept {
        return _sv;
    }

    //
    // iterators
    //

    constexpr const_iterator begin() const noexcept {
        return _sv.begin();
    }
    constexpr const_iterator end() const noexcept {
        return _sv.end();
    }
    constexpr const_iterator cbegin() const noexcept {
        return _sv.cbegin();
    }
    constexpr const_iterator cend() const noexcept {
        return _sv.cend();
    }

    constexpr const_reverse_iterator rbegin() const noexcept {
        return _sv.rbegin();
    }
    constexpr const_reverse_iterator rend() const noexcept {
        return _sv.rend();
    }
    constexpr const_reverse_iterator crbegin() const noexcept {
        return _sv.crbegin();
    }
    constexpr const_reverse_iterator crend() const noexcept {
        return _sv.crend();
    }

    //
    // accessors
    //

    constexpr const_reference operator[](size_t pos) const {
        if (kDebugBuild && !(pos < size()))
            invariant(pos < size(), "StringData index out of range");
        return _sv[pos];
    }
    constexpr const_reference at(size_t pos) const {
        return _sv.at(pos);
    }
    constexpr const_reference back() const {
        return _sv.back();
    }
    constexpr const_reference front() const {
        return _sv.front();
    }
    constexpr const char* data() const noexcept {
        return _sv.data();
    }

    //
    // Capacity
    //

    constexpr bool empty() const noexcept {
        return _sv.empty();
    }
    constexpr size_type size() const noexcept {
        return _sv.size();
    }
    constexpr size_type length() const noexcept {
        return _sv.length();
    }
    constexpr size_type max_size() const noexcept {
        return _sv.max_size();
    }

    //
    // Modifiers
    //

    /** Moves the front of the view forward by n characters. Requires n < size. */
    constexpr void remove_prefix(size_type n) {
        _sv.remove_prefix(n);
    }

    /** Moves the end of the view back by n characters. Requires n < size. */
    constexpr void remove_suffix(size_type n) {
        _sv.remove_suffix(n);
    }

    constexpr void swap(StringData& v) noexcept {
        _sv.swap(v._sv);
    }

    //
    // Operations
    //

    constexpr size_type copy(char* dest, size_type count, size_type pos = 0) const {
        return _sv.copy(dest, count, pos);
    }

    constexpr StringData substr(size_type pos = 0, size_type n = npos) const {
        return StringData{_sv.substr(pos, n)};
    }

    /**
     * Returns <0, 0, or >0 if 'this' is less, equal, or greater than 'v' in
     * lexicographical order.
     */
    constexpr int compare(StringData v) const noexcept {
        return _sv.compare(v._sv);
    }
    constexpr int compare(size_type pos1, size_type count1, StringData v) const {
        return _sv.compare(pos1, count1, v._sv);
    }
    constexpr int compare(
        size_type pos1, size_type count1, StringData v, size_type pos2, size_type count2) const {
        return _sv.compare(pos1, count1, v._sv, pos2, count2);
    }
    constexpr int compare(const char* s) const {
        return _sv.compare(s);
    }
    constexpr int compare(size_type pos1, size_type count1, const char* s) const {
        return _sv.compare(pos1, count1, s);
    }
    constexpr int compare(size_type pos1, size_type count1, const char* s, size_type count2) const {
        return _sv.compare(pos1, count1, s, count2);
    }

    /** True if 'prefix' is a substring anchored at begin. */
    constexpr bool starts_with(StringData v) const noexcept {
        return _sv.starts_with(v._sv);
    }
    constexpr bool starts_with(char ch) const noexcept {
        return _sv.starts_with(ch);
    }
    constexpr bool starts_with(const char* s) const {
        return _sv.starts_with(s);
    }

    /** True if 'suffix' is a substring anchored at end. */
    constexpr bool ends_with(StringData v) const noexcept {
        return _sv.ends_with(v._sv);
    }
    constexpr bool ends_with(char ch) const noexcept {
        return _sv.ends_with(ch);
    }
    constexpr bool ends_with(const char* s) const {
        return _sv.ends_with(s);
    }

#if MONGO_STRING_DATA_CXX20
    constexpr bool contains(StringData v) const noexcept {
        return _sv.find(v._sv) != npos;
    }
    constexpr bool contains(char ch) const noexcept {
        return _sv.find(ch) != npos;
    }
    constexpr bool contains(const char* s) const {
        return _sv.find(s) != npos;
    }
#endif  // MONGO_STRING_DATA_CXX20

/** The "find" family of functions have identical overload sets. */
#define STRING_DATA_DEFINE_FIND_OVERLOADS_(func, posDefault)                            \
    constexpr size_type func(StringData v, size_type pos = posDefault) const noexcept { \
        return _sv.func(v._sv, pos);                                                    \
    }                                                                                   \
    constexpr size_type func(char ch, size_type pos = posDefault) const noexcept {      \
        return _sv.func(ch, pos);                                                       \
    }                                                                                   \
    constexpr size_type func(const char* s, size_type pos, size_type count) const {     \
        return _sv.func(s, pos, count);                                                 \
    }                                                                                   \
    constexpr size_type func(const char* s, size_type pos = posDefault) const {         \
        return _sv.func(s, pos);                                                        \
    }
    STRING_DATA_DEFINE_FIND_OVERLOADS_(find, 0)
    STRING_DATA_DEFINE_FIND_OVERLOADS_(rfind, npos)
    STRING_DATA_DEFINE_FIND_OVERLOADS_(find_first_of, 0)
    STRING_DATA_DEFINE_FIND_OVERLOADS_(find_last_of, npos)
    STRING_DATA_DEFINE_FIND_OVERLOADS_(find_first_not_of, 0)
    STRING_DATA_DEFINE_FIND_OVERLOADS_(find_last_not_of, npos)
#undef STRING_DATA_FIND_OVERLOADS_

    //
    // MongoDB extras
    //

    constexpr bool startsWith(StringData prefix) const noexcept {
        return starts_with(prefix);
    }

    constexpr bool endsWith(StringData suffix) const noexcept {
        return ends_with(suffix);
    }

    std::string toString() const {
        return std::string{_sv};
    }

    constexpr const char* rawData() const noexcept {
        return data();
    }

    /** Uses tolower, and therefore does not handle some languages correctly. */
    bool equalCaseInsensitive(StringData other) const {
        return size() == other.size() &&
            std::equal(begin(), end(), other.begin(), other.end(), [](char a, char b) {
                   return ctype::toLower(a) == ctype::toLower(b);
               });
    }

    void copyTo(char* dest, bool includeEndingNull) const {
        if (!empty())
            copy(dest, size());
        if (includeEndingNull)
            dest[size()] = 0;
    }

private:
    explicit constexpr StringData(std::string_view sv) : _sv{sv} {}

    std::string_view _sv;
};

#if MONGO_STRING_DATA_CXX20
inline constexpr auto operator<=>(StringData a, StringData b) noexcept {
    return std::string_view{a} <=> std::string_view{b};
}
#else   // !MONGO_STRING_DATA_CXX20
inline constexpr bool operator==(StringData a, StringData b) noexcept {
    return std::string_view{a} == std::string_view{b};
}
inline constexpr bool operator!=(StringData a, StringData b) noexcept {
    return std::string_view{a} != std::string_view{b};
}
inline constexpr bool operator<(StringData a, StringData b) noexcept {
    return std::string_view{a} < std::string_view{b};
}
inline constexpr bool operator>(StringData a, StringData b) noexcept {
    return std::string_view{a} > std::string_view{b};
}
inline constexpr bool operator<=(StringData a, StringData b) noexcept {
    return std::string_view{a} <= std::string_view{b};
}
inline constexpr bool operator>=(StringData a, StringData b) noexcept {
    return std::string_view{a} >= std::string_view{b};
}
#endif  // !MONGO_STRING_DATA_CXX20

std::ostream& operator<<(std::ostream& os, StringData v);

inline std::string& operator+=(std::string& a, StringData b) {
    return a += std::string_view{b};
}

/** Not supported by `std::string_view`. */
inline std::string operator+(std::string a, StringData b) {
    return a += b;
}
/** Not supported by `std::string_view`. */
inline std::string operator+(StringData a, std::string b) {
    return b.insert(0, std::string_view{a});
}

inline namespace literals {

/**
 * Makes a constexpr StringData from a user defined literal (e.g. "hello"_sd).
 * This allows for constexpr creation of `StringData` that are known at compile time.
 */
constexpr StringData operator"" _sd(const char* c, std::size_t len) {
    return {c, len};
}
}  // namespace literals

}  // namespace mongo

namespace fmt {
template <>
class formatter<mongo::StringData> : formatter<std::string_view> {
    using Base = formatter<std::string_view>;

public:
    using Base::parse;
    template <typename FormatContext>
    auto format(mongo::StringData s, FormatContext& fc) {
        return Base::format(std::string_view{s}, fc);
    }
};
}  // namespace fmt
