// md5.hpp

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

#include "md5.h"

namespace mongo {

    typedef unsigned char md5digest[16];

    inline void md5(const void *buf, int nbytes, md5digest digest) {
        md5_state_t st;
        md5_init(&st);
        md5_append(&st, (const md5_byte_t *) buf, nbytes);
        md5_finish(&st, digest);
    }

    inline void md5(const char *str, md5digest digest) {
        md5(str, strlen(str), digest);
    }
    
    inline std::string digestToString( md5digest digest ){
        static const char * letters = "0123456789abcdef";
        stringstream ss;
        for ( int i=0; i<16; i++){
            unsigned char c = digest[i];
            ss << letters[ ( c >> 4 ) & 0xf ] << letters[ c & 0xf ];
        }
        return ss.str();
    }

    inline std::string md5simpledigest( const void* buf, int nbytes){
        md5digest d;
        md5( buf, nbytes , d );
        return digestToString( d );
    }

    inline std::string md5simpledigest( string s ){
        return md5simpledigest(s.data(), s.size());
    }


} // namespace mongo
