// bits.h

/*    Copyright 2012 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

// figure out if we're on a 64 or 32 bit system

#if defined(__x86_64__) || defined(__amd64__) || defined(_WIN64) || defined(__aarch64__) || defined(__powerpc64__)
#define MONGO_PLATFORM_64
#elif defined(__i386__) || defined(_WIN32) || defined(__arm__)
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
