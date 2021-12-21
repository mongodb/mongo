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

/*-
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Paul Vixie.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/sys/bitstring.h,v 1.5 2005/01/07 02:29:23 imp Exp $
 */

				/* byte of the bitstring bit is in */
#define	__bit_byte(bit)	((bit) >> 3)

				/* mask for the bit within its byte */
#define	__bit_mask(bit)	(1 << ((bit) & 0x7))

				/* Bytes in a bitstring of nbits */
#define	__bitstr_size(nbits) (((nbits) + 7) >> 3)

/*
 * __bit_alloc --
 *	Allocate a bitstring.
 */
static inline int
__bit_alloc(WT_SESSION_IMPL *session, uint64_t nbits, void *retp)
{
	return (__wt_calloc(
	    session, (size_t)__bitstr_size(nbits), sizeof(uint8_t), retp));
}

/*
 * __bit_test --
 *	Test one bit in name.
 */
static inline bool
__bit_test(uint8_t *bitf, uint64_t bit)
{
	return ((bitf[__bit_byte(bit)] & __bit_mask(bit)) != 0);
}

/*
 * __bit_set --
 *	Set one bit in name.
 */
static inline void
__bit_set(uint8_t *bitf, uint64_t bit)
{
	bitf[__bit_byte(bit)] |= __bit_mask(bit);
}

/*
 * __bit_clear --
 *	Clear one bit in name.
 */
static inline void
__bit_clear(uint8_t *bitf, uint64_t bit)
{
	bitf[__bit_byte(bit)] &= ~__bit_mask(bit);
}

/*
 * __bit_nclr --
 *	Clear bits start-to-stop in name.
 */
static inline void
__bit_nclr(uint8_t *bitf, uint64_t start, uint64_t stop)
{
	uint64_t startbyte, stopbyte;

	startbyte = __bit_byte(start);
	stopbyte = __bit_byte(stop);

	if (startbyte == stopbyte)
		bitf[startbyte] &=
		    ((0xff >> (8 - (start & 0x7))) |
		    (0xff << ((stop & 0x7) + 1)));
	else {
		bitf[startbyte] &= 0xff >> (8 - (start & 0x7));
		while (++startbyte < stopbyte)
			bitf[startbyte] = 0;
		bitf[stopbyte] &= 0xff << ((stop & 0x7) + 1);
	}
}

/*
 * __bit_nset --
 *	Set bits start-to-stop in name.
 */
static inline void
__bit_nset(uint8_t *bitf, uint64_t start, uint64_t stop)
{
	uint64_t startbyte, stopbyte;

	startbyte = __bit_byte(start);
	stopbyte = __bit_byte(stop);
	if (startbyte == stopbyte)
		bitf[startbyte] |=
		    ((0xff << (start & 0x7)) & (0xff >> (7 - (stop & 0x7))));
	else {
		bitf[startbyte] |= 0xff << (start & 0x7);
		while (++startbyte < stopbyte)
			bitf[startbyte] = 0xff;
		bitf[stopbyte] |= 0xff >> (7 - (stop & 0x7));
	}
}

/*
 * __bit_ffc --
 *	Find first clear bit in name, return 0 on success, -1 on no bit clear.
 */
static inline int
__bit_ffc(uint8_t *bitf, uint64_t nbits, uint64_t *retp)
{
	uint64_t byte, stopbyte, value;
	uint8_t lb;

	if (nbits == 0)
		return (-1);

	for (byte = 0, stopbyte = __bit_byte(nbits - 1);; ++byte) {
		if (bitf[byte] != 0xff) {
			value = byte << 3;
			for (lb = bitf[byte]; lb & 0x01; ++value, lb >>= 1)
				;
			break;
		}
		if (byte == stopbyte)
			return (-1);
	}

	if (value >= nbits)
		return (-1);

	*retp = value;
	return (0);
}

/*
 * __bit_ffs --
 *	Find first set bit in name, return 0 on success, -1 on no bit set.
 */
static inline int
__bit_ffs(uint8_t *bitf, uint64_t nbits, uint64_t *retp)
{
	uint64_t byte, stopbyte, value;
	uint8_t lb;

	if (nbits == 0)
		return (-1);

	for (byte = 0, stopbyte = __bit_byte(nbits - 1);; ++byte) {
		if (bitf[byte] != 0) {
			value = byte << 3;
			for (lb = bitf[byte]; !(lb & 0x01); ++value, lb >>= 1)
				;
			break;
		}
		if (byte == stopbyte)
			return (-1);
	}

	if (value >= nbits)
		return (-1);

	*retp = value;
	return (0);
}

/*
 * __bit_getv --
 *	Return a fixed-length column store bit-field value.
 */
static inline uint8_t
__bit_getv(uint8_t *bitf, uint64_t entry, uint8_t width)
{
	uint64_t bit;
	uint8_t value;

	value = 0;
	bit = entry * width;

	/*
	 * Fast-path single bytes, do repeated tests for the rest: we could
	 * slice-and-dice instead, but the compiler is probably going to do
	 * a better job than I will.
	 *
	 * The Berkeley version of this file uses a #define to compress this
	 * case statement. This code expands the case statement because gcc7
	 * complains about implicit fallthrough and doesn't support explicit
	 * fallthrough comments in macros.
	 */
	switch (width) {
	case 8:
		return (bitf[__bit_byte(bit)]);
	case 7:
		if (__bit_test(bitf, bit))
			value |= 0x40;
		++bit;
		/* FALLTHROUGH */
	case 6:
		if (__bit_test(bitf, bit))
			value |= 0x20;
		++bit;
		/* FALLTHROUGH */
	case 5:
		if (__bit_test(bitf, bit))
			value |= 0x10;
		++bit;
		/* FALLTHROUGH */
	case 4:
		if (__bit_test(bitf, bit))
			value |= 0x08;
		++bit;
		/* FALLTHROUGH */
	case 3:
		if (__bit_test(bitf, bit))
			value |= 0x04;
		++bit;
		/* FALLTHROUGH */
	case 2:
		if (__bit_test(bitf, bit))
			value |= 0x02;
		++bit;
		/* FALLTHROUGH */
	case 1:
		if (__bit_test(bitf, bit))
			value |= 0x01;
		++bit;
		break;
	}
	return (value);
}

/*
 * __bit_getv_recno --
 *	Return a record number's bit-field value.
 */
static inline uint8_t
__bit_getv_recno(WT_REF *ref, uint64_t recno, uint8_t width)
{
	return (__bit_getv(
	    ref->page->pg_fix_bitf, recno - ref->ref_recno, width));
}

/*
 * __bit_setv --
 *	Set a fixed-length column store bit-field value.
 */
static inline void
__bit_setv(uint8_t *bitf, uint64_t entry, uint8_t width, uint8_t value)
{
	uint64_t bit;

	bit = entry * width;

	/*
	 * Fast-path single bytes, do repeated tests for the rest: we could
	 * slice-and-dice instead, but the compiler is probably going to do
	 * a better job than I will.
	 *
	 * The Berkeley version of this file uses a #define to compress this
	 * case statement. This code expands the case statement because gcc7
	 * complains about implicit fallthrough and doesn't support explicit
	 * fallthrough comments in macros.
	 */
	switch (width) {
	case 8:
		bitf[__bit_byte(bit)] = value;
		return;
	case 7:
		if (value & 0x40)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 6:
		if (value & 0x20)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 5:
		if (value & 0x10)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 4:
		if (value & 0x08)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 3:
		if (value & 0x04)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 2:
		if (value & 0x02)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		/* FALLTHROUGH */
	case 1:
		if (value & 0x01)
			__bit_set(bitf, bit);
		else
			__bit_clear(bitf, bit);
		++bit;
		break;
	}
}

/*
 * __bit_clear_end --
 *     Clear the leftover end bits of a fixed-length column store bitstring.
 */
static inline void
__bit_clear_end(uint8_t *bitf, uint64_t numentries, uint8_t width)
{
	uint64_t byte, firstbit;
        uint8_t mask;

        /* Figure the first bit that's past the end of the data, and get its position. */
	firstbit = numentries * width;
        byte = __bit_byte(firstbit);
        mask = (uint8_t)__bit_mask(firstbit);

        /* If mask is the first bit of the byte, we fit evenly and don't need to do anything. */
        if (mask == 0x01)
            return;

        /*
         * We want to clear this bit and up in the byte. Convert first to the bits below this bit,
         * then flip to get the bits to clear. That is, 0b00000100 -> 0b00000011 -> 0b11111100.
         */
        mask = ~(uint8_t)(mask - 1);
        bitf[byte] &= ~mask;
}

/*
 * __bit_end_is_clear --
 *     Check the leftover end bits of a fixed-length column store bitstring.
 */
static inline bool
__bit_end_is_clear(const uint8_t *bitf, uint64_t numentries, uint8_t width)
{
	uint64_t byte, firstbit;
        uint8_t mask;

	firstbit = numentries * width;
        byte = __bit_byte(firstbit);
        mask = (uint8_t)__bit_mask(firstbit);

        if (mask == 0x01)
            return (true);

        mask = ~(uint8_t)(mask - 1);
        return ((bitf[byte] & mask) == 0);
}
