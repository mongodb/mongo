/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#if defined(_MSC_VER) && (_MSC_VER >= 1300)
#include <stdlib.h>
#define	__wt_bswap16(v)	_byteswap_ushort(v)
#define	__wt_bswap32(v)	_byteswap_ulong(v)
#define	__wt_bswap64(v)	_byteswap_uint64(v)
#elif defined(__clang__) && \
    defined(__clang_major__) && defined(__clang_minor__) && \
    (__clang_major__ >= 3) && (__clang_minor__ >= 1)
#if __has_builtin(__builtin_bswap16)
#define	__wt_bswap16(v)	__builtin_bswap16(v)
#endif
#if __has_builtin(__builtin_bswap32)
#define	__wt_bswap32(v)	__builtin_bswap32(v)
#endif
#if __has_builtin(__builtin_bswap64)
#define	__wt_bswap64(v)	__builtin_bswap64(v)
#endif
#elif defined(__GNUC__) && (__GNUC__ >= 4)
#if __GNUC__ >= 4 && defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 3
#define	__wt_bswap32(v)	__builtin_bswap32(v)
#define	__wt_bswap64(v)	__builtin_bswap64(v)
#endif
#if __GNUC__ >= 4 && defined(__GNUC_MINOR__) && __GNUC_MINOR__ >= 8
#define	__wt_bswap16(v) __builtin_bswap16(v)
#endif
#elif defined(__sun)
#include <sys/byteorder.h>
#define	__wt_bswap16(v)	BSWAP_16(v)
#define	__wt_bswap32(v)	BSWAP_32(v)
#define	__wt_bswap64(v)	BSWAP_64(v)
#endif

#if !defined(__wt_bswap64)
/*
 * __wt_bswap64 --
 *	64-bit unsigned little-endian to/from big-endian value.
 */
static inline uint64_t
__wt_bswap64(uint64_t v)
{
	return (
	    ((v << 56) & 0xff00000000000000UL) |
	    ((v << 40) & 0x00ff000000000000UL) |
	    ((v << 24) & 0x0000ff0000000000UL) |
	    ((v <<  8) & 0x000000ff00000000UL) |
	    ((v >>  8) & 0x00000000ff000000UL) |
	    ((v >> 24) & 0x0000000000ff0000UL) |
	    ((v >> 40) & 0x000000000000ff00UL) |
	    ((v >> 56) & 0x00000000000000ffUL)
	);
}
#endif

#if !defined(__wt_bswap32)
/*
 * __wt_bswap32 --
 *	32-bit unsigned little-endian to/from big-endian value.
 */
static inline uint32_t
__wt_bswap32(uint32_t v)
{
	return (
	    ((v << 24) & 0xff000000) |
	    ((v <<  8) & 0x00ff0000) |
	    ((v >>  8) & 0x0000ff00) |
	    ((v >> 24) & 0x000000ff)
	);
}
#endif

#if !defined(__wt_bswap16)
/*
 * __wt_bswap16 --
 *	16-bit unsigned little-endian to/from big-endian value.
 */
static inline uint16_t
__wt_bswap16(uint16_t v)
{
	return (
	    ((v << 8) & 0xff00) |
	    ((v >> 8) & 0x00ff)
	);
}
#endif
