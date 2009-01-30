// md5.hpp

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

    inline std::string md5simpledigest( string s ){
        md5digest d;
        md5( s.c_str() , d );
        return digestToString( d );
    }

} // namespace mongo
