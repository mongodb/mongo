/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* No instrics defined for 32-bit MSVC */
#if (defined(_M_ARM64) || defined(__aarch64__) || defined(__arm__))
#    include <aws/checksums/private/crc_priv.h>
#    ifdef _M_ARM64
#        include <arm64_neon.h>
#        define PREFETCH(p) __prefetch(p)
#    else
#        include <arm_acle.h>
#        define PREFETCH(p) __builtin_prefetch(p)
#    endif

uint32_t aws_checksums_crc32c_armv8(const uint8_t *data, int length, uint32_t previous_crc32c) {
    uint32_t crc = ~previous_crc32c;

    // Align data if it's not aligned
    while (((uintptr_t)data & 7) && length > 0) {
        crc = __crc32cb(crc, *(uint8_t *)data);
        data++;
        length--;
    }

    while (length >= 64) {
        PREFETCH(data + 384);
        uint64_t *d = (uint64_t *)data;
        crc = __crc32cd(crc, d[0]);
        crc = __crc32cd(crc, d[1]);
        crc = __crc32cd(crc, d[2]);
        crc = __crc32cd(crc, d[3]);
        crc = __crc32cd(crc, d[4]);
        crc = __crc32cd(crc, d[5]);
        crc = __crc32cd(crc, d[6]);
        crc = __crc32cd(crc, d[7]);
        data += 64;
        length -= 64;
    }

    while (length >= 8) {
        crc = __crc32cd(crc, *(uint64_t *)data);
        data += 8;
        length -= 8;
    }

    while (length > 0) {
        crc = __crc32cb(crc, *(uint8_t *)data);
        data++;
        length--;
    }

    return ~crc;
}

uint32_t aws_checksums_crc32_armv8(const uint8_t *data, int length, uint32_t previous_crc32) {
    uint32_t crc = ~previous_crc32;

    // Align data if it's not aligned
    while (((uintptr_t)data & 7) && length > 0) {
        crc = __crc32b(crc, *(uint8_t *)data);
        data++;
        length--;
    }

    while (length >= 64) {
        PREFETCH(data + 384);
        uint64_t *d = (uint64_t *)data;
        crc = __crc32d(crc, d[0]);
        crc = __crc32d(crc, d[1]);
        crc = __crc32d(crc, d[2]);
        crc = __crc32d(crc, d[3]);
        crc = __crc32d(crc, d[4]);
        crc = __crc32d(crc, d[5]);
        crc = __crc32d(crc, d[6]);
        crc = __crc32d(crc, d[7]);
        data += 64;
        length -= 64;
    }

    while (length >= 8) {
        crc = __crc32d(crc, *(uint64_t *)data);
        data += 8;
        length -= 8;
    }

    while (length > 0) {
        crc = __crc32b(crc, *(uint8_t *)data);
        data++;
        length--;
    }

    return ~crc;
}

#endif
