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

#include <algorithm>  // for min
#include <cstring>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "mongo/platform/compiler.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/ctype.h"
#include "mongo/util/debug_util.h"

namespace mongo {

/**
 * A StringData object wraps a 'const std::string&' or a 'const char*' without copying its
 * contents. The most common usage is as a function argument that takes any of the two
 * forms of strings above. Fundamentally, this class tries go around the fact that string
 * literals in C++ are char[N]'s.
 *
 * Notes:
 *
 *  + The object StringData wraps around must be alive while the StringData is.
 *
 *  + Because std::string data can be used to pass a substring around, one should never assume a
 *    rawData() terminates with a null.
 */
class StringData {
    /** Tag used to bypass the invariant of the {c,len} constructor. */
    struct TrustedInitTag {};
    constexpr StringData(const char* c, size_t len, TrustedInitTag) : _data(c), _size(len) {}

public:
    // Declared in string_data_comparator_interface.h.
    class ComparatorInterface;

    // Iterator type
    using const_iterator = const char*;

    /** Constructs an empty StringData. */
    constexpr StringData() = default;

/**
 * Constructs a StringData, for the case where the length of the
 * string is not known. 'c' must either be NULL, or a pointer to a
 * null-terminated string.
 * Workaround for ticket SERVER-68887, it seems like a compiler bug with gcc11,
 * it has been fixed in gcc 11.3, created a ticket SERVER-69503 to update v4 gcc to 11.3.
 * we can delete 'GCC diagnostic ignored' when we change the minimum version of gcc to 11.3
 */
#if defined(__GNUC__) && (__GNUC__) >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
    StringData(const char* str) : StringData(str, str ? std::strlen(str) : 0) {}
#if defined(__GNUC__) && (__GNUC__) >= 11
#pragma GCC diagnostic pop
#endif
    /**
     * Constructs a StringData, for the case of a std::string. We can
     * use the trusted init path with no follow on checks because
     * string::data is assured to never return nullptr.
     */
    StringData(const std::string& s) : StringData(s.data(), s.length(), TrustedInitTag()) {}

    /**
     * Constructs a StringData with an explicit length. 'c' must
     * either be nullptr (in which case len must be zero), or be a
     * pointer into a character array. The StringData will refer to
     * the first 'len' characters starting at 'c'. The range of
     * characters in the half-open interval `[c, c + len)` must be valid.
     */
    constexpr StringData(const char* c, size_t len) : StringData(c, len, TrustedInitTag()) {
        if (MONGO_unlikely(kDebugBuild && !_data && (_size != 0)))
            invariant(0, "StringData(nullptr,len) requires len==0");
    }

    explicit operator std::string() const {
        return toString();
    }

    /**
     * Constructs a StringData with begin and end iterators. begin points to the beginning of the
     * string. end points to the position past the end of the string. In a null-terminated string,
     * end points to the null-terminator.
     *
     * We template the second parameter to ensure if StringData is called with 0 in the second
     * parameter, the (ptr,len) constructor is chosen instead.
     */
    template <typename T, std::enable_if_t<std::is_same_v<const char*, T>, int> = 0>
    constexpr StringData(T begin, T end) : StringData(begin, end - begin) {}

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    constexpr int compare(StringData other) const;

    /**
     * note: this uses tolower, and therefore does not handle
     *       come languages correctly.
     *       should be use sparingly
     */
    bool equalCaseInsensitive(StringData other) const;

    void copyTo(char* dest, bool includeEndingNull) const;

    constexpr StringData substr(size_t pos, size_t n = std::numeric_limits<size_t>::max()) const;

    //
    // finders
    //

    size_t find(char c, size_t fromPos = 0) const;
    size_t find(StringData needle, size_t fromPos = 0) const;
    size_t rfind(char c, size_t fromPos = std::string::npos) const;

    /**
     * Returns true if 'prefix' is a substring of this instance, anchored at position 0.
     */
    bool startsWith(StringData prefix) const;

    /**
     * Returns true if 'suffix' is a substring of this instance, anchored at the end.
     */
    bool endsWith(StringData suffix) const;

    //
    // accessors
    //

    /**
     * Get the pointer to the first byte of StringData.  This is not guaranteed to be
     * null-terminated, so if using this without checking size(), you are likely doing
     * something wrong.
     */
    constexpr const char* rawData() const noexcept {
        return _data;
    }

    constexpr size_t size() const noexcept {
        return _size;
    }
    constexpr bool empty() const {
        return size() == 0;
    }
    std::string toString() const {
        return std::string(_data, size());
    }
    constexpr char operator[](unsigned pos) const {
        return _data[pos];
    }

    //
    // iterators
    //
    constexpr const_iterator begin() const {
        return rawData();
    }
    constexpr const_iterator end() const {
        return rawData() + size();
    }

private:
    const char* _data = nullptr;  // is not guaranted to be null terminated (see "notes" above)
    size_t _size = 0;             // 'size' does not include the null terminator
};

constexpr bool operator==(StringData lhs, StringData rhs) {
    return (lhs.size() == rhs.size()) && (lhs.compare(rhs) == 0);
}

constexpr bool operator!=(StringData lhs, StringData rhs) {
    return !(lhs == rhs);
}

constexpr bool operator<(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) < 0;
}

constexpr bool operator<=(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) <= 0;
}

constexpr bool operator>(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) > 0;
}

constexpr bool operator>=(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) >= 0;
}

std::ostream& operator<<(std::ostream& stream, StringData value);

constexpr int StringData::compare(StringData other) const {
    // Note: char_traits::compare() allows nullptr arguments unlike memcmp().
    int res = std::char_traits<char>::compare(_data, other._data, std::min(_size, other._size));

    if (res != 0)
        return res > 0 ? 1 : -1;

    if (_size == other._size)
        return 0;

    return _size > other._size ? 1 : -1;
}

inline bool StringData::equalCaseInsensitive(StringData other) const {
    return size() == other.size() &&
        std::equal(begin(), end(), other.begin(), other.end(), [](char a, char b) {
               return ctype::toLower(a) == ctype::toLower(b);
           });
}

inline void StringData::copyTo(char* dest, bool includeEndingNull) const {
    if (_data)
        memcpy(dest, _data, size());
    if (includeEndingNull)
        dest[size()] = 0;
}

inline size_t StringData::find(char c, size_t fromPos) const {
    if (fromPos >= size())
        return std::string::npos;

    const void* x = memchr(_data + fromPos, c, _size - fromPos);
    if (x == nullptr)
        return std::string::npos;
    return static_cast<size_t>(static_cast<const char*>(x) - _data);
}

inline size_t StringData::find(StringData needle, size_t fromPos) const {
    size_t mx = size();
    if (fromPos > mx)
        return std::string::npos;
    size_t needleSize = needle.size();

    if (needleSize == 0)
        return fromPos;
    else if (needleSize > mx)
        return std::string::npos;

    mx -= needleSize;

    for (size_t i = fromPos; i <= mx; i++) {
        if (memcmp(_data + i, needle._data, needleSize) == 0)
            return i;
    }
    return std::string::npos;
}

inline size_t StringData::rfind(char c, size_t fromPos) const {
    const size_t sz = size();
    if (sz < 1)
        return std::string::npos;
    fromPos = std::min(fromPos, sz - 1) + 1;

    for (const char* cur = _data + fromPos; cur > _data; --cur) {
        if (*(cur - 1) == c)
            return (cur - _data) - 1;
    }
    return std::string::npos;
}

constexpr StringData StringData::substr(size_t pos, size_t n) const {
    if (pos > size())
        throw std::out_of_range("out of range");

    // truncate to end of string
    if (n > size() - pos)
        n = size() - pos;

    return StringData(_data + pos, n);
}

inline bool StringData::startsWith(StringData prefix) const {
    // TODO: Investigate an optimized implementation.
    return substr(0, prefix.size()) == prefix;
}

inline bool StringData::endsWith(StringData suffix) const {
    // TODO: Investigate an optimized implementation.
    const size_t thisSize = size();
    const size_t suffixSize = suffix.size();
    if (suffixSize > thisSize)
        return false;
    return substr(thisSize - suffixSize) == suffix;
}

inline std::string& operator+=(std::string& lhs, StringData rhs) {
    if (!rhs.empty())
        lhs.append(rhs.rawData(), rhs.size());
    return lhs;
}

inline std::string operator+(std::string lhs, StringData rhs) {
    if (!rhs.empty())
        lhs.append(rhs.rawData(), rhs.size());
    return lhs;
}

inline std::string operator+(StringData lhs, std::string rhs) {
    if (!lhs.empty())
        rhs.insert(0, lhs.rawData(), lhs.size());
    return rhs;
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
class formatter<mongo::StringData> : formatter<fmt::string_view> {
    using Base = formatter<fmt::string_view>;

public:
    using Base::parse;
    template <typename FormatContext>
    auto format(const mongo::StringData& s, FormatContext& fc) {
        return Base::format(fmt::string_view{s.rawData(), s.size()}, fc);
    }
};
}  // namespace fmt
