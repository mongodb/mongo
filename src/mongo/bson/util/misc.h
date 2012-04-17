/* @file misc.h 
*/

/*
 *    Copyright 2009 10gen Inc.
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

#include <ctime>
#include <limits>

namespace mongo {

    inline void time_t_to_String(time_t t, char *buf) {
#if defined(_WIN32)
        ctime_s(buf, 32, &t);
#else
        ctime_r(&t, buf);
#endif
        buf[24] = 0; // don't want the \n
    }

    inline std::string time_t_to_String(time_t t = time(0) ) {
        char buf[64];
#if defined(_WIN32)
        ctime_s(buf, sizeof(buf), &t);
#else
        ctime_r(&t, buf);
#endif
        buf[24] = 0; // don't want the \n
        return buf;
    }

    inline std::string time_t_to_String_no_year(time_t t) {
        char buf[64];
#if defined(_WIN32)
        ctime_s(buf, sizeof(buf), &t);
#else
        ctime_r(&t, buf);
#endif
        buf[19] = 0;
        return buf;
    }

    inline std::string time_t_to_String_short(time_t t) {
        char buf[64];
#if defined(_WIN32)
        ctime_s(buf, sizeof(buf), &t);
#else
        ctime_r(&t, buf);
#endif
        buf[19] = 0;
        if( buf[0] && buf[1] && buf[2] && buf[3] )
            return buf + 4; // skip day of week
        return buf;
    }

    struct Date_t {
        // TODO: make signed (and look for related TODO's)
        unsigned long long millis;
        Date_t(): millis(0) {}
        Date_t(unsigned long long m): millis(m) {}
        operator unsigned long long&() { return millis; }
        operator const unsigned long long&() const { return millis; }
        void toTm (tm *buf) {
            time_t dtime = toTimeT();
#if defined(_WIN32)
            gmtime_s(buf, &dtime);
#else
            gmtime_r(&dtime, buf);
#endif
        }
        std::string toString() const {
            char buf[64];
            time_t_to_String(toTimeT(), buf);
            return buf;
        }
        time_t toTimeT() const {
            // cant use uassert from bson/util
            verify((long long)millis >= 0); // TODO when millis is signed, delete 
            verify(((long long)millis/1000) < (std::numeric_limits<time_t>::max)());
            return millis / 1000;
        }
    };

    /*
    template<> struct storage_type<Date_t> {
        typedef unsigned long long t;

        static t toStorage( Date_t src ) { return src; }
        static Date_t fromStorage( t stored ) { return stored; }

    };
    */


    // Like strlen, but only scans up to n bytes.
    // Returns -1 if no '0' found.
    inline int strnlen( const char *s, int n ) {
        for( int i = 0; i < n; ++i )
            if ( !s[ i ] )
                return i;
        return -1;
    }

    inline bool isNumber( char c ) {
        return c >= '0' && c <= '9';
    }

    inline unsigned stringToNum(const char *str) {
        unsigned x = 0;
        const char *p = str;
        while( 1 ) {
            if( !isNumber(*p) ) {
                if( *p == 0 && p != str )
                    break;
                throw 0;
            }
            x = x * 10 + *p++ - '0';
        }
        return x;
    }

}
