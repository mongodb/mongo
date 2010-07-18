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

#ifndef BSON_STRINDATA_HEADER
#define BSON_STRINDATA_HEADER

#include <string>
#include <cstring>

namespace mongo {

    using std::string;

    class StringData {
    public:
        StringData( const char* c ) 
            : _data(c), _size((unsigned) strlen(c)) {}

        StringData( const string& s )
            : _data(s.c_str()), _size((unsigned) s.size()) {}
        
        struct LiteralTag {};
        template<size_t N>
        StringData( const char (&val)[N], LiteralTag )
            : _data(&val[0]), _size(N-1) {}

        const char* const data() const { return _data; }
        const unsigned size() const { return _size; }

    private:
        // TODO - Hook this class up in the BSON machinery
        // There are two assumptions here that we may want to review then.
        // '_data' *always* finishes with a null terminator
        // 'size' does *not* account for the null terminator
        // These assumptions may make it easier to minimize changes to existing code
        const char* const _data;
        const unsigned    _size;
    };

} // namespace mongo

#endif  // BSON_STRINGDATA_HEADER
