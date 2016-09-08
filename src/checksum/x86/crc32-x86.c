/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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

#include "wt_internal.h"

#if defined(HAVE_CRC32_HARDWARE)
#if (defined(__amd64) || defined(__x86_64))
/*
 * __wt_checksum_hw --
 *	Return a checksum for a chunk of memory, computed in hardware
 *	using 8 byte steps.
 */
static uint32_t
__wt_checksum_hw(const void *chunk, size_t len)
{
	uint32_t crc;
	size_t nqwords;
	const uint8_t *p;
	const uint64_t *p64;

	crc = 0xffffffff;

	/* Checksum one byte at a time to the first 4B boundary. */
	for (p = chunk;
	    ((uintptr_t)p & (sizeof(uint32_t) - 1)) != 0 &&
	    len > 0; ++p, --len) {
		__asm__ __volatile__(
				     ".byte 0xF2, 0x0F, 0x38, 0xF0, 0xF1"
				     : "=S" (crc)
				     : "0" (crc), "c" (*p));
	}

	p64 = (const uint64_t *)p;
	/* Checksum in 8B chunks. */
	for (nqwords = len / sizeof(uint64_t); nqwords; nqwords--) {
		__asm__ __volatile__ (
				      ".byte 0xF2, 0x48, 0x0F, 0x38, 0xF1, 0xF1"
				      : "=S"(crc)
				      : "0"(crc), "c" (*p64));
		p64++;
	}

	/* Checksum trailing bytes one byte at a time. */
	p = (const uint8_t *)p64;
	for (len &= 0x7; len > 0; ++p, len--) {
		__asm__ __volatile__(
				     ".byte 0xF2, 0x0F, 0x38, 0xF0, 0xF1"
				     : "=S" (crc)
				     : "0" (crc), "c" (*p));
	}
	return (~crc);
}
#endif

#if defined(_M_AMD64)
/*
 * __wt_checksum_hw --
 *	Return a checksum for a chunk of memory, computed in hardware
 *	using 8 byte steps.
 */
static uint32_t
__wt_checksum_hw(const void *chunk, size_t len)
{
	uint32_t crc;
	size_t nqwords;
	const uint8_t *p;
	const uint64_t *p64;

	crc = 0xffffffff;

	/* Checksum one byte at a time to the first 4B boundary. */
	for (p = chunk;
	    ((uintptr_t)p & (sizeof(uint32_t) - 1)) != 0 &&
	    len > 0; ++p, --len) {
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
#endif
#endif /* HAVE_CRC32_HARDWARE */

/*
 * __wt_checksum_init --
 *	WiredTiger: detect CRC hardware and set the checksum function.
 */
void
__wt_checksum_init(void)
{
#if defined(HAVE_CRC32_HARDWARE)
#if (defined(__amd64) || defined(__x86_64))
	unsigned int eax, ebx, ecx, edx;

	__asm__ __volatile__ (
			      "cpuid"
			      : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
			      : "a" (1));

#define	CPUID_ECX_HAS_SSE42	(1 << 20)
	if (ecx & CPUID_ECX_HAS_SSE42)
		__wt_process.checksum = __wt_checksum_hw;
	else
		__wt_process.checksum = __wt_checksum_sw;

#elif defined(_M_AMD64)
	int cpuInfo[4];

	__cpuid(cpuInfo, 1);

#define	CPUID_ECX_HAS_SSE42	(1 << 20)
	if (cpuInfo[2] & CPUID_ECX_HAS_SSE42)
		__wt_process.checksum = __wt_checksum_hw;
	else
		__wt_process.checksum = __wt_checksum_sw;
#else
	__wt_process.checksum = __wt_checksum_sw;
#endif

#else /* !HAVE_CRC32_HARDWARE */
	__wt_process.checksum = __wt_checksum_sw;
#endif /* HAVE_CRC32_HARDWARE */
}
