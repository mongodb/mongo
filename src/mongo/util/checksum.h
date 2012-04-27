#pragma once
#include "../pch.h"
namespace mongo {
    /** a simple, rather dumb, but very fast checksum.  see perftests.cpp for unit tests. */
    struct Checksum { 
        union { 
            unsigned char bytes[16];
            little_pod<unsigned long long> words[2];
        };

        // if you change this you must bump dur::CurrentVersion
        void gen(const void *buf, unsigned len) {
            wassert( ((size_t)buf) % 8 == 0 ); // performance warning
            unsigned n = len / 8 / 2;
            const little<unsigned long long> *p = &little<unsigned long long>::ref( buf );
            unsigned long long a = 0;
            for( unsigned i = 0; i < n; i++ ) {
                a += (*p ^ i);
                p++;
            }
            unsigned long long b = 0;
            for( unsigned i = 0; i < n; i++ ) {
                b += (*p ^ i);
                p++;
            }
            unsigned long long c = 0;
            for( unsigned i = n * 2 * 8; i < len; i++ ) { // 0-7 bytes left
                c = (c << 8) | ((const char *)buf)[i];
            }
            words[0] = a ^ len;
            words[1] = b ^ c;
        }

        bool operator==(const Checksum& rhs) const { return words[0]==rhs.words[0] && words[1]==rhs.words[1]; }
        bool operator!=(const Checksum& rhs) const { return words[0]!=rhs.words[0] || words[1]!=rhs.words[1]; }
    };
}
