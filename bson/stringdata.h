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

#ifndef UTIL_BSON_STRINGDATA_HEADER
#define UTIL_BSON_STRINGDATA_HEADER

#include <string>
#include <cstring>

namespace mongo {

    using std::string;

    struct StringData {
        // TODO - Hook this class up in the BSON machinery
        // There are two assumptions here that we may want to review then.
        // 'data' *always* finishes with a null terminator
        // 'size' does *not* account for the null terminator
        // These assumptions may make it easier to minimize changes to existing code
        const char*    data;
        const unsigned size;

        StringData( const char* c ) 
            : data(c), size(strlen(c)) {}

        StringData( const string& s )
            : data(s.c_str()), size(s.size()) {}
        
        struct LiteralTag {};
        template<size_t N>
        StringData( const char (&val)[N], LiteralTag )
            : data(&val[0]), size(N-1) {}
    };

} // namespace mongo

#endif  // UTIL_BSON_STRINGDATA_HEADER
