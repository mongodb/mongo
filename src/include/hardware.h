/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Publish a value to a shared location.  All previous stores must complete
 * before the value is made public.
 */
#define	WT_PUBLISH(v, val) do {						\
	WT_WRITE_BARRIER();						\
	(v) = (val);							\
} while (0)

/*
 * Read a shared location and guarantee that subsequent reads do not see any
 * earlier state.
 */
#define	WT_ORDERED_READ(v, val) do {					\
	(v) = (val);							\
	WT_READ_BARRIER();						\
} while (0)

/*
 * Atomic versions of the flag set/clear macros.
 */
#define	F_ISSET_ATOMIC(p, mask)	((p)->flags_atomic & (uint8_t)(mask))

#define	F_SET_ATOMIC(p, mask) do {					\
	uint8_t __orig;							\
	do {								\
		__orig = (p)->flags_atomic;				\
	} while (!__wt_atomic_cas8(					\
	    &(p)->flags_atomic, __orig, __orig | (uint8_t)(mask)));	\
} while (0)

#define	F_CLR_ATOMIC(p, mask)	do {					\
	uint8_t __orig;							\
	do {								\
		__orig = (p)->flags_atomic;				\
	} while (!__wt_atomic_cas8(					\
	    &(p)->flags_atomic, __orig, __orig & ~(uint8_t)(mask)));	\
} while (0)

#define	WT_CACHE_LINE_ALIGNMENT	64	/* Cache line alignment */
#define	WT_CACHE_LINE_ALIGNMENT_VERIFY(session, a)			\
	WT_ASSERT(session,						\
	    WT_PTRDIFF(&(a)[1], &(a)[0]) >= WT_CACHE_LINE_ALIGNMENT &&	\
	    WT_PTRDIFF(&(a)[1], &(a)[0]) % WT_CACHE_LINE_ALIGNMENT == 0)

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
