// string_data.h

/*    Copyright 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <algorithm>  // for min
#include <cstring>
#include <iosfwd>
#include <limits>
#include <stdexcept>
#include <string>

#include "mongo/stdx/type_traits.h"
#define MONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "mongo/util/invariant.h"
#undef MONGO_INCLUDE_INVARIANT_H_WHITELISTED

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
     */
    StringData(const char* str) : StringData(str, str ? std::strlen(str) : 0) {}

    /**
     * Constructs a StringData, for the case of a std::string. We can
     * use the trusted init path with no follow on checks because
     * string::data is assured to never return nullptr.
     */
    StringData(const std::string& s) : StringData(s.data(), s.length(), TrustedInitTag()) {}

    /**
     * Constructs a StringData with an explicit length. 'c' must
     * either be NULL (in which case len must be zero), or be a
     * pointer into a character array. The StringData will refer to
     * the first 'len' characters starting at 'c'. The range of
     * characters c to c+len must be valid.
     */
    StringData(const char* c, size_t len) : StringData(c, len, TrustedInitTag()) {
        invariant(_data || (_size == 0));
    }

    /**
     * Constructs a StringData from a user defined literal.  This allows
     * for constexpr creation of StringData's that are known at compile time.
     */
    constexpr friend StringData operator"" _sd(const char* c, std::size_t len);

    /**
     * Constructs a StringData with begin and end iterators. begin points to the beginning of the
     * string. end points to the position past the end of the string. In a null-terminated string,
     * end points to the null-terminator.
     *
     * We template the second parameter to ensure if StringData is called with 0 in the second
     * parameter, the (ptr,len) constructor is chosen instead.
     */
    template <
        typename InputIt,
        typename = stdx::enable_if_t<std::is_same<StringData::const_iterator, InputIt>::value>>
    StringData(InputIt begin, InputIt end) {
        invariant(begin && end);
        _data = begin;
        _size = std::distance(begin, end);
    }

    /**
     * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
     * lexicographical order.
     */
    int compare(StringData other) const;

    /**
     * note: this uses tolower, and therefore does not handle
     *       come languages correctly.
     *       should be use sparingly
     */
    bool equalCaseInsensitive(StringData other) const;

    void copyTo(char* dest, bool includeEndingNull) const;

    StringData substr(size_t pos, size_t n = std::numeric_limits<size_t>::max()) const;

    //
    // finders
    //

    size_t find(char c, size_t fromPos = 0) const;
    size_t find(StringData needle) const;
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
    const char* rawData() const {
        return _data;
    }

    size_t size() const {
        return _size;
    }
    bool empty() const {
        return size() == 0;
    }
    std::string toString() const {
        return std::string(_data, size());
    }
    char operator[](unsigned pos) const {
        return _data[pos];
    }

    /**
     * Functor compatible with std::hash for std::unordered_{map,set}
     * Warning: The hash function is subject to change. Do not use in cases where hashes need
     *          to be consistent across versions.
     */
    struct Hasher {
        size_t operator()(StringData str) const;
    };

    //
    // iterators
    //
    const_iterator begin() const {
        return rawData();
    }
    const_iterator end() const {
        return rawData() + size();
    }

private:
    const char* _data = nullptr;  // is not guaranted to be null terminated (see "notes" above)
    size_t _size = 0;             // 'size' does not include the null terminator
};

inline bool operator==(StringData lhs, StringData rhs) {
    return (lhs.size() == rhs.size()) && (lhs.compare(rhs) == 0);
}

inline bool operator!=(StringData lhs, StringData rhs) {
    return !(lhs == rhs);
}

inline bool operator<(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) < 0;
}

inline bool operator<=(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) <= 0;
}

inline bool operator>(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) > 0;
}

inline bool operator>=(StringData lhs, StringData rhs) {
    return lhs.compare(rhs) >= 0;
}

std::ostream& operator<<(std::ostream& stream, StringData value);

constexpr StringData operator"" _sd(const char* c, std::size_t len) {
    return StringData(c, len, StringData::TrustedInitTag{});
}

inline int StringData::compare(StringData other) const {
    // It is illegal to pass nullptr to memcmp. It is an invariant of
    // StringData that if _data is nullptr, _size is zero. If asked to
    // compare zero bytes, memcmp returns zero (how could they
    // differ?). So, if either StringData object has a nullptr _data
    // object, then memcmp would return zero. Achieve this by assuming
    // zero, and only calling memcmp if both pointers are valid.
    int res = 0;
    if (_data && other._data)
        res = memcmp(_data, other._data, std::min(_size, other._size));

    if (res != 0)
        return res > 0 ? 1 : -1;

    if (_size == other._size)
        return 0;

    return _size > other._size ? 1 : -1;
}

inline bool StringData::equalCaseInsensitive(StringData other) const {
    if (other.size() != size())
        return false;

    for (size_t x = 0; x < size(); x++) {
        char a = _data[x];
        char b = other._data[x];
        if (a == b)
            continue;
        if (tolower(a) == tolower(b))
            continue;
        return false;
    }

    return true;
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
    if (x == 0)
        return std::string::npos;
    return static_cast<size_t>(static_cast<const char*>(x) - _data);
}

inline size_t StringData::find(StringData needle) const {
    size_t mx = size();
    size_t needleSize = needle.size();

    if (needleSize == 0)
        return 0;
    else if (needleSize > mx)
        return std::string::npos;

    mx -= needleSize;

    for (size_t i = 0; i <= mx; i++) {
        if (memcmp(_data + i, needle._data, needleSize) == 0)
            return i;
    }
    return std::string::npos;
}

inline size_t StringData::rfind(char c, size_t fromPos) const {
    const size_t sz = size();
    if (fromPos > sz)
        fromPos = sz;

    for (const char* cur = _data + fromPos; cur > _data; --cur) {
        if (*(cur - 1) == c)
            return (cur - _data) - 1;
    }
    return std::string::npos;
}

inline StringData StringData::substr(size_t pos, size_t n) const {
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

}  // namespace mongo
