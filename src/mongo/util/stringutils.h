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

#pragma once

#include <ctype.h>

#include <string>
#include <vector>

#include <boost/scoped_array.hpp>

namespace mongo {

    // see also mongoutils/str.h - perhaps move these there?
    // see also text.h

    void splitStringDelim( const std::string& str , std::vector<std::string>* res , char delim );

    void joinStringDelim( const std::vector<std::string>& strs , std::string* res , char delim );

    inline std::string tolowerString( StringData input ) {
        std::string::size_type sz = input.size();

        boost::scoped_array<char> line(new char[sz+1]);
        char * copy = line.get();

        for ( std::string::size_type i=0; i<sz; i++ ) {
            char c = input[i];
            copy[i] = (char)tolower( (int)c );
        }
        copy[sz] = 0;
        return copy;
    }

    /** Functor for combining lexical and numeric comparisons. */
    class LexNumCmp {
    public:
        /** @param lexOnly - compare all characters lexically, including digits. */
        LexNumCmp( bool lexOnly );
        /**
         * Non numeric characters are compared lexicographically; numeric substrings
         * are compared numerically; dots separate ordered comparable subunits.
         * For convenience, character 255 is greater than anything else.
         * @param lexOnly - compare all characters lexically, including digits.
         */
        static int cmp( const StringData& s1, const StringData& s2, bool lexOnly );
        int cmp( const StringData& s1, const StringData& s2 ) const;
        bool operator()( const StringData& s1, const StringData& s2 ) const;
    private:
        bool _lexOnly;
    };
    
    // TODO: Sane-ify core string functionality
    // For now, this needs to be near the LexNumCmp or else
    int versionCmp(const StringData rhs, const StringData lhs);

} // namespace mongo
