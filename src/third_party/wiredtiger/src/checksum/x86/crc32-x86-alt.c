/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <wiredtiger_config.h>
/*
 * Here be dragons!
 * The intrin.h include file is in a different place for the 4.0 release of
 * MongoDB than it is for the 4.2 release. There was a bug in the 4.0 release
 * which caused checksums to be invalid if they weren't 8 byte aligned and a
 * multiple of 8 bytes long. That bug is fixed by including the intrin.h header
 * file.
 *
 * We want to keep writing the old format when running in MongoDB 4.0 so
 * users can downgrade and do point release upgrades without concern of a data
 * format changes. We also want users to be able to downgrade from 4.2 to 4.0
 * without pain. We achieve that by including this fixed version of the checksum
 * implementation. It works because when a checksum fails we automatically try
 * the alternative checksum. In the case of 4.0 the alternative is the correct
 * checksum. In the case of 4.2 the alternative is the old style checksum - i.e
 * it's important that intrin.h is included in this file in 4.0 and in the
 * non-alternate checksum implementation in 4.2.
 */
#if defined(_M_AMD64)
#include <intrin.h>
#endif
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The hardware-accelerated checksum code that originally shipped on Windows
 * did not correctly handle memory that wasn't 8B aligned and a multiple of 8B.
 * It's likely that calculations were always 8B aligned, but there's some risk.
 *
 * What we do is always write the correct checksum, and if a checksum test
 * fails, check it against the alternate version have before failing.
 */

#if defined(_M_AMD64) && !defined(HAVE_NO_CRC32_HARDWARE)
/*
 * __checksum_alt --
 *     Return a checksum for a chunk of memory, computed in hardware using 8 byte steps.
 */
static uint32_t
__checksum_alt(const void *chunk, size_t len)
{
    uint32_t crc;
    size_t nqwords;
    const uint8_t *p;
    const uint64_t *p64;

    crc = 0xffffffff;

    /* Checksum one byte at a time to the first 4B boundary. */
    for (p = chunk; ((uintptr_t)p & (sizeof(uint32_t) - 1)) != 0 && len > 0; ++p, --len) {
        crc = _mm_crc32_u8(crc, *p);
    }

    p64 = (const uint64_t *)p;
    /* Checksum in 8B chunks. */
    for (nqwords = len / sizeof(uint64_t); nqwords; nqwords--) {
        crc = (uint32_t)_mm_crc32_u64(crc, *p64);
        p64++;
    }

    /* Checksum trailing bytes one byte at a time. */
    p = (const uint8_t *)p64;
    for (len &= 0x7; len > 0; ++p, len--) {
        crc = _mm_crc32_u8(crc, *p);
    }

    return (~crc);
}

/*
 * __wt_checksum_alt_match --
 *     Return if a checksum matches the alternate calculation.
 */
bool
__wt_checksum_alt_match(const void *chunk, size_t len, uint32_t v)
{
    int cpuInfo[4];

    __cpuid(cpuInfo, 1);

#define CPUID_ECX_HAS_SSE42 (1 << 20)
    if (cpuInfo[2] & CPUID_ECX_HAS_SSE42)
        return (__checksum_alt(chunk, len) == v);

    return (false);
}

#endif
