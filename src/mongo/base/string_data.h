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

#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/ctype.h"
#include "mongo/util/debug_util.h"

#include <algorithm>
#include <concepts>
#include <cstring>
#include <functional>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>  // NOLINT
#include <type_traits>

#include <absl/hash/hash.h>
#include <fmt/format.h>

namespace mongo {

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
    constexpr StringData(const char* c)
        : StringData{c ? std::string_view{c} : std::string_view{}} {}

    StringData(const std::string& s) : StringData{std::string_view{s}} {}

    /**
     * Constructs a StringData with an explicit length. 'c' must
     * either be nullptr (in which case len must be zero), or be a
     * pointer into a character array. The StringData will refer to
     * the first 'len' characters starting at 'c'. The range of
     * characters in the half-open interval `[c, c + len)` must be valid.
     */
    constexpr StringData(const char* c, size_type len) : StringData(_checkedView(c, len)) {}

    /**
     * Constructs a StringData with iterator range [first, last). `first` points to the beginning of
     * the string. `last` points to the position past the end of the string.
     *
     * The constraint on `End` avoids competing with the `(ptr, len)` constructor.
     *
     * We accept whatever `std::string_view` accepts, as `std::string_view` already does advanced
     * concepts checks on these arguments.
     */
    template <typename It, typename End>
    requires(std::constructible_from<std::string_view, It, End> &&
             !std::convertible_to<End, size_type>)
    constexpr StringData(It first, End last) : _sv{first, last} {}

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

    constexpr bool contains(StringData v) const noexcept {
        return _sv.find(v._sv) != npos;
    }
    constexpr bool contains(char ch) const noexcept {
        return _sv.find(ch) != npos;
    }
    constexpr bool contains(const char* s) const {
        return _sv.find(s) != npos;
    }

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

    friend constexpr auto operator<=>(StringData a, StringData b) = default;
    friend constexpr bool operator==(StringData a, StringData b) = default;

    /** absl::Hash ADL hook (behave exactly as std::string_view). */
    template <typename H>
    friend H AbslHashValue(H h, StringData sd) {
        return H::combine(std::move(h), std::string_view{sd});
    }

private:
    explicit constexpr StringData(std::string_view sv) : _sv{sv} {}

    static constexpr std::string_view _checkedView(const char* c, size_type len) {
        if constexpr (kDebugBuild) {
            if (MONGO_unlikely(!c && (len != 0)))
                invariant(0, "StringData(nullptr,len) requires len==0");
        }
        return std::string_view{c, len};
    }

    std::string_view _sv;
};

// Adds support for boost::Hash.
size_t hash_value(StringData sd);

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

/**
 * Converts `StringData` -> `std::string_view`.
 * `std::string_view` is banned in favor of `StringData`.
 * Use this where `std::string_view` must be used instead.
 */
constexpr std::string_view toStdStringViewForInterop(StringData s) {
    return {s.data(), s.size()};
}

/**
 * Converts `std::string_view` -> `StringData`.
 * `std::string_view` is banned in favor of `StringData`.
 * Use this where `std::string_view` must be used instead.
 */
constexpr StringData toStringDataForInterop(std::string_view s) {
    return {s.data(), s.size()};
}

inline namespace literals {

/**
 * Makes a constexpr StringData from a user defined literal (e.g. "hello"_sd).
 * This allows for constexpr creation of `StringData` that are known at compile time.
 */
constexpr StringData operator""_sd(const char* c, std::size_t len) {
    return {c, len};
}
}  // namespace literals

}  // namespace mongo

namespace std {
template <>
struct hash<mongo::StringData> {
    size_t operator()(mongo::StringData s) const noexcept {
        return hash<std::string_view>{}(toStdStringViewForInterop(s));
    }
};
}  // namespace std

namespace fmt {
template <>
class formatter<mongo::StringData> : private formatter<std::string_view> {
    using Base = formatter<std::string_view>;

public:
    using Base::parse;

    auto format(mongo::StringData s, auto& fc) const {
        return Base::format(std::string_view{s}, fc);
    }
};
}  // namespace fmt
