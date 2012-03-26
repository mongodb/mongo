// minilex.h
// mini js lexical analyzer.  idea is to be dumb and fast.

/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#error does anything use this?

namespace mongo {

#if defined(_WIN32)

} // namespace mongo

#include <hash_map>
using namespace stdext;

namespace mongo {

    typedef const char * MyStr;
    struct less_str {
        bool operator()(const MyStr & x, const MyStr & y) const {
            if ( strcmp(x, y) > 0)
                return true;

            return false;
        }
    };

    typedef hash_map<const char*, int, hash_compare<const char *, less_str> > strhashmap;

#else

} // namespace mongo

#include <ext/hash_map>

namespace mongo {

    using namespace __gnu_cxx;

    typedef const char * MyStr;
    struct eq_str {
        bool operator()(const MyStr & x, const MyStr & y) const {
            if ( strcmp(x, y) == 0)
                return true;

            return false;
        }
    };

    typedef hash_map<const char*, int, hash<const char *>, eq_str > strhashmap;

#endif

    /*
    struct MiniLexNotUsed {
        strhashmap reserved;
        bool ic[256]; // ic=Identifier Character
        bool starter[256];

        // dm: very dumb about comments and escaped quotes -- but we are faster then at least,
        // albeit returning too much (which is ok for jsbobj current usage).
        void grabVariables(char *code , strhashmap& vars) { // 'code' modified and must stay in scope*/
    char *p = code;
    char last = 0;
    while ( *p ) {
        if ( starter[*p] ) {
            char *q = p+1;
            while ( *q && ic[*q] ) q++;
            const char *identifier = p;
            bool done = *q == 0;
            *q = 0;
            if ( !reserved.count(identifier) ) {
                // we try to be smart about 'obj' but have to be careful as obj.obj
                // can happen; this is so that nFields is right for simplistic where cases
                // so we can stop scanning in jsobj when we find the field of interest.
                if ( strcmp(identifier,"obj")==0 && p>code && p[-1] != '.' )
                    ;
                else
                    vars[identifier] = 1;
            }
            if ( done )
                break;
            p = q + 1;
            continue;
        }

        if ( *p == '\'' ) {
            p++;
            while ( *p && *p != '\'' ) p++;
        }
        else if ( *p == '"' ) {
            p++;
            while ( *p && *p != '"' ) p++;
        }
        p++;
    }
}

MiniLex() {
    strhashmap atest;
    atest["foo"] = 3;
    verify( atest.count("bar") == 0 );
    verify( atest.count("foo") == 1 );
    verify( atest["foo"] == 3 );

    for ( int i = 0; i < 256; i++ ) {
        ic[i] = starter[i] = false;
    }
    for ( int i = 'a'; i <= 'z'; i++ )
        ic[i] = starter[i] = true;
    for ( int i = 'A'; i <= 'Z'; i++ )
        ic[i] = starter[i] = true;
    for ( int i = '0'; i <= '9'; i++ )
        ic[i] = true;
    for ( int i = 128; i < 256; i++ )
        ic[i] = starter[i] = true;
    ic['$'] = starter['$'] = true;
    ic['_'] = starter['_'] = true;

    reserved["break"] = true;
    reserved["case"] = true;
    reserved["catch"] = true;
    reserved["continue"] = true;
    reserved["default"] = true;
    reserved["delete"] = true;
    reserved["do"] = true;
    reserved["else"] = true;
    reserved["finally"] = true;
    reserved["for"] = true;
    reserved["function"] = true;
    reserved["if"] = true;
    reserved["in"] = true;
    reserved["instanceof"] = true;
    reserved["new"] = true;
    reserved["return"] = true;
    reserved["switch"] = true;
    reserved["this"] = true;
    reserved["throw"] = true;
    reserved["try"] = true;
    reserved["typeof"] = true;
    reserved["var"] = true;
    reserved["void"] = true;
    reserved["while"] = true;
    reserved["with "] = true;
}
};
*/

} // namespace mongo
