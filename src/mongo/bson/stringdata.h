// stringdata.h

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

#include <string>
#include <cstring>

namespace mongo {

    using std::string;

    /** A StringData object wraps a 'const string&' or a 'const char*' without
     * copying its contents. The most common usage is as a function argument that
     * takes any of the two forms of strings above. Fundamentally, this class tries
     * go around the fact that string literals in C++ are char[N]'s.
     *
     * Note that the object StringData wraps around must be alive while the StringData
     * is.
    */
    class StringData {
    public:
        /** Construct a StringData, for the case where the length of
         * string is not known. 'c' must be a pointer to a null-terminated string.
         */
        StringData( const char* c )
            : _data(c), _size((unsigned) strlen(c)) {}

        /** Construct a StringData explicitly, for the case where the length of the string
         * is already known. 'c' must be a pointer to a null-terminated string, and strlenOfc
         * must be the length that strlen(c) would return, a.k.a the index of the
         * terminator in c.
         */
        StringData( const char* c, unsigned len )
            : _data(c), _size(len) {}

        /** Construct a StringData, for the case of a string. */
        StringData( const std::string& s )
            : _data(s.c_str()), _size((unsigned) s.size()) {}

        // Construct a StringData explicitly, for the case of a literal whose size is
        // known at compile time.
        struct LiteralTag {};
        template<size_t N>
        StringData( const char (&val)[N], LiteralTag )
            : _data(&val[0]), _size(N-1) {}

        // accessors
        const char* data() const { return _data; }
        unsigned size() const { return _size; }

    private:
        const char* const _data;  // is always null terminated
        const unsigned    _size;  // 'size' does not include the null terminator
    };

} // namespace mongo
