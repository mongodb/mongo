// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
}  // namespace mongo
