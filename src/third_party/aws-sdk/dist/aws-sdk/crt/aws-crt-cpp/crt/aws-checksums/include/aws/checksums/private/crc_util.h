#ifndef AWS_CHECKSUMS_PRIVATE_CRC_UTIL_H
#define AWS_CHECKSUMS_PRIVATE_CRC_UTIL_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_order.h>
#include <aws/common/stdint.h>
#include <limits.h>
#include <stdlib.h>

#define large_buffer_apply_impl(Name, T)                                                                               \
    static T aws_large_buffer_apply_##Name(                                                                            \
        T (*checksum_fn)(const uint8_t *, int, T), const uint8_t *buffer, size_t length, T previous) {                 \
        T val = previous;                                                                                              \
        while (length > INT_MAX) {                                                                                     \
            val = checksum_fn(buffer, INT_MAX, val);                                                                   \
            buffer += (size_t)INT_MAX;                                                                                 \
            length -= (size_t)INT_MAX;                                                                                 \
        }                                                                                                              \
        val = checksum_fn(buffer, (int)length, val);                                                                   \
        return val;                                                                                                    \
    }

/* helper function to reverse byte order on big-endian platforms*/
static inline uint32_t aws_bswap32_if_be(uint32_t x) {
    if (!aws_is_big_endian()) {
        return x;
    }

#if _MSC_VER
    return _byteswap_ulong(x);
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    return __builtin_bswap32(x);
#elif defined(__clang__) && __clang_major__ >= 3
    return __builtin_bswap32(x);
#else
    return (
        ((x & 0xff000000u) >> 24) | ((x & 0x00ff0000u) >> 8) | ((x & 0x0000ff00u) << 8) | ((x & 0x000000ffu) << 24));
#endif
}

/* Reverse the bytes in a 64-bit word. */
static inline uint64_t aws_bswap64_if_be(uint64_t x) {
    if (!aws_is_big_endian()) {
        return x;
    }

#if _MSC_VER
    return _byteswap_uint64(x);
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    /* Note: gcc supports it starting with 4.2. here its just picking the lowest version we run test on. */
    return __builtin_bswap64(x);
#elif defined(__clang__) && __clang_major__ >= 3
    return __builtin_bswap64(x);
#else
    return ((x << 56) & 0xff00000000000000ULL) | ((x << 40) & 0x00ff000000000000ULL) |
           ((x << 24) & 0x0000ff0000000000ULL) | ((x << 8) & 0x000000ff00000000ULL) |
           ((x >> 8) & 0x00000000ff000000ULL) | ((x >> 24) & 0x0000000000ff0000ULL) |
           ((x >> 40) & 0x000000000000ff00ULL) | ((x >> 56) & 0x00000000000000ffULL);
#endif
}

#endif /* AWS_CHECKSUMS_PRIVATE_CRC_UTIL_H */
