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
#include <string>

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
    StringData(const char* c, size_t len, TrustedInitTag) : _data(c), _size(len) {}

public:
    // Declared in string_data_comparator_interface.h.
    class ComparatorInterface;

    /** Constructs an empty StringData. */
    StringData() = default;

    /**
     * Constructs a StringData, for the case where the length of the
     * string is not known. 'c' must either be NULL, or a pointer to a
     * null-terminated string.
     */
    StringData(const char* str) : StringData(str, str ? std::strlen(str) : 0) {}

    /**
     * Constructs a StringData explicitly, for the case of a literal
     * whose size is known at compile time. Note that you probably
     * don't need this on a modern compiler that can see that the call
     * to std::strlen on StringData("foo") can be constexpr'ed out.
     */
    struct LiteralTag {};
    template <size_t N>
    StringData(const char(&val)[N], LiteralTag)
        : StringData(&val[0], N - 1) {}

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

    typedef const char* const_iterator;

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

}  // namespace mongo

#include "mongo/base/string_data-inl.h"
