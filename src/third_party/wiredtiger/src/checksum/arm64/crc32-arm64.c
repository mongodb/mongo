/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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

#if defined(__linux__) && defined(HAVE_CRC32_HARDWARE)
#include <asm/hwcap.h>
#include <sys/auxv.h>

#ifndef __GNUC__
#define	__asm__	asm
#endif

#define	CRC32CX(crc,value)						\
	__asm__("crc32cx %w[c], %w[c], %x[v]" : [c]"+r"(*&crc) : [v]"r"(+value))
#define	CRC32CW(crc,value)						\
	__asm__("crc32cw %w[c], %w[c], %w[v]" : [c]"+r"(*&crc) : [v]"r"(+value))
#define	CRC32CH(crc,value)						\
	__asm__("crc32ch %w[c], %w[c], %w[v]" : [c]"+r"(*&crc) : [v]"r"(+value))
#define	CRC32CB(crc,value)						\
	__asm__("crc32cb %w[c], %w[c], %w[v]" : [c]"+r"(*&crc) : [v]"r"(+value))

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
		CRC32CB(crc, *p);
	}

	p64 = (const uint64_t *)p;
	/* Checksum in 8B chunks. */
	for (nqwords = len / sizeof(uint64_t); nqwords; nqwords--) {
		CRC32CX(crc, *p64);
		p64++;
	}

	/* Checksum trailing bytes one byte at a time. */
	p = (const uint8_t *)p64;
	for (len &= 0x7; len > 0; ++p, len--) {
		CRC32CB(crc, *p);
	}

	return (~crc);
}
#endif

/*
 * __wt_checksum_init --
 *	WiredTiger: detect CRC hardware and set the checksum function.
 */
void
__wt_checksum_init(void)
{
#if defined(__linux__) && defined(HAVE_CRC32_HARDWARE)
	unsigned long caps = getauxval(AT_HWCAP);

	if (caps & HWCAP_CRC32)
		__wt_process.checksum = __wt_checksum_hw;
	else
		__wt_process.checksum = __wt_checksum_sw;

#else
	__wt_process.checksum = __wt_checksum_sw;
#endif
}
