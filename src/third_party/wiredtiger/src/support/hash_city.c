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

/*
 * Copyright (c) 2011 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * CityHash, by Geoff Pike and Jyrki Alakuijala
 *
 * This file provides CityHash64() and related functions.
 *
 * It's probably possible to create even faster hash functions by
 * writing a program that systematically explores some of the space of
 * possible hash functions, by using SIMD instructions, or by
 * compromising on hash quality.
 */

#include "wt_internal.h"

/*
 * Google City Hash implementation. Based on source code from:
 * http://code.google.com/p/cityhash/
 */

typedef struct _uint128 uint128;
struct _uint128 {
  uint64_t first;
  uint64_t second;
};

#define	Uint128Low64(x) 	(x).first
#define	Uint128High64(x)	(x).second

static uint64_t UNALIGNED_LOAD64(const char *p) {
	uint64_t result;
	memcpy(&result, p, sizeof(result));
	return (result);
}

static uint32_t UNALIGNED_LOAD32(const char *p) {
	uint32_t result;
	memcpy(&result, p, sizeof(result));
	return (result);
}

#ifdef WORDS_BIGENDIAN
#ifdef _MSC_VER

#include <stdlib.h>
#define	bswap_32(x) _byteswap_ulong(x)
#define	bswap_64(x) _byteswap_uint64(x)

#elif defined(__APPLE__)

// Mac OS X / Darwin features
#include <libkern/OSByteOrder.h>
#define	bswap_32(x) OSSwapInt32(x)
#define	bswap_64(x) OSSwapInt64(x)

#elif defined(__sun) || defined(sun)

#include <sys/byteorder.h>
#define	bswap_32(x) BSWAP_32(x)
#define	bswap_64(x) BSWAP_64(x)

#elif defined(__FreeBSD__)

#include <sys/endian.h>
#define	bswap_32(x) bswap32(x)
#define	bswap_64(x) bswap64(x)

#elif defined(__OpenBSD__)

#include <sys/types.h>
#define	bswap_32(x) swap32(x)
#define	bswap_64(x) swap64(x)

#elif defined(__NetBSD__)

#include <sys/types.h>
#include <machine/bswap.h>
#if defined(__BSWAP_RENAME) && !defined(__bswap_32)
#define	bswap_32(x) bswap32(x)
#define	bswap_64(x) bswap64(x)
#endif

#else

#define	bswap_32(x) __wt_bswap32(x)
#define	bswap_64(x) __wt_bswap64(x)

#endif

#define	uint32_in_expected_order(x) (bswap_32(x))
#define	uint64_in_expected_order(x) (bswap_64(x))
#else
#define	uint32_in_expected_order(x) (x)
#define	uint64_in_expected_order(x) (x)
#endif

static uint64_t Fetch64(const char *p) {
	return uint64_in_expected_order(UNALIGNED_LOAD64(p));
}

static uint32_t Fetch32(const char *p) {
	return uint32_in_expected_order(UNALIGNED_LOAD32(p));
}

/* Some primes between 2^63 and 2^64 for various uses. */
static const uint64_t k0 = 0xc3a5c85c97cb3127ULL;
static const uint64_t k1 = 0xb492b66fbe98f273ULL;
static const uint64_t k2 = 0x9ae16a3b2f90404fULL;
static const uint64_t k3 = 0xc949d7c7509e6557ULL;

/*
 * Hash 128 input bits down to 64 bits of output.
 * This is intended to be a reasonably good hash function.
 */
static inline uint64_t Hash128to64(const uint128 x) {
	/* Murmur-inspired hashing. */
	const uint64_t kMul = 0x9ddfea08eb382d69ULL;
	uint64_t a, b;

	a = (Uint128Low64(x) ^ Uint128High64(x)) * kMul;
	a ^= (a >> 47);
	b = (Uint128High64(x) ^ a) * kMul;
	b ^= (b >> 47);
	b *= kMul;
	return (b);
}

/*
 * Bitwise right rotate.  Normally this will compile to a single
 * instruction, especially if the shift is a manifest constant.
 */
static uint64_t Rotate(uint64_t val, int shift) {
	/* Avoid shifting by 64: doing so yields an undefined result. */
	return shift == 0 ? val : ((val >> shift) | (val << (64 - shift)));
}

/*
 * Equivalent to Rotate(), but requires the second arg to be non-zero.
 * On x86-64, and probably others, it's possible for this to compile
 * to a single instruction if both args are already in registers.
 */
static uint64_t RotateByAtLeast1(uint64_t val, int shift) {
	return (val >> shift) | (val << (64 - shift));
}

static uint64_t ShiftMix(uint64_t val) {
	return val ^ (val >> 47);
}

static uint64_t HashLen16(uint64_t u, uint64_t v) {
	uint128 result;

	result.first = u;
	result.second = v;
	return Hash128to64(result);
}

static uint64_t HashLen0to16(const char *s, size_t len) {
	uint64_t a64, b64;
	uint32_t y, z;
	uint8_t a8, b8, c8;
	if (len > 8) {
		a64 = Fetch64(s);
		b64 = Fetch64(s + len - 8);
		return HashLen16(
		    a64, RotateByAtLeast1(b64 + len, (int)len)) ^ b64;
	}
	if (len >= 4) {
		a64 = Fetch32(s);
		return HashLen16(len + (a64 << 3), Fetch32(s + len - 4));
	}
	if (len > 0) {
		a8 = (uint8_t)s[0];
		b8 = (uint8_t)s[len >> 1];
		c8 = (uint8_t)s[len - 1];
		y = (uint32_t)(a8) + ((uint32_t)(b8) << 8);
		z = (uint32_t)len + ((uint32_t)(c8) << 2);
		return ShiftMix(y * k2 ^ z * k3) * k2;
	}
	return (k2);
}

/*
 * This probably works well for 16-byte strings as well, but it may be overkill
 * in that case.
 */
static uint64_t HashLen17to32(const char *s, size_t len) {
	uint64_t a = Fetch64(s) * k1;
	uint64_t b = Fetch64(s + 8);
	uint64_t c = Fetch64(s + len - 8) * k2;
	uint64_t d = Fetch64(s + len - 16) * k0;
	return HashLen16(Rotate(a - b, 43) + Rotate(c, 30) + d,
			a + Rotate(b ^ k3, 20) + len - c);
}

/*
 * Return a 16-byte hash for 48 bytes.  Quick and dirty.
 * Callers do best to use "random-looking" values for a and b.
 * static pair<uint64, uint64> WeakHashLen32WithSeeds(
 */
static void WeakHashLen32WithSeeds6(uint64_t w, uint64_t x,
    uint64_t y, uint64_t z, uint64_t a, uint64_t b, uint128 *ret) {
	uint64_t c;

	a += w;
	b = Rotate(b + a + z, 21);
	c = a;
	a += x;
	a += y;
	b += Rotate(a, 44);

	ret->first = (uint64_t) (a + z);
	ret->second = (uint64_t) (b + c);
}

/*
 * Return a 16-byte hash for s[0] ... s[31], a, and b.  Quick and dirty.
 * static pair<uint64, uint64> WeakHashLen32WithSeeds(
 */
static void WeakHashLen32WithSeeds(
		const char* s, uint64_t a, uint64_t b, uint128 *ret) {
	WeakHashLen32WithSeeds6(Fetch64(s),
	    Fetch64(s + 8),
	    Fetch64(s + 16),
	    Fetch64(s + 24),
	    a,
	    b,
	    ret);
}

/* Return an 8-byte hash for 33 to 64 bytes. */
static uint64_t HashLen33to64(const char *s, size_t len) {
	uint64_t a, b, c, r, vf, vs, wf, ws, z;
	z = Fetch64(s + 24);
	a = Fetch64(s) + (len + Fetch64(s + len - 16)) * k0;
	b = Rotate(a + z, 52);
	c = Rotate(a, 37);
	a += Fetch64(s + 8);
	c += Rotate(a, 7);
	a += Fetch64(s + 16);
	vf = a + z;
	vs = b + Rotate(a, 31) + c;
	a = Fetch64(s + 16) + Fetch64(s + len - 32);
	z = Fetch64(s + len - 8);
	b = Rotate(a + z, 52);
	c = Rotate(a, 37);
	a += Fetch64(s + len - 24);
	c += Rotate(a, 7);
	a += Fetch64(s + len - 16);
	wf = a + z;
	ws = b + Rotate(a, 31) + c;
	r = ShiftMix((vf + ws) * k2 + (wf + vs) * k0);
	return ShiftMix(r * k0 + vs) * k2;
}

static inline uint64_t CityHash64(const char *s, size_t len) {
	uint64_t temp, x, y, z;
	uint128 v, w;

	if (len <= 32) {
		if (len <= 16) {
			return HashLen0to16(s, len);
		} else {
			return HashLen17to32(s, len);
		}
	} else if (len <= 64) {
		return HashLen33to64(s, len);
	}

	/*
	 * For strings over 64 bytes we hash the end first, and then as we
	 * loop we keep 56 bytes of state: v, w, x, y, and z.
	 */
	x = Fetch64(s + len - 40);
	y = Fetch64(s + len - 16) + Fetch64(s + len - 56);
	z = HashLen16(Fetch64(s + len - 48) + len, Fetch64(s + len - 24));
	WeakHashLen32WithSeeds(s + len - 64, len, z, &v);
	WeakHashLen32WithSeeds(s + len - 32, y + k1, x, &w);
	x = x * k1 + Fetch64(s);

	/*
	 * Use len to count multiples of 64, and operate on 64-byte chunks.
	 */
	for (len = (len - 1) >> 6; len != 0; len--) {
		x = Rotate(x + y + v.first + Fetch64(s + 8), 37) * k1;
		y = Rotate(y + v.second + Fetch64(s + 48), 42) * k1;
		x ^= w.second;
		y += v.first + Fetch64(s + 40);
		z = Rotate(z + w.first, 33) * k1;
		WeakHashLen32WithSeeds(s, v.second * k1, x + w.first, &v);
		WeakHashLen32WithSeeds(
		    s + 32, z + w.second, y + Fetch64(s + 16), &w);
		temp = z;
		z = x;
		x = temp;
		s += 64;
	}
	return HashLen16(HashLen16(v.first, w.first) + ShiftMix(y) * k1 + z,
	    HashLen16(v.second, w.second) + x);
}

/*
 * __wt_hash_city64 --
 *	WiredTiger wrapper around third party hash implementation.
 */
uint64_t
__wt_hash_city64(const void *s, size_t len)
{
	return (CityHash64(s, len));
}
