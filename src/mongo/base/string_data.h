// string_data.h

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
     * A StringData object wraps a 'const string&' or a 'const char*' without copying its
     * contents. The most common usage is as a function argument that takes any of the two
     * forms of strings above. Fundamentally, this class tries go around the fact that string
     * literals in C++ are char[N]'s.
     *
     * Notes:
     *
     *  + The object StringData wraps around must be alive while the StringData is.
     *
     *  + Because string data can be used to pass a substring around, one should never assume a
     *    rawData() terminates with a null.
     */
    class StringData {
    public:

        /** Constructs an empty string data */
        StringData()
            : _data(NULL), _size(0) {}

        /**
         * Constructs a StringData, for the case where the length of string is not known. 'c'
         * must be a pointer to a null-terminated string.
         */
        StringData( const char* c )
            : _data(c), _size((c == NULL) ? 0 : string::npos) {}

        /**
         * Constructs a StringData explicitly, for the case where the length of the string is
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
        size_t rfind( char c, size_t fromPos = string::npos ) const;

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

        size_t size() const { fillSize(); return _size; }
        bool empty() const { return size() == 0; }
        string toString() const { return string(_data, size()); }
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
        mutable size_t _size;     // 'size' does not include the null terminator

        void fillSize() const {
            if (_size == string::npos) {
                _size = strlen(_data);
            }
        }
    };

    inline bool operator==(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) == 0;
    }

    inline bool operator!=(const StringData& lhs, const StringData& rhs) {
        return lhs.compare(rhs) != 0;
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
