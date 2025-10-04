/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/util/modules.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace MONGO_MOD_PUB mongo {

/**
 * Returns the number of leading 0-bits in num. Returns 64 if num is 0.
 */
inline int countLeadingZeros64(unsigned long long num);

/**
 * Returns the number of leading 0-bits in num. The result is undefined if num is 0.
 */
inline int countLeadingZerosNonZero64(unsigned long long num);

/**
 * Returns the number of trailing 0-bits in num. Returns 64 if num is 0.
 */
inline int countTrailingZeros64(unsigned long long num);

/**
 * Returns the number of trailing 0-bits in num. The result is undefined if num is 0.
 */
inline int countTrailingZerosNonZero64(unsigned long long num);

/**
 * Same as above, but for int.
 */
inline int countTrailingZerosNonZero32(unsigned int num);

#if defined(__GNUC__)
int countLeadingZeros64(unsigned long long num) {
    if (num == 0)
        return 64;
    return __builtin_clzll(num);
}

int countLeadingZerosNonZero64(unsigned long long num) {
    return __builtin_clzll(num);
}

int countTrailingZeros64(unsigned long long num) {
    if (num == 0)
        return 64;
    return __builtin_ctzll(num);
}

int countTrailingZerosNonZero64(unsigned long long num) {
    return __builtin_ctzll(num);
}

int countTrailingZerosNonZero32(unsigned int num) {
    return __builtin_ctz(num);
}

#elif defined(_MSC_VER) && defined(_WIN64)
int countLeadingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanReverse64(&out, num))
        return 63 ^ out;
    return 64;
}

int countLeadingZerosNonZero64(unsigned long long num) {
    unsigned long out;
    _BitScanReverse64(&out, num);
    return 63 ^ out;
}

int countTrailingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanForward64(&out, num))
        return out;
    return 64;
}

int countTrailingZerosNonZero64(unsigned long long num) {
    unsigned long out;
    _BitScanForward64(&out, num);
    return out;
}

int countTrailingZerosNonZero32(unsigned int num) {
    unsigned long out;
    _BitScanForward(&out, static_cast<unsigned long>(num));
    return out;
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

int countLeadingZerosNonZero64(unsigned long long num) {
    unsigned long out;
    if (_BitScanReverse(&out, static_cast<unsigned long>(num >> 32)))
        return 31 ^ out;
    _BitScanReverse(&out, static_cast<unsigned long>(num));
    return 63 ^ out;
}

int countTrailingZeros64(unsigned long long num) {
    unsigned long out;
    if (_BitScanForward(&out, static_cast<unsigned long>(num)))
        return out;
    if (_BitScanForward(&out, static_cast<unsigned long>(num >> 32)))
        return out + 32;
    return 64;
}

int countTrailingZerosNonZero64(unsigned long long num) {
    unsigned long out;
    if (_BitScanForward(&out, static_cast<unsigned long>(num)))
        return out;

    _BitScanForward(&out, static_cast<unsigned long>(num >> 32));
    return out + 32;
}

int countTrailingZerosNonZero32(unsigned int num) {
    unsigned long out;
    _BitScanForward(&out, static_cast<unsigned long>(num));
    return out;
}
#else
#error "No bit-ops definitions for your platform"
#endif
}  // namespace MONGO_MOD_PUB mongo
