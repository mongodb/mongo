// @file str.h

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

/* Things in the mongoutils namespace 
   (1) are not database specific, rather, true utilities
   (2) are cross platform
   (3) may require boost headers, but not libs
   (4) are clean and easy to use in any c++ project without pulling in lots of other stuff

   Note: within this module, we use int for all offsets -- there are no unsigned offsets 
   and no size_t's.  If you need 3 gigabyte long strings, don't use this module. */

#include <string>

namespace mongoutils {

    namespace str {

        using namespace std;

        /** find char x, and return rest of string thereafter, or "" if not found */
        inline const char * after(const char *s, char x) {
            const char *p = strchr(s, x);
            return (p != 0) ? p+1 : ""; }
        inline string after(const string& s, char x) {
            const char *p = strchr(s.c_str(), x);
            return (p != 0) ? string(p+1) : ""; }

        /** @return everything befor the character x, else entire string */
        inline string before(const string& s, char x) {
            const char *p = strchr(s.c_str(), x);
            return (p != 0) ? s.substr(0, p-s.c_str()) : s; }

        /** check if if strings share a common starting prefix 
            @return offset of divergence (or length if equal).  0=nothing in common. */
        inline int shareCommonPrefix(const char *p, const char *q) {
            int ofs = 0;
            while( 1 ) {
                if( *p == 0 || *q == 0 )
                    break;
                if( *p != *q )
                    break;
                p++; q++; ofs++;
            }
            return ofs; }
        inline int shareCommonPrefix(const string &a, const string &b)
        { return shareCommonPrefix(a.c_str(), b.c_str()); }

    }

}
