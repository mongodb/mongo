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

namespace mongo {

    using std::string;

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
    public:

        /** Constructs an empty std::string data */
        StringData()
            : _data(NULL), _size(0) {}

        /**
         * Constructs a StringData, for the case where the length of std::string is not known. 'c'
         * must be a pointer to a null-terminated string.
         */
        StringData( const char* str )
            : _data(str), _size((str == NULL) ? 0 : std::strlen(str)) {}

        /**
         * Constructs a StringData explicitly, for the case where the length of the std::string is
         * already known. 'c' must be a pointer to a null-terminated string, and len must
         * be the length that strlen(c) would return, a.k.a the index of the terminator in c.
         */
        StringData( const char* c, size_t len )
            : _data(c), _size(len) {}

        /** Constructs a StringData, for the case of a string. */
        StringData( const std::string& s )
            : _data(s.c_str()), _size(s.size()) {}

        /**
         * Constructs a StringData explicitly, for the case of a literal whose size is known at
         * compile time.
         */
        struct LiteralTag {};
        template<size_t N>
        StringData( const char (&val)[N], LiteralTag )
            : _data(&val[0]), _size(N-1) {}

        /**
         * Returns -1, 0, or 1 if 'this' is less, equal, or greater than 'other' in
         * lexicographical order.
         */
        int compare(const StringData& other) const;

        /**
         * note: this uses tolower, and therefore does not handle
         *       come languages correctly.
         *       should be use sparingly
         */
        bool equalCaseInsensitive( const StringData& other ) const;

        void copyTo( char* dest, bool includeEndingNull ) const;

        StringData substr( size_t pos, size_t n = std::numeric_limits<size_t>::max() ) const;

        //
        // finders
        //

        size_t find( char c , size_t fromPos = 0 ) const;
        size_t find( const StringData& needle ) const;
        size_t rfind( char c, size_t fromPos = std::string::npos ) const;

        /**
         * Returns true if 'prefix' is a substring of this instance, anchored at position 0.
         */
        bool startsWith( const StringData& prefix ) const;

        /**
         * Returns true if 'suffix' is a substring of this instance, anchored at the end.
         */
        bool endsWith( const StringData& suffix ) const;

        //
        // accessors
        //

        /**
         * Get the pointer to the first byte of StringData.  This is not guaranteed to be
         * null-terminated, so if using this without checking size(), you are likely doing
         * something wrong.
         */
        const char* rawData() const { return _data; }

        size_t size() const { return _size; }
        bool empty() const { return size() == 0; }
        std::string toString() const { return std::string(_data, size()); }
        char operator[] ( unsigned pos ) const { return _data[pos]; }

        /**
         * Functor compatible with std::hash for std::unordered_{map,set}
         * Warning: The hash function is subject to change. Do not use in cases where hashes need
         *          to be consistent across versions.
         */
        struct Hasher {
            size_t operator() (const StringData& str) const;
        };

        //
        // iterators
        //

        typedef const char* const_iterator;

        const_iterator begin() const { return rawData(); }
        const_iterator end() const { return rawData() + size(); }

    private:
        const char* _data;        // is not guaranted to be null terminated (see "notes" above)
        size_t _size;     // 'size' does not include the null terminator
    };

    inline bool operator==(const StringData& lhs, const StringData& rhs) {
        return (lhs.size() == rhs.size()) && (lhs.compare(rhs) == 0);
    }

    inline bool operator!=(const StringData& lhs, const StringData& rhs) {
        return !(lhs == rhs);
    }

    inline bool operator<(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) < 0 ;
    }

    inline bool operator<=(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) <= 0;
    }

    inline bool operator>(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) > 0;
    }

    inline bool operator>=(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) >= 0;
    }

    std::ostream& operator<<(std::ostream& stream, const StringData& value);

} // namespace mongo

#include "mongo/base/string_data-inl.h"
