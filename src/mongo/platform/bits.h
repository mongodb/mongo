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

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// figure out if we're on a 64 or 32 bit system

#if defined(__x86_64__) || defined(__amd64__) || defined(_WIN64) || defined(__aarch64__) || \
    defined(__powerpc64__) || defined(__s390x__) || defined(__sparcv9)
#define MONGO_PLATFORM_64
#elif defined(__i386__) || defined(_WIN32) || defined(__arm__) || defined(__sparc__)
#define MONGO_PLATFORM_32
#else
#error "unknown platform"
#endif

namespace mongo {

/**
 * Returns the number of leading 0-bits in num. Returns 64 if num is 0.
 */
inline int countLeadingZeros64(unsigned long long num);

/**
 * Returns the number of trailing 0-bits in num. Returns 64 if num is 0.
 */
inline int countTrailingZeros64(unsigned long long num);


#if defined(__GNUC__)
int countLeadingZeros64(unsigned long long num) {
    if (num == 0)
        return 64;
    return __builtin_clzll(num);
}

int countTrailingZeros64(unsigned long long num) {
    if (num == 0)
        return 64;
    return __builtin_ctzll(num);
}
#elif defined(_MSC_VER) && defined(_WIN64)
int countLeadingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanReverse64(&out, num))
        return 63 ^ out;
    return 64;
}

int countTrailingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanForward64(&out, num))
        return out;
    return 64;
}
#elif defined(_MSC_VER) && defined(_WIN32)
int countLeadingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanReverse(&out, static_cast<unsigned long>(num >> 32)))
        return 31 ^ out;
    if (_BitScanReverse(&out, static_cast<unsigned long>(num)))
        return 63 ^ out;
    return 64;
}

int countTrailingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanForward(&out, static_cast<unsigned long>(num)))
        return out;
    if (_BitScanForward(&out, static_cast<unsigned long>(num >> 32)))
        return out + 32;
    return 64;
}
#else
#error "No bit-ops definitions for your platform"
#endif
}
