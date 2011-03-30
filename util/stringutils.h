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

    // see also mongoutils/str.h - perhaps move these there?
    // see also text.h

    void splitStringDelim( const string& str , vector<string>* res , char delim );

    void joinStringDelim( const vector<string>& strs , string* res , char delim );

    inline string tolowerString( const string& input ) {
        string::size_type sz = input.size();

        boost::scoped_array<char> line(new char[sz+1]);
        char * copy = line.get();

        for ( string::size_type i=0; i<sz; i++ ) {
            char c = input[i];
            copy[i] = (char)tolower( (int)c );
        }
        copy[sz] = 0;
        return string(copy);
    }

} // namespace mongo

#endif // UTIL_STRING_UTILS_HEADER
