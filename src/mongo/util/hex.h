// util/hex.h

/*    Copyright 2009 10gen Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder.h"

namespace mongo {
    //can't use hex namespace because it conflicts with hex iostream function
    inline int fromHex( char c ) {
        if ( '0' <= c && c <= '9' )
            return c - '0';
        if ( 'a' <= c && c <= 'f' )
            return c - 'a' + 10;
        if ( 'A' <= c && c <= 'F' )
            return c - 'A' + 10;
        verify( false );
        return 0xff;
    }
    inline char fromHex( const char *c ) {
        return (char)(( fromHex( c[ 0 ] ) << 4 ) | fromHex( c[ 1 ] ));
    }
    inline char fromHex( const StringData& c ) {
        return (char)(( fromHex( c[ 0 ] ) << 4 ) | fromHex( c[ 1 ] ));
    }

    inline std::string toHex(const void* inRaw, int len) {
        static const char hexchars[] = "0123456789ABCDEF";

        StringBuilder out;
        const char* in = reinterpret_cast<const char*>(inRaw);
        for (int i=0; i<len; ++i) {
            char c = in[i];
            char hi = hexchars[(c & 0xF0) >> 4];
            char lo = hexchars[(c & 0x0F)];

            out << hi << lo;
        }

        return out.str();
    }

    template <typename T> std::string integerToHex(T val);

    inline std::string toHexLower(const void* inRaw, int len) {
        static const char hexchars[] = "0123456789abcdef";

        StringBuilder out;
        const char* in = reinterpret_cast<const char*>(inRaw);
        for (int i=0; i<len; ++i) {
            char c = in[i];
            char hi = hexchars[(c & 0xF0) >> 4];
            char lo = hexchars[(c & 0x0F)];

            out << hi << lo;
        }

        return out.str();
    }
}
