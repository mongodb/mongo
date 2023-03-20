/*
 * C port of the snappy compressor from Google.
 * This is a very fast compressor with comparable compression to lzo.
 * Works best on 64bit little-endian, but should be good on others too.
 * Ported by Andi Kleen.
 * Uptodate with snappy 1.1.0
 */

/*
 * Copyright 2005 Google Inc. All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#endif

#ifndef SG
#define SG /* Scatter-Gather / iovec support in Snappy */
#endif

#ifdef __KERNEL__
#include <linux/kernel.h>
#ifdef SG
#include <linux/uio.h>
#endif
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/snappy.h>
#include <linux/vmalloc.h>
#include <asm/unaligned.h>
#else
#include "snappy.h"
#include "snappy_compat.h"
#endif

#include "rd.h"

#ifdef _MSC_VER
#define inline __inline
#endif

static inline u64 get_unaligned64(const void *b)
{
	u64 ret;
	memcpy(&ret, b, sizeof(u64));
	return ret;
}
static inline u32 get_unaligned32(const void *b)
{
	u32 ret;
	memcpy(&ret, b, sizeof(u32));
	return ret;
}
#define get_unaligned_le32(x) (le32toh(get_unaligned32((u32 *)(x))))

static inline void put_unaligned64(u64 v, void *b)
{
	memcpy(b, &v, sizeof(v));
}
static inline void put_unaligned32(u32 v, void *b)
{
	memcpy(b, &v, sizeof(v));
}
static inline void put_unaligned16(u16 v, void *b)
{
	memcpy(b, &v, sizeof(v));
}
#define put_unaligned_le16(v,x) (put_unaligned16(htole16(v), (u16 *)(x)))


#define CRASH_UNLESS(x) BUG_ON(!(x))
#define CHECK(cond) CRASH_UNLESS(cond)
#define CHECK_LE(a, b) CRASH_UNLESS((a) <= (b))
#define CHECK_GE(a, b) CRASH_UNLESS((a) >= (b))
#define CHECK_EQ(a, b) CRASH_UNLESS((a) == (b))
#define CHECK_NE(a, b) CRASH_UNLESS((a) != (b))
#define CHECK_LT(a, b) CRASH_UNLESS((a) < (b))
#define CHECK_GT(a, b) CRASH_UNLESS((a) > (b))

#define UNALIGNED_LOAD32(_p) get_unaligned32((u32 *)(_p))
#define UNALIGNED_LOAD64(_p) get_unaligned64((u64 *)(_p))

#define UNALIGNED_STORE16(_p, _val) put_unaligned16(_val, (u16 *)(_p))
#define UNALIGNED_STORE32(_p, _val) put_unaligned32(_val, (u32 *)(_p))
#define UNALIGNED_STORE64(_p, _val) put_unaligned64(_val, (u64 *)(_p))

/*
 * This can be more efficient than UNALIGNED_LOAD64 + UNALIGNED_STORE64
 * on some platforms, in particular ARM.
 */
static inline void unaligned_copy64(const void *src, void *dst)
{
	if (sizeof(void *) == 8) {
		UNALIGNED_STORE64(dst, UNALIGNED_LOAD64(src));
	} else {
		const char *src_char = (const char *)(src);
		char *dst_char = (char *)(dst);

		UNALIGNED_STORE32(dst_char, UNALIGNED_LOAD32(src_char));
		UNALIGNED_STORE32(dst_char + 4, UNALIGNED_LOAD32(src_char + 4));
	}
}

#ifdef NDEBUG

#define DCHECK(cond) do {} while(0)
#define DCHECK_LE(a, b) do {} while(0)
#define DCHECK_GE(a, b) do {} while(0)
#define DCHECK_EQ(a, b) do {} while(0)
#define DCHECK_NE(a, b) do {} while(0)
#define DCHECK_LT(a, b) do {} while(0)
#define DCHECK_GT(a, b) do {} while(0)

#else

#define DCHECK(cond) CHECK(cond)
#define DCHECK_LE(a, b) CHECK_LE(a, b)
#define DCHECK_GE(a, b) CHECK_GE(a, b)
#define DCHECK_EQ(a, b) CHECK_EQ(a, b)
#define DCHECK_NE(a, b) CHECK_NE(a, b)
#define DCHECK_LT(a, b) CHECK_LT(a, b)
#define DCHECK_GT(a, b) CHECK_GT(a, b)

#endif

static inline bool is_little_endian(void)
{
#ifdef __LITTLE_ENDIAN__
	return true;
#endif
	return false;
}

#if defined(__xlc__) // xlc compiler on AIX
#define rd_clz(n)   __cntlz4(n)
#define rd_ctz(n)   __cnttz4(n)
#define rd_ctz64(n) __cnttz8(n)

#elif defined(__SUNPRO_C) // Solaris Studio compiler on sun  
/*
 * Source for following definitions is Hacker’s Delight, Second Edition by Henry S. Warren
 * http://www.hackersdelight.org/permissions.htm
 */
u32 rd_clz(u32 x) {
   u32 n;

   if (x == 0) return(32);
   n = 1;
   if ((x >> 16) == 0) {n = n +16; x = x <<16;}
   if ((x >> 24) == 0) {n = n + 8; x = x << 8;}
   if ((x >> 28) == 0) {n = n + 4; x = x << 4;}
   if ((x >> 30) == 0) {n = n + 2; x = x << 2;}
   n = n - (x >> 31);
   return n;
}

u32 rd_ctz(u32 x) {
   u32 y;
   u32 n;

   if (x == 0) return 32;
   n = 31;
   y = x <<16;  if (y != 0) {n = n -16; x = y;}
   y = x << 8;  if (y != 0) {n = n - 8; x = y;}
   y = x << 4;  if (y != 0) {n = n - 4; x = y;}
   y = x << 2;  if (y != 0) {n = n - 2; x = y;}
   y = x << 1;  if (y != 0) {n = n - 1;}
   return n;
}

u64 rd_ctz64(u64 x) {
   u64 y;
   u64 n;

   if (x == 0) return 64;
   n = 63;
   y = x <<32;  if (y != 0) {n = n -32; x = y;}
   y = x <<16;  if (y != 0) {n = n -16; x = y;}
   y = x << 8;  if (y != 0) {n = n - 8; x = y;}
   y = x << 4;  if (y != 0) {n = n - 4; x = y;}
   y = x << 2;  if (y != 0) {n = n - 2; x = y;}
   y = x << 1;  if (y != 0) {n = n - 1;}
   return n;
}
#elif !defined(_MSC_VER)
#define rd_clz(n)   __builtin_clz(n)
#define rd_ctz(n)   __builtin_ctz(n)
#define rd_ctz64(n) __builtin_ctzll(n)
#else
#include <intrin.h>
static int inline rd_clz(u32 x) {
	int r = 0;
	if (_BitScanForward(&r, x))
		return 31 - r;
	else
		return 32;
}

static int inline rd_ctz(u32 x) {
	int r = 0;
	if (_BitScanForward(&r, x))
		return r;
	else
		return 32;
}

static int inline rd_ctz64(u64 x) {
#ifdef _M_X64
	int r = 0;
	if (_BitScanReverse64(&r, x))
		return r;
	else
		return 64;
#else
	int r;
	if ((r = rd_ctz(x & 0xffffffff)) < 32)
		return r;
	return 32 + rd_ctz(x >> 32);
#endif
}
#endif


static inline int log2_floor(u32 n)
{
	return n == 0 ? -1 : 31 ^ rd_clz(n);
}

static inline RD_UNUSED int find_lsb_set_non_zero(u32 n)
{
	return rd_ctz(n);
}

static inline RD_UNUSED int find_lsb_set_non_zero64(u64 n)
{
	return rd_ctz64(n);
}

#define kmax32 5

/*
 * Attempts to parse a varint32 from a prefix of the bytes in [ptr,limit-1].
 *  Never reads a character at or beyond limit.  If a valid/terminated varint32
 * was found in the range, stores it in *OUTPUT and returns a pointer just
 * past the last byte of the varint32. Else returns NULL.  On success,
 * "result <= limit".
 */
static inline const char *varint_parse32_with_limit(const char *p,
						    const char *l,
						    u32 * OUTPUT)
{
	const unsigned char *ptr = (const unsigned char *)(p);
	const unsigned char *limit = (const unsigned char *)(l);
	u32 b, result;

	if (ptr >= limit)
		return NULL;
	b = *(ptr++);
	result = b & 127;
	if (b < 128)
		goto done;
	if (ptr >= limit)
		return NULL;
	b = *(ptr++);
	result |= (b & 127) << 7;
	if (b < 128)
		goto done;
	if (ptr >= limit)
		return NULL;
	b = *(ptr++);
	result |= (b & 127) << 14;
	if (b < 128)
		goto done;
	if (ptr >= limit)
		return NULL;
	b = *(ptr++);
	result |= (b & 127) << 21;
	if (b < 128)
		goto done;
	if (ptr >= limit)
		return NULL;
	b = *(ptr++);
	result |= (b & 127) << 28;
	if (b < 16)
		goto done;
	return NULL;		/* Value is too long to be a varint32 */
done:
	*OUTPUT = result;
	return (const char *)(ptr);
}

/*
 * REQUIRES   "ptr" points to a buffer of length sufficient to hold "v".
 *  EFFECTS    Encodes "v" into "ptr" and returns a pointer to the
 *            byte just past the last encoded byte.
 */
static inline char *varint_encode32(char *sptr, u32 v)
{
	/* Operate on characters as unsigneds */
	unsigned char *ptr = (unsigned char *)(sptr);
	static const int B = 128;

	if (v < (1 << 7)) {
		*(ptr++) = v;
	} else if (v < (1 << 14)) {
		*(ptr++) = v | B;
		*(ptr++) = v >> 7;
	} else if (v < (1 << 21)) {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = v >> 14;
	} else if (v < (1 << 28)) {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = v >> 21;
	} else {
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = (v >> 21) | B;
		*(ptr++) = v >> 28;
	}
	return (char *)(ptr);
}

#ifdef SG

static inline void *n_bytes_after_addr(void *addr, size_t n_bytes)
{
    return (void *) ((char *)addr + n_bytes);
}

struct source {
	struct iovec *iov;
	int iovlen;
	int curvec;
	int curoff;
	size_t total;
};

/* Only valid at beginning when nothing is consumed */
static inline int available(struct source *s)
{
	return (int) s->total;
}

static inline const char *peek(struct source *s, size_t *len)
{
	if (likely(s->curvec < s->iovlen)) {
		struct iovec *iv = &s->iov[s->curvec];
		if ((unsigned)s->curoff < (size_t)iv->iov_len) { 
			*len = iv->iov_len - s->curoff;
			return n_bytes_after_addr(iv->iov_base, s->curoff);
		}
	}
	*len = 0;
	return NULL;
}

static inline void skip(struct source *s, size_t n)
{
	struct iovec *iv = &s->iov[s->curvec];
	s->curoff += (int) n;
	DCHECK_LE((unsigned)s->curoff, (size_t)iv->iov_len);
	if ((unsigned)s->curoff >= (size_t)iv->iov_len &&
	    s->curvec + 1 < s->iovlen) {
		s->curoff = 0;
		s->curvec++;
	}
}

struct sink {
	struct iovec *iov;
	int iovlen;
	unsigned curvec;
	unsigned curoff;
	unsigned written;
};

static inline void append(struct sink *s, const char *data, size_t n)
{
	struct iovec *iov = &s->iov[s->curvec];
	char *dst = n_bytes_after_addr(iov->iov_base, s->curoff);
	size_t nlen = min_t(size_t, iov->iov_len - s->curoff, n);
	if (data != dst)
		memcpy(dst, data, nlen);
	s->written += (int) n;
	s->curoff += (int) nlen;
	while ((n -= nlen) > 0) {
		data += nlen;
		s->curvec++;
		DCHECK_LT((signed)s->curvec, s->iovlen);
		iov++;
		nlen = min_t(size_t, (size_t)iov->iov_len, n);
		memcpy(iov->iov_base, data, nlen);
		s->curoff = (int) nlen;
	}
}

static inline void *sink_peek(struct sink *s, size_t n)
{
	struct iovec *iov = &s->iov[s->curvec];
	if (s->curvec < (size_t)iov->iov_len && iov->iov_len - s->curoff >= n)
		return n_bytes_after_addr(iov->iov_base, s->curoff);
	return NULL;
}

#else

struct source {
	const char *ptr;
	size_t left;
};

static inline int available(struct source *s)
{
	return s->left;
}

static inline const char *peek(struct source *s, size_t * len)
{
	*len = s->left;
	return s->ptr;
}

static inline void skip(struct source *s, size_t n)
{
	s->left -= n;
	s->ptr += n;
}

struct sink {
	char *dest;
};

static inline void append(struct sink *s, const char *data, size_t n)
{
	if (data != s->dest)
		memcpy(s->dest, data, n);
	s->dest += n;
}

#define sink_peek(s, n) sink_peek_no_sg(s)

static inline void *sink_peek_no_sg(const struct sink *s)
{
	return s->dest;
}

#endif

struct writer {
	char *base;
	char *op;
	char *op_limit;
};

/* Called before decompression */
static inline void writer_set_expected_length(struct writer *w, size_t len)
{
	w->op_limit = w->op + len;
}

/* Called after decompression */
static inline bool writer_check_length(struct writer *w)
{
	return w->op == w->op_limit;
}

/*
 * Copy "len" bytes from "src" to "op", one byte at a time.  Used for
 *  handling COPY operations where the input and output regions may
 * overlap.  For example, suppose:
 *    src    == "ab"
 *    op     == src + 2
 *    len    == 20
 * After IncrementalCopy(src, op, len), the result will have
 * eleven copies of "ab"
 *    ababababababababababab
 * Note that this does not match the semantics of either memcpy()
 * or memmove().
 */
static inline void incremental_copy(const char *src, char *op, ssize_t len)
{
	DCHECK_GT(len, 0);
	do {
		*op++ = *src++;
	} while (--len > 0);
}

/*
 * Equivalent to IncrementalCopy except that it can write up to ten extra
 *  bytes after the end of the copy, and that it is faster.
 *
 * The main part of this loop is a simple copy of eight bytes at a time until
 * we've copied (at least) the requested amount of bytes.  However, if op and
 * src are less than eight bytes apart (indicating a repeating pattern of
 * length < 8), we first need to expand the pattern in order to get the correct
 * results. For instance, if the buffer looks like this, with the eight-byte
 * <src> and <op> patterns marked as intervals:
 *
 *    abxxxxxxxxxxxx
 *    [------]           src
 *      [------]         op
 *
 * a single eight-byte copy from <src> to <op> will repeat the pattern once,
 * after which we can move <op> two bytes without moving <src>:
 *
 *    ababxxxxxxxxxx
 *    [------]           src
 *        [------]       op
 *
 * and repeat the exercise until the two no longer overlap.
 *
 * This allows us to do very well in the special case of one single byte
 * repeated many times, without taking a big hit for more general cases.
 *
 * The worst case of extra writing past the end of the match occurs when
 * op - src == 1 and len == 1; the last copy will read from byte positions
 * [0..7] and write to [4..11], whereas it was only supposed to write to
 * position 1. Thus, ten excess bytes.
 */

#define kmax_increment_copy_overflow  10

static inline void incremental_copy_fast_path(const char *src, char *op,
					      ssize_t len)
{
	while (op - src < 8) {
		unaligned_copy64(src, op);
		len -= op - src;
		op += op - src;
	}
	while (len > 0) {
		unaligned_copy64(src, op);
		src += 8;
		op += 8;
		len -= 8;
	}
}

static inline bool writer_append_from_self(struct writer *w, u32 offset,
					   u32 len)
{
	char *const op = w->op;
	CHECK_LE(op, w->op_limit);
	const u32 space_left = (u32) (w->op_limit - op);

	if ((unsigned)(op - w->base) <= offset - 1u)	/* -1u catches offset==0 */
		return false;
	if (len <= 16 && offset >= 8 && space_left >= 16) {
		/* Fast path, used for the majority (70-80%) of dynamic
		 * invocations. */
		unaligned_copy64(op - offset, op);
		unaligned_copy64(op - offset + 8, op + 8);
	} else {
		if (space_left >= len + kmax_increment_copy_overflow) {
			incremental_copy_fast_path(op - offset, op, len);
		} else {
			if (space_left < len) {
				return false;
			}
			incremental_copy(op - offset, op, len);
		}
	}

	w->op = op + len;
	return true;
}

static inline bool writer_append(struct writer *w, const char *ip, u32 len)
{
	char *const op = w->op;
	CHECK_LE(op, w->op_limit);
	const u32 space_left = (u32) (w->op_limit - op);
	if (space_left < len)
		return false;
	memcpy(op, ip, len);
	w->op = op + len;
	return true;
}

static inline bool writer_try_fast_append(struct writer *w, const char *ip, 
					  u32 available_bytes, u32 len)
{
	char *const op = w->op;
	const int space_left = (int) (w->op_limit - op);
	if (len <= 16 && available_bytes >= 16 && space_left >= 16) {
		/* Fast path, used for the majority (~95%) of invocations */
		unaligned_copy64(ip, op);
		unaligned_copy64(ip + 8, op + 8);
		w->op = op + len;
		return true;
	}
	return false;
}

/*
 * Any hash function will produce a valid compressed bitstream, but a good
 * hash function reduces the number of collisions and thus yields better
 * compression for compressible input, and more speed for incompressible
 * input. Of course, it doesn't hurt if the hash function is reasonably fast
 * either, as it gets called a lot.
 */
static inline u32 hash_bytes(u32 bytes, int shift)
{
	u32 kmul = 0x1e35a7bd;
	return (bytes * kmul) >> shift;
}

static inline u32 hash(const char *p, int shift)
{
	return hash_bytes(UNALIGNED_LOAD32(p), shift);
}

/*
 * Compressed data can be defined as:
 *    compressed := item* literal*
 *    item       := literal* copy
 *
 * The trailing literal sequence has a space blowup of at most 62/60
 * since a literal of length 60 needs one tag byte + one extra byte
 * for length information.
 *
 * Item blowup is trickier to measure.  Suppose the "copy" op copies
 * 4 bytes of data.  Because of a special check in the encoding code,
 * we produce a 4-byte copy only if the offset is < 65536.  Therefore
 * the copy op takes 3 bytes to encode, and this type of item leads
 * to at most the 62/60 blowup for representing literals.
 *
 * Suppose the "copy" op copies 5 bytes of data.  If the offset is big
 * enough, it will take 5 bytes to encode the copy op.  Therefore the
 * worst case here is a one-byte literal followed by a five-byte copy.
 * I.e., 6 bytes of input turn into 7 bytes of "compressed" data.
 *
 * This last factor dominates the blowup, so the final estimate is:
 */
size_t rd_kafka_snappy_max_compressed_length(size_t source_len)
{
	return 32 + source_len + source_len / 6;
}
EXPORT_SYMBOL(rd_kafka_snappy_max_compressed_length);

enum {
	LITERAL = 0,
	COPY_1_BYTE_OFFSET = 1,	/* 3 bit length + 3 bits of offset in opcode */
	COPY_2_BYTE_OFFSET = 2,
	COPY_4_BYTE_OFFSET = 3
};

static inline char *emit_literal(char *op,
				 const char *literal,
				 int len, bool allow_fast_path)
{
	int n = len - 1;	/* Zero-length literals are disallowed */

	if (n < 60) {
		/* Fits in tag byte */
		*op++ = LITERAL | (n << 2);

/*
 * The vast majority of copies are below 16 bytes, for which a
 * call to memcpy is overkill. This fast path can sometimes
 * copy up to 15 bytes too much, but that is okay in the
 * main loop, since we have a bit to go on for both sides:
 *
 *   - The input will always have kInputMarginBytes = 15 extra
 *     available bytes, as long as we're in the main loop, and
 *     if not, allow_fast_path = false.
 *   - The output will always have 32 spare bytes (see
 *     MaxCompressedLength).
 */
		if (allow_fast_path && len <= 16) {
			unaligned_copy64(literal, op);
			unaligned_copy64(literal + 8, op + 8);
			return op + len;
		}
	} else {
		/* Encode in upcoming bytes */
		char *base = op;
		int count = 0;
		op++;
		while (n > 0) {
			*op++ = n & 0xff;
			n >>= 8;
			count++;
		}
		DCHECK(count >= 1);
		DCHECK(count <= 4);
		*base = LITERAL | ((59 + count) << 2);
	}
	memcpy(op, literal, len);
	return op + len;
}

static inline char *emit_copy_less_than64(char *op, int offset, int len)
{
	DCHECK_LE(len, 64);
	DCHECK_GE(len, 4);
	DCHECK_LT(offset, 65536);

	if ((len < 12) && (offset < 2048)) {
		int len_minus_4 = len - 4;
		DCHECK(len_minus_4 < 8);	/* Must fit in 3 bits */
		*op++ =
		    COPY_1_BYTE_OFFSET + ((len_minus_4) << 2) + ((offset >> 8)
								 << 5);
		*op++ = offset & 0xff;
	} else {
		*op++ = COPY_2_BYTE_OFFSET + ((len - 1) << 2);
		put_unaligned_le16(offset, op);
		op += 2;
	}
	return op;
}

static inline char *emit_copy(char *op, int offset, int len)
{
	/*
	 * Emit 64 byte copies but make sure to keep at least four bytes
	 * reserved
	 */
	while (len >= 68) {
		op = emit_copy_less_than64(op, offset, 64);
		len -= 64;
	}

	/*
	 * Emit an extra 60 byte copy if have too much data to fit in
	 * one copy
	 */
	if (len > 64) {
		op = emit_copy_less_than64(op, offset, 60);
		len -= 60;
	}

	/* Emit remainder */
	op = emit_copy_less_than64(op, offset, len);
	return op;
}

/**
 * rd_kafka_snappy_uncompressed_length - return length of uncompressed output.
 * @start: compressed buffer
 * @n: length of compressed buffer.
 * @result: Write the length of the uncompressed output here.
 *
 * Returns true when successfull, otherwise false.
 */
bool rd_kafka_snappy_uncompressed_length(const char *start, size_t n, size_t * result)
{
	u32 v = 0;
	const char *limit = start + n;
	if (varint_parse32_with_limit(start, limit, &v) != NULL) {
		*result = v;
		return true;
	} else {
		return false;
	}
}
EXPORT_SYMBOL(rd_kafka_snappy_uncompressed_length);

/*
 * The size of a compression block. Note that many parts of the compression
 * code assumes that kBlockSize <= 65536; in particular, the hash table
 * can only store 16-bit offsets, and EmitCopy() also assumes the offset
 * is 65535 bytes or less. Note also that if you change this, it will
 * affect the framing format
 * Note that there might be older data around that is compressed with larger
 * block sizes, so the decompression code should not rely on the
 * non-existence of long backreferences.
 */
#define kblock_log 16
#define kblock_size (1 << kblock_log)

/* 
 * This value could be halfed or quartered to save memory 
 * at the cost of slightly worse compression.
 */
#define kmax_hash_table_bits 14
#define kmax_hash_table_size (1U << kmax_hash_table_bits)

/*
 * Use smaller hash table when input.size() is smaller, since we
 * fill the table, incurring O(hash table size) overhead for
 * compression, and if the input is short, we won't need that
 * many hash table entries anyway.
 */
static u16 *get_hash_table(struct snappy_env *env, size_t input_size,
			      int *table_size)
{
	unsigned htsize = 256;

	DCHECK(kmax_hash_table_size >= 256);
	while (htsize < kmax_hash_table_size && htsize < input_size)
		htsize <<= 1;
	CHECK_EQ(0, htsize & (htsize - 1));
	CHECK_LE(htsize, kmax_hash_table_size);

	u16 *table;
	table = env->hash_table;

	*table_size = htsize;
	memset(table, 0, htsize * sizeof(*table));
	return table;
}

/*
 * Return the largest n such that
 *
 *   s1[0,n-1] == s2[0,n-1]
 *   and n <= (s2_limit - s2).
 *
 * Does not read *s2_limit or beyond.
 * Does not read *(s1 + (s2_limit - s2)) or beyond.
 * Requires that s2_limit >= s2.
 *
 * Separate implementation for x86_64, for speed.  Uses the fact that
 * x86_64 is little endian.
 */
#if defined(__LITTLE_ENDIAN__) && BITS_PER_LONG == 64
static inline int find_match_length(const char *s1,
				    const char *s2, const char *s2_limit)
{
	int matched = 0;

	DCHECK_GE(s2_limit, s2);
	/*
	 * Find out how long the match is. We loop over the data 64 bits at a
	 * time until we find a 64-bit block that doesn't match; then we find
	 * the first non-matching bit and use that to calculate the total
	 * length of the match.
	 */
	while (likely(s2 <= s2_limit - 8)) {
		if (unlikely
		    (UNALIGNED_LOAD64(s2) == UNALIGNED_LOAD64(s1 + matched))) {
			s2 += 8;
			matched += 8;
		} else {
			/*
			 * On current (mid-2008) Opteron models there
			 * is a 3% more efficient code sequence to
			 * find the first non-matching byte.  However,
			 * what follows is ~10% better on Intel Core 2
			 * and newer, and we expect AMD's bsf
			 * instruction to improve.
			 */
			u64 x =
			    UNALIGNED_LOAD64(s2) ^ UNALIGNED_LOAD64(s1 +
								    matched);
			int matching_bits = find_lsb_set_non_zero64(x);
			matched += matching_bits >> 3;
			return matched;
		}
	}
	while (likely(s2 < s2_limit)) {
		if (likely(s1[matched] == *s2)) {
			++s2;
			++matched;
		} else {
			return matched;
		}
	}
	return matched;
}
#else
static inline int find_match_length(const char *s1,
				    const char *s2, const char *s2_limit)
{
	/* Implementation based on the x86-64 version, above. */
	DCHECK_GE(s2_limit, s2);
	int matched = 0;

	while (s2 <= s2_limit - 4 &&
	       UNALIGNED_LOAD32(s2) == UNALIGNED_LOAD32(s1 + matched)) {
		s2 += 4;
		matched += 4;
	}
	if (is_little_endian() && s2 <= s2_limit - 4) {
		u32 x =
		    UNALIGNED_LOAD32(s2) ^ UNALIGNED_LOAD32(s1 + matched);
		int matching_bits = find_lsb_set_non_zero(x);
		matched += matching_bits >> 3;
	} else {
		while ((s2 < s2_limit) && (s1[matched] == *s2)) {
			++s2;
			++matched;
		}
	}
	return matched;
}
#endif

/*
 * For 0 <= offset <= 4, GetU32AtOffset(GetEightBytesAt(p), offset) will
 *  equal UNALIGNED_LOAD32(p + offset).  Motivation: On x86-64 hardware we have
 * empirically found that overlapping loads such as
 *  UNALIGNED_LOAD32(p) ... UNALIGNED_LOAD32(p+1) ... UNALIGNED_LOAD32(p+2)
 * are slower than UNALIGNED_LOAD64(p) followed by shifts and casts to u32.
 *
 * We have different versions for 64- and 32-bit; ideally we would avoid the
 * two functions and just inline the UNALIGNED_LOAD64 call into
 * GetUint32AtOffset, but GCC (at least not as of 4.6) is seemingly not clever
 * enough to avoid loading the value multiple times then. For 64-bit, the load
 * is done when GetEightBytesAt() is called, whereas for 32-bit, the load is
 * done at GetUint32AtOffset() time.
 */

#if BITS_PER_LONG == 64

typedef u64 eight_bytes_reference;

static inline eight_bytes_reference get_eight_bytes_at(const char* ptr)
{
	return UNALIGNED_LOAD64(ptr);
}

static inline u32 get_u32_at_offset(u64 v, int offset)
{
	DCHECK_GE(offset, 0);
	DCHECK_LE(offset, 4);
	return v >> (is_little_endian()? 8 * offset : 32 - 8 * offset);
}

#else

typedef const char *eight_bytes_reference;

static inline eight_bytes_reference get_eight_bytes_at(const char* ptr) 
{
	return ptr;
}

static inline u32 get_u32_at_offset(const char *v, int offset) 
{
	DCHECK_GE(offset, 0);
	DCHECK_LE(offset, 4);
	return UNALIGNED_LOAD32(v + offset);
}
#endif

/*
 * Flat array compression that does not emit the "uncompressed length"
 *  prefix. Compresses "input" string to the "*op" buffer.
 *
 * REQUIRES: "input" is at most "kBlockSize" bytes long.
 * REQUIRES: "op" points to an array of memory that is at least
 * "MaxCompressedLength(input.size())" in size.
 * REQUIRES: All elements in "table[0..table_size-1]" are initialized to zero.
 * REQUIRES: "table_size" is a power of two
 *
 * Returns an "end" pointer into "op" buffer.
 * "end - op" is the compressed size of "input".
 */

static char *compress_fragment(const char *const input,
			       const size_t input_size,
			       char *op, u16 * table, const unsigned table_size)
{
	/* "ip" is the input pointer, and "op" is the output pointer. */
	const char *ip = input;
	CHECK_LE(input_size, kblock_size);
	CHECK_EQ(table_size & (table_size - 1), 0);
	const int shift = 32 - log2_floor(table_size);
	DCHECK_EQ(UINT_MAX >> shift, table_size - 1);
	const char *ip_end = input + input_size;
	const char *baseip = ip;
	/*
	 * Bytes in [next_emit, ip) will be emitted as literal bytes.  Or
	 *  [next_emit, ip_end) after the main loop.
	 */
	const char *next_emit = ip;

	const unsigned kinput_margin_bytes = 15;

	if (likely(input_size >= kinput_margin_bytes)) {
		const char *const ip_limit = input + input_size -
			kinput_margin_bytes;

		u32 next_hash;
		for (next_hash = hash(++ip, shift);;) {
			DCHECK_LT(next_emit, ip);
/*
 * The body of this loop calls EmitLiteral once and then EmitCopy one or
 * more times.  (The exception is that when we're close to exhausting
 * the input we goto emit_remainder.)
 *
 * In the first iteration of this loop we're just starting, so
 * there's nothing to copy, so calling EmitLiteral once is
 * necessary.  And we only start a new iteration when the
 * current iteration has determined that a call to EmitLiteral will
 * precede the next call to EmitCopy (if any).
 *
 * Step 1: Scan forward in the input looking for a 4-byte-long match.
 * If we get close to exhausting the input then goto emit_remainder.
 *
 * Heuristic match skipping: If 32 bytes are scanned with no matches
 * found, start looking only at every other byte. If 32 more bytes are
 * scanned, look at every third byte, etc.. When a match is found,
 * immediately go back to looking at every byte. This is a small loss
 * (~5% performance, ~0.1% density) for lcompressible data due to more
 * bookkeeping, but for non-compressible data (such as JPEG) it's a huge
 * win since the compressor quickly "realizes" the data is incompressible
 * and doesn't bother looking for matches everywhere.
 *
 * The "skip" variable keeps track of how many bytes there are since the
 * last match; dividing it by 32 (ie. right-shifting by five) gives the
 * number of bytes to move ahead for each iteration.
 */
			u32 skip_bytes = 32;

			const char *next_ip = ip;
			const char *candidate;
			do {
				ip = next_ip;
				u32 hval = next_hash;
				DCHECK_EQ(hval, hash(ip, shift));
				u32 bytes_between_hash_lookups = skip_bytes++ >> 5;
				next_ip = ip + bytes_between_hash_lookups;
				if (unlikely(next_ip > ip_limit)) {
					goto emit_remainder;
				}
				next_hash = hash(next_ip, shift);
				candidate = baseip + table[hval];
				DCHECK_GE(candidate, baseip);
				DCHECK_LT(candidate, ip);

				table[hval] = (u16) (ip - baseip);
			} while (likely(UNALIGNED_LOAD32(ip) !=
					UNALIGNED_LOAD32(candidate)));

/*
 * Step 2: A 4-byte match has been found.  We'll later see if more
 * than 4 bytes match.  But, prior to the match, input
 * bytes [next_emit, ip) are unmatched.  Emit them as "literal bytes."
 */
			DCHECK_LE(next_emit + 16, ip_end);
			op = emit_literal(op, next_emit, (int) (ip - next_emit), true);

/*
 * Step 3: Call EmitCopy, and then see if another EmitCopy could
 * be our next move.  Repeat until we find no match for the
 * input immediately after what was consumed by the last EmitCopy call.
 *
 * If we exit this loop normally then we need to call EmitLiteral next,
 * though we don't yet know how big the literal will be.  We handle that
 * by proceeding to the next iteration of the main loop.  We also can exit
 * this loop via goto if we get close to exhausting the input.
 */
			eight_bytes_reference input_bytes;
			u32 candidate_bytes = 0;

			do {
/*
 * We have a 4-byte match at ip, and no need to emit any
 *  "literal bytes" prior to ip.
 */
				const char *base = ip;
				int matched = 4 +
				    find_match_length(candidate + 4, ip + 4,
						      ip_end);
				ip += matched;
				int offset = (int) (base - candidate);
				DCHECK_EQ(0, memcmp(base, candidate, matched));
				op = emit_copy(op, offset, matched);
/*
 * We could immediately start working at ip now, but to improve
 * compression we first update table[Hash(ip - 1, ...)].
 */
				const char *insert_tail = ip - 1;
				next_emit = ip;
				if (unlikely(ip >= ip_limit)) {
					goto emit_remainder;
				}
				input_bytes = get_eight_bytes_at(insert_tail);
				u32 prev_hash =
				    hash_bytes(get_u32_at_offset
					       (input_bytes, 0), shift);
				table[prev_hash] = (u16) (ip - baseip - 1);
				u32 cur_hash =
				    hash_bytes(get_u32_at_offset
					       (input_bytes, 1), shift);
				candidate = baseip + table[cur_hash];
				candidate_bytes = UNALIGNED_LOAD32(candidate);
				table[cur_hash] = (u16) (ip - baseip);
			} while (get_u32_at_offset(input_bytes, 1) ==
				 candidate_bytes);

			next_hash =
			    hash_bytes(get_u32_at_offset(input_bytes, 2),
				       shift);
			++ip;
		}
	}

emit_remainder:
	/* Emit the remaining bytes as a literal */
	if (next_emit < ip_end)
		op = emit_literal(op, next_emit, (int) (ip_end - next_emit), false);

	return op;
}

/*
 * -----------------------------------------------------------------------
 *  Lookup table for decompression code.  Generated by ComputeTable() below.
 * -----------------------------------------------------------------------
 */

/* Mapping from i in range [0,4] to a mask to extract the bottom 8*i bits */
static const u32 wordmask[] = {
	0u, 0xffu, 0xffffu, 0xffffffu, 0xffffffffu
};

/*
 * Data stored per entry in lookup table:
 *       Range   Bits-used       Description
 *      ------------------------------------
 *      1..64   0..7            Literal/copy length encoded in opcode byte
 *      0..7    8..10           Copy offset encoded in opcode byte / 256
 *      0..4    11..13          Extra bytes after opcode
 *
 * We use eight bits for the length even though 7 would have sufficed
 * because of efficiency reasons:
 *      (1) Extracting a byte is faster than a bit-field
 *      (2) It properly aligns copy offset so we do not need a <<8
 */
static const u16 char_table[256] = {
	0x0001, 0x0804, 0x1001, 0x2001, 0x0002, 0x0805, 0x1002, 0x2002,
	0x0003, 0x0806, 0x1003, 0x2003, 0x0004, 0x0807, 0x1004, 0x2004,
	0x0005, 0x0808, 0x1005, 0x2005, 0x0006, 0x0809, 0x1006, 0x2006,
	0x0007, 0x080a, 0x1007, 0x2007, 0x0008, 0x080b, 0x1008, 0x2008,
	0x0009, 0x0904, 0x1009, 0x2009, 0x000a, 0x0905, 0x100a, 0x200a,
	0x000b, 0x0906, 0x100b, 0x200b, 0x000c, 0x0907, 0x100c, 0x200c,
	0x000d, 0x0908, 0x100d, 0x200d, 0x000e, 0x0909, 0x100e, 0x200e,
	0x000f, 0x090a, 0x100f, 0x200f, 0x0010, 0x090b, 0x1010, 0x2010,
	0x0011, 0x0a04, 0x1011, 0x2011, 0x0012, 0x0a05, 0x1012, 0x2012,
	0x0013, 0x0a06, 0x1013, 0x2013, 0x0014, 0x0a07, 0x1014, 0x2014,
	0x0015, 0x0a08, 0x1015, 0x2015, 0x0016, 0x0a09, 0x1016, 0x2016,
	0x0017, 0x0a0a, 0x1017, 0x2017, 0x0018, 0x0a0b, 0x1018, 0x2018,
	0x0019, 0x0b04, 0x1019, 0x2019, 0x001a, 0x0b05, 0x101a, 0x201a,
	0x001b, 0x0b06, 0x101b, 0x201b, 0x001c, 0x0b07, 0x101c, 0x201c,
	0x001d, 0x0b08, 0x101d, 0x201d, 0x001e, 0x0b09, 0x101e, 0x201e,
	0x001f, 0x0b0a, 0x101f, 0x201f, 0x0020, 0x0b0b, 0x1020, 0x2020,
	0x0021, 0x0c04, 0x1021, 0x2021, 0x0022, 0x0c05, 0x1022, 0x2022,
	0x0023, 0x0c06, 0x1023, 0x2023, 0x0024, 0x0c07, 0x1024, 0x2024,
	0x0025, 0x0c08, 0x1025, 0x2025, 0x0026, 0x0c09, 0x1026, 0x2026,
	0x0027, 0x0c0a, 0x1027, 0x2027, 0x0028, 0x0c0b, 0x1028, 0x2028,
	0x0029, 0x0d04, 0x1029, 0x2029, 0x002a, 0x0d05, 0x102a, 0x202a,
	0x002b, 0x0d06, 0x102b, 0x202b, 0x002c, 0x0d07, 0x102c, 0x202c,
	0x002d, 0x0d08, 0x102d, 0x202d, 0x002e, 0x0d09, 0x102e, 0x202e,
	0x002f, 0x0d0a, 0x102f, 0x202f, 0x0030, 0x0d0b, 0x1030, 0x2030,
	0x0031, 0x0e04, 0x1031, 0x2031, 0x0032, 0x0e05, 0x1032, 0x2032,
	0x0033, 0x0e06, 0x1033, 0x2033, 0x0034, 0x0e07, 0x1034, 0x2034,
	0x0035, 0x0e08, 0x1035, 0x2035, 0x0036, 0x0e09, 0x1036, 0x2036,
	0x0037, 0x0e0a, 0x1037, 0x2037, 0x0038, 0x0e0b, 0x1038, 0x2038,
	0x0039, 0x0f04, 0x1039, 0x2039, 0x003a, 0x0f05, 0x103a, 0x203a,
	0x003b, 0x0f06, 0x103b, 0x203b, 0x003c, 0x0f07, 0x103c, 0x203c,
	0x0801, 0x0f08, 0x103d, 0x203d, 0x1001, 0x0f09, 0x103e, 0x203e,
	0x1801, 0x0f0a, 0x103f, 0x203f, 0x2001, 0x0f0b, 0x1040, 0x2040
};

struct snappy_decompressor {
	struct source *reader;	/* Underlying source of bytes to decompress */
	const char *ip;		/* Points to next buffered byte */
	const char *ip_limit;	/* Points just past buffered bytes */
	u32 peeked;		/* Bytes peeked from reader (need to skip) */
	bool eof;		/* Hit end of input without an error? */
	char scratch[5];	/* Temporary buffer for peekfast boundaries */
};

static void
init_snappy_decompressor(struct snappy_decompressor *d, struct source *reader)
{
	d->reader = reader;
	d->ip = NULL;
	d->ip_limit = NULL;
	d->peeked = 0;
	d->eof = false;
}

static void exit_snappy_decompressor(struct snappy_decompressor *d)
{
	skip(d->reader, d->peeked);
}

/*
 * Read the uncompressed length stored at the start of the compressed data.
 * On succcess, stores the length in *result and returns true.
 * On failure, returns false.
 */
static bool read_uncompressed_length(struct snappy_decompressor *d,
				     u32 * result)
{
	DCHECK(d->ip == NULL);	/*
				 * Must not have read anything yet
				 * Length is encoded in 1..5 bytes
				 */
	*result = 0;
	u32 shift = 0;
	while (true) {
		if (shift >= 32)
			return false;
		size_t n;
		const char *ip = peek(d->reader, &n);
		if (n == 0)
			return false;
		const unsigned char c = *(const unsigned char *)(ip);
		skip(d->reader, 1);
		*result |= (u32) (c & 0x7f) << shift;
		if (c < 128) {
			break;
		}
		shift += 7;
	}
	return true;
}

static bool refill_tag(struct snappy_decompressor *d);

/*
 * Process the next item found in the input.
 * Returns true if successful, false on error or end of input.
 */
static void decompress_all_tags(struct snappy_decompressor *d,
				struct writer *writer)
{
	const char *ip = d->ip;

	/*
	 * We could have put this refill fragment only at the beginning of the loop.
	 * However, duplicating it at the end of each branch gives the compiler more
	 * scope to optimize the <ip_limit_ - ip> expression based on the local
	 * context, which overall increases speed.
	 */
#define MAYBE_REFILL() \
        if (d->ip_limit - ip < 5) {		\
		d->ip = ip;			\
		if (!refill_tag(d)) return;	\
		ip = d->ip;			\
        }


	MAYBE_REFILL();
	for (;;) {
		if (d->ip_limit - ip < 5) {
			d->ip = ip;
			if (!refill_tag(d))
				return;
			ip = d->ip;
		}

		const unsigned char c = *(const unsigned char *)(ip++);

		if ((c & 0x3) == LITERAL) {
			u32 literal_length = (c >> 2) + 1;
			if (writer_try_fast_append(writer, ip, (u32) (d->ip_limit - ip), 
						   literal_length)) {
				DCHECK_LT(literal_length, 61);
				ip += literal_length;
				MAYBE_REFILL();
				continue;
			}
			if (unlikely(literal_length >= 61)) {
				/* Long literal */
				const u32 literal_ll = literal_length - 60;
				literal_length = (get_unaligned_le32(ip) &
						  wordmask[literal_ll]) + 1;
				ip += literal_ll;
			}

			u32 avail = (u32) (d->ip_limit - ip);
			while (avail < literal_length) {
				if (!writer_append(writer, ip, avail))
					return;
				literal_length -= avail;
				skip(d->reader, d->peeked);
				size_t n;
				ip = peek(d->reader, &n);
				avail = (u32) n;
				d->peeked = avail;
				if (avail == 0)
					return;	/* Premature end of input */
				d->ip_limit = ip + avail;
			}
			if (!writer_append(writer, ip, literal_length))
				return;
			ip += literal_length;
			MAYBE_REFILL();
		} else {
			const u32 entry = char_table[c];
			const u32 trailer = get_unaligned_le32(ip) &
				wordmask[entry >> 11];
			const u32 length = entry & 0xff;
			ip += entry >> 11;

			/*
			 * copy_offset/256 is encoded in bits 8..10.
			 * By just fetching those bits, we get
			 * copy_offset (since the bit-field starts at
			 * bit 8).
			 */
			const u32 copy_offset = entry & 0x700;
			if (!writer_append_from_self(writer,
						     copy_offset + trailer,
						     length))
				return;
			MAYBE_REFILL();
		}
	}
}

#undef MAYBE_REFILL

static bool refill_tag(struct snappy_decompressor *d)
{
	const char *ip = d->ip;

	if (ip == d->ip_limit) {
		size_t n;
		/* Fetch a new fragment from the reader */
		skip(d->reader, d->peeked); /* All peeked bytes are used up */
		ip = peek(d->reader, &n);
		d->peeked = (u32) n;
		if (n == 0) {
			d->eof = true;
			return false;
		}
		d->ip_limit = ip + n;
	}

	/* Read the tag character */
	DCHECK_LT(ip, d->ip_limit);
	const unsigned char c = *(const unsigned char *)(ip);
	const u32 entry = char_table[c];
	const u32 needed = (entry >> 11) + 1;	/* +1 byte for 'c' */
	DCHECK_LE(needed, sizeof(d->scratch));

	/* Read more bytes from reader if needed */
	u32 nbuf = (u32) (d->ip_limit - ip);

	if (nbuf < needed) {
		/*
		 * Stitch together bytes from ip and reader to form the word
		 * contents.  We store the needed bytes in "scratch".  They
		 * will be consumed immediately by the caller since we do not
		 * read more than we need.
		 */
		memmove(d->scratch, ip, nbuf);
		skip(d->reader, d->peeked); /* All peeked bytes are used up */
		d->peeked = 0;
		while (nbuf < needed) {
			size_t length;
			const char *src = peek(d->reader, &length);
			if (length == 0)
				return false;
			u32 to_add = min_t(u32, needed - nbuf, (u32) length);
			memcpy(d->scratch + nbuf, src, to_add);
			nbuf += to_add;
			skip(d->reader, to_add);
		}
		DCHECK_EQ(nbuf, needed);
		d->ip = d->scratch;
		d->ip_limit = d->scratch + needed;
	} else if (nbuf < 5) {
		/*
		 * Have enough bytes, but move into scratch so that we do not
		 * read past end of input
		 */
		memmove(d->scratch, ip, nbuf);
		skip(d->reader, d->peeked); /* All peeked bytes are used up */
		d->peeked = 0;
		d->ip = d->scratch;
		d->ip_limit = d->scratch + nbuf;
	} else {
		/* Pass pointer to buffer returned by reader. */
		d->ip = ip;
	}
	return true;
}

static int internal_uncompress(struct source *r,
			       struct writer *writer, u32 max_len)
{
	struct snappy_decompressor decompressor;
	u32 uncompressed_len = 0;

	init_snappy_decompressor(&decompressor, r);

	if (!read_uncompressed_length(&decompressor, &uncompressed_len))
		return -EIO;
	/* Protect against possible DoS attack */
	if ((u64) (uncompressed_len) > max_len)
		return -EIO;

	writer_set_expected_length(writer, uncompressed_len);

	/* Process the entire input */
	decompress_all_tags(&decompressor, writer);

	exit_snappy_decompressor(&decompressor);
	if (decompressor.eof && writer_check_length(writer))
		return 0;
	return -EIO;
}

static inline int sn_compress(struct snappy_env *env, struct source *reader,
			   struct sink *writer)
{
	int err;
	size_t written = 0;
	int N = available(reader);
	char ulength[kmax32];
	char *p = varint_encode32(ulength, N);

	append(writer, ulength, p - ulength);
	written += (p - ulength);

	while (N > 0) {
		/* Get next block to compress (without copying if possible) */
		size_t fragment_size;
		const char *fragment = peek(reader, &fragment_size);
		if (fragment_size == 0) {
			err = -EIO;
			goto out;
		}
		const unsigned num_to_read = min_t(int, N, kblock_size);
		size_t bytes_read = fragment_size;

		int pending_advance = 0;
		if (bytes_read >= num_to_read) {
			/* Buffer returned by reader is large enough */
			pending_advance = num_to_read;
			fragment_size = num_to_read;
		}
		else {
			memcpy(env->scratch, fragment, bytes_read);
			skip(reader, bytes_read);

			while (bytes_read < num_to_read) {
				fragment = peek(reader, &fragment_size);
				size_t n =
				    min_t(size_t, fragment_size,
					  num_to_read - bytes_read);
				memcpy((char *)(env->scratch) + bytes_read, fragment, n);
				bytes_read += n;
				skip(reader, n);
			}
			DCHECK_EQ(bytes_read, num_to_read);
			fragment = env->scratch;
			fragment_size = num_to_read;
		}
		if (fragment_size < num_to_read)
			return -EIO;

		/* Get encoding table for compression */
		int table_size;
		u16 *table = get_hash_table(env, num_to_read, &table_size);

		/* Compress input_fragment and append to dest */
		char *dest;
		dest = sink_peek(writer, rd_kafka_snappy_max_compressed_length(num_to_read));
		if (!dest) {
			/*
			 * Need a scratch buffer for the output,
			 * because the byte sink doesn't have enough
			 * in one piece.
			 */
			dest = env->scratch_output;
		}
		char *end = compress_fragment(fragment, fragment_size,
					      dest, table, table_size);
		append(writer, dest, end - dest);
		written += (end - dest);

		N -= num_to_read;
		skip(reader, pending_advance);
	}

	err = 0;
out:
	return err;
}

#ifdef SG

int rd_kafka_snappy_compress_iov(struct snappy_env *env,
                                 const struct iovec *iov_in, size_t iov_in_cnt,
                                 size_t input_length,
                                 struct iovec *iov_out) {
        struct source reader = {
                .iov = (struct iovec *)iov_in,
                .iovlen = (int)iov_in_cnt,
                .total = input_length
        };
        struct sink writer = {
                .iov = iov_out,
                .iovlen = 1
        };
        int err = sn_compress(env, &reader, &writer);

        iov_out->iov_len = writer.written;

        return err;
}
EXPORT_SYMBOL(rd_kafka_snappy_compress_iov);

/**
 * rd_kafka_snappy_compress - Compress a buffer using the snappy compressor.
 * @env: Preallocated environment
 * @input: Input buffer
 * @input_length: Length of input_buffer
 * @compressed: Output buffer for compressed data
 * @compressed_length: The real length of the output written here.
 *
 * Return 0 on success, otherwise an negative error code.
 *
 * The output buffer must be at least
 * rd_kafka_snappy_max_compressed_length(input_length) bytes long.
 *
 * Requires a preallocated environment from rd_kafka_snappy_init_env.
 * The environment does not keep state over individual calls
 * of this function, just preallocates the memory.
 */
int rd_kafka_snappy_compress(struct snappy_env *env,
		    const char *input,
		    size_t input_length,
		    char *compressed, size_t *compressed_length)
{
	struct iovec iov_in = {
		.iov_base = (char *)input,
		.iov_len = input_length,
	};
	struct iovec iov_out = {
		.iov_base = compressed,
		.iov_len = 0xffffffff,
	};
        return rd_kafka_snappy_compress_iov(env,
                                            &iov_in, 1, input_length,
                                            &iov_out);
}
EXPORT_SYMBOL(rd_kafka_snappy_compress);

int rd_kafka_snappy_uncompress_iov(struct iovec *iov_in, int iov_in_len,
			   size_t input_len, char *uncompressed)
{
	struct source reader = {
		.iov = iov_in,
		.iovlen = iov_in_len,
		.total = input_len
	};
	struct writer output = {
		.base = uncompressed,
		.op = uncompressed
	};
	return internal_uncompress(&reader, &output, 0xffffffff);
}
EXPORT_SYMBOL(rd_kafka_snappy_uncompress_iov);

/**
 * rd_kafka_snappy_uncompress - Uncompress a snappy compressed buffer
 * @compressed: Input buffer with compressed data
 * @n: length of compressed buffer
 * @uncompressed: buffer for uncompressed data
 *
 * The uncompressed data buffer must be at least
 * rd_kafka_snappy_uncompressed_length(compressed) bytes long.
 *
 * Return 0 on success, otherwise an negative error code.
 */
int rd_kafka_snappy_uncompress(const char *compressed, size_t n, char *uncompressed)
{
	struct iovec iov = {
		.iov_base = (char *)compressed,
		.iov_len = n
	};
	return rd_kafka_snappy_uncompress_iov(&iov, 1, n, uncompressed);
}
EXPORT_SYMBOL(rd_kafka_snappy_uncompress);


/**
 * @brief Decompress Snappy message with Snappy-java framing.
 *
 * @returns a malloced buffer with the uncompressed data, or NULL on failure.
 */
char *rd_kafka_snappy_java_uncompress (const char *inbuf, size_t inlen,
                                       size_t *outlenp,
                                       char *errstr, size_t errstr_size) {
        int pass;
        char *outbuf = NULL;

        /**
         * Traverse all chunks in two passes:
         *  pass 1: calculate total uncompressed length
         *  pass 2: uncompress
         *
         * Each chunk is prefixed with 4: length */

        for (pass = 1 ; pass <= 2 ; pass++) {
                ssize_t of = 0;  /* inbuf offset */
                ssize_t uof = 0; /* outbuf offset */

                while (of + 4 <= (ssize_t)inlen) {
                        uint32_t clen; /* compressed length */
                        size_t ulen; /* uncompressed length */
                        int r;

                        memcpy(&clen, inbuf+of, 4);
                        clen = be32toh(clen);
                        of += 4;

                        if (unlikely(clen > inlen - of)) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid snappy-java chunk length "
                                            "%"PRId32" > %"PRIdsz
                                            " available bytes",
                                            clen, (ssize_t)inlen - of);
                                return NULL;
                        }

                        /* Acquire uncompressed length */
                        if (unlikely(!rd_kafka_snappy_uncompressed_length(
                                             inbuf+of, clen, &ulen))) {
                                rd_snprintf(errstr, errstr_size,
                                            "Failed to get length of "
                                            "(snappy-java framed) Snappy "
                                            "compressed payload "
                                            "(clen %"PRId32")",
                                            clen);
                                return NULL;
                        }

                        if (pass == 1) {
                                /* pass 1: calculate total length */
                                of  += clen;
                                uof += ulen;
                                continue;
                        }

                        /* pass 2: Uncompress to outbuf */
                        if (unlikely((r = rd_kafka_snappy_uncompress(
                                              inbuf+of, clen, outbuf+uof)))) {
                                rd_snprintf(errstr, errstr_size,
                                            "Failed to decompress Snappy-java "
                                            "framed payload of size %"PRId32
                                            ": %s",
                                            clen,
                                            rd_strerror(-r/*negative errno*/));
                                rd_free(outbuf);
                                return NULL;
                        }

                        of  += clen;
                        uof += ulen;
                }

                if (unlikely(of != (ssize_t)inlen)) {
                        rd_snprintf(errstr, errstr_size,
                                    "%"PRIusz" trailing bytes in Snappy-java "
                                    "framed compressed data",
                                    inlen - of);
                        if (outbuf)
                                rd_free(outbuf);
                        return NULL;
                }

                if (pass == 1) {
                        if (uof <= 0) {
                                rd_snprintf(errstr, errstr_size,
                                            "Empty Snappy-java framed data");
                                return NULL;
                        }

                        /* Allocate memory for uncompressed data */
                        outbuf = rd_malloc(uof);
                        if (unlikely(!outbuf)) {
                                rd_snprintf(errstr, errstr_size,
                                           "Failed to allocate memory "
                                            "(%"PRIdsz") for "
                                            "uncompressed Snappy data: %s",
                                            uof, rd_strerror(errno));
                                return NULL;
                        }

                } else {
                        /* pass 2 */
                        *outlenp = uof;
                }
        }

        return outbuf;
}



#else
/**
 * rd_kafka_snappy_compress - Compress a buffer using the snappy compressor.
 * @env: Preallocated environment
 * @input: Input buffer
 * @input_length: Length of input_buffer
 * @compressed: Output buffer for compressed data
 * @compressed_length: The real length of the output written here.
 *
 * Return 0 on success, otherwise an negative error code.
 *
 * The output buffer must be at least
 * rd_kafka_snappy_max_compressed_length(input_length) bytes long.
 *
 * Requires a preallocated environment from rd_kafka_snappy_init_env.
 * The environment does not keep state over individual calls
 * of this function, just preallocates the memory.
 */
int rd_kafka_snappy_compress(struct snappy_env *env,
		    const char *input,
		    size_t input_length,
		    char *compressed, size_t *compressed_length)
{
	struct source reader = {
		.ptr = input,
		.left = input_length
	};
	struct sink writer = {
		.dest = compressed,
	};
	int err = sn_compress(env, &reader, &writer);

	/* Compute how many bytes were added */
	*compressed_length = (writer.dest - compressed);
	return err;
}
EXPORT_SYMBOL(rd_kafka_snappy_compress);

/**
 * rd_kafka_snappy_uncompress - Uncompress a snappy compressed buffer
 * @compressed: Input buffer with compressed data
 * @n: length of compressed buffer
 * @uncompressed: buffer for uncompressed data
 *
 * The uncompressed data buffer must be at least
 * rd_kafka_snappy_uncompressed_length(compressed) bytes long.
 *
 * Return 0 on success, otherwise an negative error code.
 */
int rd_kafka_snappy_uncompress(const char *compressed, size_t n, char *uncompressed)
{
	struct source reader = {
		.ptr = compressed,
		.left = n
	};
	struct writer output = {
		.base = uncompressed,
		.op = uncompressed
	};
	return internal_uncompress(&reader, &output, 0xffffffff);
}
EXPORT_SYMBOL(rd_kafka_snappy_uncompress);
#endif

static inline void clear_env(struct snappy_env *env)
{
    memset(env, 0, sizeof(*env));
}

#ifdef SG
/**
 * rd_kafka_snappy_init_env_sg - Allocate snappy compression environment
 * @env: Environment to preallocate
 * @sg: Input environment ever does scather gather
 *
 * If false is passed to sg then multiple entries in an iovec
 * are not legal.
 * Returns 0 on success, otherwise negative errno.
 * Must run in process context.
 */
int rd_kafka_snappy_init_env_sg(struct snappy_env *env, bool sg)
{
	if (rd_kafka_snappy_init_env(env) < 0)
		goto error;

	if (sg) {
		env->scratch = vmalloc(kblock_size);
		if (!env->scratch)
			goto error;
		env->scratch_output =
			vmalloc(rd_kafka_snappy_max_compressed_length(kblock_size));
		if (!env->scratch_output)
			goto error;
	}
	return 0;
error:
	rd_kafka_snappy_free_env(env);
	return -ENOMEM;
}
EXPORT_SYMBOL(rd_kafka_snappy_init_env_sg);
#endif

/**
 * rd_kafka_snappy_init_env - Allocate snappy compression environment
 * @env: Environment to preallocate
 *
 * Passing multiple entries in an iovec is not allowed
 * on the environment allocated here.
 * Returns 0 on success, otherwise negative errno.
 * Must run in process context.
 */
int rd_kafka_snappy_init_env(struct snappy_env *env)
{
    clear_env(env);
	env->hash_table = vmalloc(sizeof(u16) * kmax_hash_table_size);
	if (!env->hash_table)
		return -ENOMEM;
	return 0;
}
EXPORT_SYMBOL(rd_kafka_snappy_init_env);

/**
 * rd_kafka_snappy_free_env - Free an snappy compression environment
 * @env: Environment to free.
 *
 * Must run in process context.
 */
void rd_kafka_snappy_free_env(struct snappy_env *env)
{
	vfree(env->hash_table);
#ifdef SG
	vfree(env->scratch);
	vfree(env->scratch_output);
#endif
	clear_env(env);
}
EXPORT_SYMBOL(rd_kafka_snappy_free_env);

#ifdef __GNUC__
#pragma GCC diagnostic pop /* -Wcast-align ignore */
#endif
