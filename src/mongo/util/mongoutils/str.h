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
   and no size_t's.  If you need 3 gigabyte long strings, don't use this module.
*/

#include <string>
#include <sstream>

// this violates the README rules for mongoutils:
#include "../../bson/util/builder.h"

namespace mongoutils {

    namespace str {

        /** the idea here is to make one liners easy.  e.g.:

               return str::stream() << 1 << ' ' << 2;

            since the following doesn't work:

               (stringstream() << 1).str();
        */
        class stream {
        public:
            mongo::StringBuilder ss;
            template<class T>
            stream& operator<<(const T& v) {
                ss << v;
                return *this;
            }
            operator std::string () const { return ss.str(); }
        };

        inline bool startsWith(const char *str, const char *prefix) {
            const char *s = str;
            const char *p = prefix;
            while( *p ) {
                if( *p != *s ) return false;
                p++; s++;
            }
            return true;
        }
        inline bool startsWith(const std::string& s, const std::string& p) {
            return startsWith(s.c_str(), p.c_str());
        }

        // while these are trivial today use in case we do different wide char things later
        inline bool startsWith(const char *p, char ch) { return *p == ch; }
        inline bool startsWith(const std::string& s, char ch) { return startsWith(s.c_str(), ch); }

        inline bool endsWith(const std::string& s, const std::string& p) {
            int l = p.size();
            int x = s.size();
            if( x < l ) return false;
            return strncmp(s.c_str()+x-l, p.c_str(), l) == 0;
        }
        inline bool endsWith(const char *s, char p) {
            size_t len = strlen(s);
            return len && s[len-1] == p;
        }

        inline bool equals( const char * a , const char * b ) { return strcmp( a , b ) == 0; }

        /** find char x, and return rest of string thereafter, or "" if not found */
        inline const char * after(const char *s, char x) {
            const char *p = strchr(s, x);
            return (p != 0) ? p+1 : "";
        }
        inline std::string after(const std::string& s, char x) {
            const char *p = strchr(s.c_str(), x);
            return (p != 0) ? std::string(p+1) : "";
        }

        /** find string x, and return rest of string thereafter, or "" if not found */
        inline const char * after(const char *s, const char *x) {
            const char *p = strstr(s, x);
            return (p != 0) ? p+strlen(x) : "";
        }
        inline std::string after(const std::string& s, const std::string& x) {
            const char *p = strstr(s.c_str(), x.c_str());
            return (p != 0) ? std::string(p+x.size()) : "";
        }

        /** @return true if s contains x
         *  These should not be used with strings containing NUL bytes
         */
        inline bool contains(const std::string& s, const std::string& x) {
            return strstr(s.c_str(), x.c_str()) != 0;
        }
        inline bool contains(const std::string& s, char x) {
            verify(x != '\0'); // this expects c-strings so don't use when looking for NUL bytes
            return strchr(s.c_str(), x) != 0;
        }

        /** @return everything before the character x, else entire string */
        inline std::string before(const std::string& s, char x) {
            const char *p = strchr(s.c_str(), x);
            return (p != 0) ? s.substr(0, p-s.c_str()) : s;
        }

        /** @return everything before the string x, else entire string */
        inline std::string before(const std::string& s, const std::string& x) {
            const char *p = strstr(s.c_str(), x.c_str());
            return (p != 0) ? s.substr(0, p-s.c_str()) : s;
        }

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
            return ofs;
        }
        inline int shareCommonPrefix(const std::string &a, const std::string &b)
        { return shareCommonPrefix(a.c_str(), b.c_str()); }

        /** string to unsigned. zero if not a number. can end with non-num chars */
        inline unsigned toUnsigned(const std::string& a) {
            unsigned x = 0;
            const char *p = a.c_str();
            while( 1 ) {
                if( !isdigit(*p) )
                    break;
                x = x * 10 + (*p - '0');
                p++;
            }
            return x;
        }

        /** split a string on a specific char.  We don't split N times, just once
            on the first occurrence.  If char not present entire string is in L
            and R is empty.
            @return true if char found
        */
        inline bool splitOn(const std::string &s, char c, std::string& L, std::string& R) {
            const char *start = s.c_str();
            const char *p = strchr(start, c);
            if( p == 0 ) {
                L = s; R.clear();
                return false;
            }
            L = std::string(start, p-start);
            R = std::string(p+1);
            return true;
        }
        /** split scanning reverse direction. Splits ONCE ONLY. */
        inline bool rSplitOn(const std::string &s, char c, std::string& L, std::string& R) {
            const char *start = s.c_str();
            const char *p = strrchr(start, c);
            if( p == 0 ) {
                L = s; R.clear();
                return false;
            }
            L = std::string(start, p-start);
            R = std::string(p+1);
            return true;
        }

        /** @return number of occurrences of c in s */
        inline unsigned count( const std::string& s , char c ) {
            unsigned n=0;
            for ( unsigned i=0; i<s.size(); i++ )
                if ( s[i] == c )
                    n++;
            return n;
        }

        /** trim leading spaces. spaces only, not tabs etc. */
        inline std::string ltrim(const std::string& s) {
            const char *p = s.c_str();
            while( *p == ' ' ) p++;
            return p;
        }

        /** remove trailing chars in place */
        inline void stripTrailing(std::string& s, const char *chars) {
            std::string::iterator i = s.end();
            while( s.begin() != i ) {
                i--;
                if( contains(chars, *i) ) {
                    s.erase(i);
                }
            }
        }

    }

}
