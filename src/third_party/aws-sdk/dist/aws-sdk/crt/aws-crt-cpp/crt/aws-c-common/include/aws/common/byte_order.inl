#ifndef AWS_COMMON_BYTE_ORDER_INL
#define AWS_COMMON_BYTE_ORDER_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_order.h>
#include <aws/common/common.h>

#ifdef _WIN32
#    include <stdlib.h>
#else
#    include <netinet/in.h>
#endif /* _MSC_VER */

AWS_EXTERN_C_BEGIN

/**
 * Returns 1 if machine is big endian, 0 if little endian.
 * If you compile with even -O1 optimization, this check is completely optimized
 * out at compile time and code which calls "if (aws_is_big_endian())" will do
 * the right thing without branching.
 */
AWS_STATIC_IMPL int aws_is_big_endian(void) {
    const uint16_t z = 0x100;
    return *(const uint8_t *)&z;
}

/**
 * Convert 64 bit integer from host to network byte order.
 */
AWS_STATIC_IMPL uint64_t aws_hton64(uint64_t x) {
    if (aws_is_big_endian()) {
        return x;
    }
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(CBMC)
    uint64_t v;
    __asm__("bswap %q0" : "=r"(v) : "0"(x));
    return v;
#elif defined(_MSC_VER)
    return _byteswap_uint64(x);
#else
    uint32_t low = x & UINT32_MAX;
    uint32_t high = (uint32_t)(x >> 32);
    return ((uint64_t)htonl(low)) << 32 | htonl(high);
#endif
}

/**
 * Convert 64 bit integer from network to host byte order.
 */
AWS_STATIC_IMPL uint64_t aws_ntoh64(uint64_t x) {
    return aws_hton64(x);
}

/**
 * Convert 32 bit integer from host to network byte order.
 */
AWS_STATIC_IMPL uint32_t aws_hton32(uint32_t x) {
#ifdef _WIN32
    return aws_is_big_endian() ? x : _byteswap_ulong(x);
#else
    return htonl(x);
#endif
}

/**
 * Convert 32 bit float from host to network byte order.
 */
AWS_STATIC_IMPL float aws_htonf32(float x) {
    if (aws_is_big_endian()) {
        return x;
    }

    uint8_t *f_storage = (uint8_t *)&x;

    float ret_value;
    uint8_t *ret_storage = (uint8_t *)&ret_value;

    ret_storage[0] = f_storage[3];
    ret_storage[1] = f_storage[2];
    ret_storage[2] = f_storage[1];
    ret_storage[3] = f_storage[0];

    return ret_value;
}

/**
 * Convert 64 bit double from host to network byte order.
 */
AWS_STATIC_IMPL double aws_htonf64(double x) {
    if (aws_is_big_endian()) {
        return x;
    }

    uint8_t *f_storage = (uint8_t *)&x;

    double ret_value;
    uint8_t *ret_storage = (uint8_t *)&ret_value;

    ret_storage[0] = f_storage[7];
    ret_storage[1] = f_storage[6];
    ret_storage[2] = f_storage[5];
    ret_storage[3] = f_storage[4];
    ret_storage[4] = f_storage[3];
    ret_storage[5] = f_storage[2];
    ret_storage[6] = f_storage[1];
    ret_storage[7] = f_storage[0];

    return ret_value;
}

/**
 * Convert 32 bit integer from network to host byte order.
 */
AWS_STATIC_IMPL uint32_t aws_ntoh32(uint32_t x) {
#ifdef _WIN32
    return aws_is_big_endian() ? x : _byteswap_ulong(x);
#else
    return ntohl(x);
#endif
}

/**
 * Convert 32 bit float from network to host byte order.
 */
AWS_STATIC_IMPL float aws_ntohf32(float x) {
    return aws_htonf32(x);
}

/**
 * Convert 32 bit float from network to host byte order.
 */
AWS_STATIC_IMPL double aws_ntohf64(double x) {
    return aws_htonf64(x);
}

/**
 * Convert 16 bit integer from host to network byte order.
 */
AWS_STATIC_IMPL uint16_t aws_hton16(uint16_t x) {
#ifdef _WIN32
    return aws_is_big_endian() ? x : _byteswap_ushort(x);
#else
    return htons(x);
#endif
}

/**
 * Convert 16 bit integer from network to host byte order.
 */
AWS_STATIC_IMPL uint16_t aws_ntoh16(uint16_t x) {
#ifdef _WIN32
    return aws_is_big_endian() ? x : _byteswap_ushort(x);
#else
    return ntohs(x);
#endif
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_BYTE_ORDER_INL */
