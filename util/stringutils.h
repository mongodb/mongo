// stringutils.h

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

#ifndef UTIL_STRING_UTILS_HEADER
#define UTIL_STRING_UTILS_HEADER

namespace mongo {

    void splitStringDelim( const string& str , vector<string>* res , char delim );

    void joinStringDelim( const vector<string>& strs , string* res , char delim );

    struct StringData {
        const char* data;
        unsigned    size;

        StringData( const char * c ){
            data = c;
            size = strlen(c);
        }

        StringData( const string& s ){
            data = s.c_str();
            size = s.size();
        }
    };
   
} // namespace mongo

#endif // UTIL_STRING_UTILS_HEADER
