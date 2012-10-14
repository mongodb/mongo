// bits.h

/*    Copyright 2012 10gen Inc.
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

// figure out if we're on a 64 or 32 bit system

#if defined(__x86_64__) || defined(__amd64__) || defined(_WIN64)
#define MONGO_PLATFORM_64
#elif defined(__i386__) || defined(_WIN32)
#define MONGO_PLATFORM_32
#else
#error "unknown platform"
#endif

namespace mongo {
    // defined here so can test on linux
    inline int mongo_firstBitSet( unsigned long long v ) {
        if ( v == 0 )
            return 0;
        
        for ( int i = 0; i<8; i++ ) {
            unsigned long long x = ( v >> ( 8 * i ) ) & 0xFF;
            if ( x == 0 )
                continue;

            for ( int j = 0; j < 8; j++ ) {
                if ( ( x >> j ) & 0x1 )
                    return ( i * 8 ) + j + 1;
            }
        }
        
        return 0;
    }
}


#if defined(__linux__)
#define firstBitSet ffsll
#define MONGO_SYSTEM_FFS 1
#elif defined(__MACH__) && defined(MONGO_PLATFORM_64)
#define firstBitSet ffsl
#define MONGO_SYSTEM_FFS 1
#else
#define firstBitSet mongo::mongo_firstBitSet
#endif
