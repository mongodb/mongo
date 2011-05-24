/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * Variable-length integer encoding.
 * We need up to 64 bits, signed and unsigned.  Further, we want the packed
 * representation to have the same lexicographic ordering as the integer
 * values.  This avoids the need for special-purpose comparison code.
 *
 * Try hard to keep small values small (up to ~2 bytes): that gives the biggest
 * benefit for common cases storing small values.  After that, just encode the
 * length in the first byte: we could squeeze in a couple of extra bits, but
 * the marginal benefit is small, and we want this code to be relatively
 * easy to implement in client code or scripting APIs.
 *
 * First byte | Next |                        |
 * byte       | bytes| Min Value              | Max Value
 * ------------+------+------------------------+--------------------------------
 * [00 00xxxx] | free | N/A                    | N/A
 * [00 01llll] | llll | -2^64                  | -2^13 - 2^6
 * [00 1xxxxx] | 1    | -2^13 - 2^6            | -2^6 - 1
 * [01 xxxxxx] | 0    | -2^6                   | -1
 * [10 xxxxxx] | 0    | 0                      | 2^6 - 1
 * [11 0xxxxx] | 1    | 2^6                    | 2^13 + 2^6 - 1
 * [11 10llll] | llll | 2^14 + 2^7             | 2^64 - 1
 * [11 11xxxx] | free | N/A                    | N/A
 */

#define	NEG_MULTI_MARKER (uint8_t)0x10
#define	NEG_2BYTE_MARKER (uint8_t)0x20
#define	NEG_1BYTE_MARKER (uint8_t)0x40
#define	POS_1BYTE_MARKER (uint8_t)0x80
#define	POS_2BYTE_MARKER (uint8_t)0xc0
#define	POS_MULTI_MARKER (uint8_t)0xe0

#define	NEG_1BYTE_MIN ((-1) << 6)
#define	NEG_2BYTE_MIN (((-1) << 13) + NEG_1BYTE_MIN)
#define	POS_1BYTE_MAX ((1 << 6) - 1)
#define	POS_2BYTE_MAX ((1 << 13) + POS_1BYTE_MAX)

#define	GET_BITS(x, start, end) (((x) & ((1 << (start)) - 1)) >> (end))

/*
 * __wt_pack_bigint --
 *      Packs larger variable-length integer in the specified location.
 */
static inline int
__wt_pack_bigint(SESSION *session, uint8_t **pp, size_t maxlen, uint64_t x)
{
	uint8_t *p;
	int len, shift;

	p = *pp;

	for (shift = 56, len = 8; len != 0; shift -= 8, --len)
		if (x >> shift != 0)
			break;

	/* There are four bits we can use in the first byte. */
	*p++ |= (len & 0xf);

	for (; len != 0; shift -= 8, --len)
		*p++ = (x >> shift);

	WT_ASSERT(session, (size_t)(p - *pp) < maxlen);
	*pp = p;
	return (0);
}

/*
 * __wt_unpack_bigint --
 *      Reads a larger, variable-length integer from the specified location.
 */
static inline int
__wt_unpack_bigint(
    SESSION *session, uint8_t **pp, size_t maxlen, uint64_t *retp)
{
	uint64_t x;
	uint8_t *p;
	uint8_t len;

	p = *pp;

	/* There are four length bits in the first byte. */
	len = (*p++ & 0xf);

	for (x = 0; len != 0; --len, ++p)
		x = (x << 8) | *p;

	*retp = x;
	WT_ASSERT(session, (size_t)(p - *pp) < maxlen);
	*pp = p;
	return (0);
}

/*
 * __wt_vpack_uint
 *      Variable-sized packing for unsigned integers
 */
static inline int
__wt_vpack_uint(SESSION *session, uint8_t **pp, size_t maxlen, uint64_t x)
{
	uint8_t *p;

	p = *pp;
	if (x <= POS_1BYTE_MAX)
		*p++ = POS_1BYTE_MARKER | GET_BITS(x, 6, 0);
	else if (x <= POS_2BYTE_MAX) {
		x -= POS_1BYTE_MAX + 1;
		*p++ = POS_2BYTE_MARKER | GET_BITS(x, 13, 8);
		*p++ = GET_BITS(x, 8, 0);
	} else {
		*p = POS_MULTI_MARKER;
		return (__wt_pack_bigint(session, pp, maxlen, x));
	}

	WT_ASSERT(session, (size_t)(p - *pp) < maxlen);
	*pp = p;
	return (0);
}

/*
 * __wt_vpack_int
 *      Variable-sized packing for signed integers
 */
static inline int
__wt_vpack_int(SESSION *session, uint8_t **pp, size_t maxlen, int64_t x)
{
	uint8_t *p;

	p = *pp;
	if (x < NEG_2BYTE_MIN) {
		*p = NEG_MULTI_MARKER;
		return (__wt_pack_bigint(session, pp, maxlen, x - INT64_MIN));
	} else if (x < NEG_1BYTE_MIN) {
		x -= NEG_2BYTE_MIN;
		*p++ = NEG_2BYTE_MARKER | GET_BITS(x, 13, 8);
		*p++ = GET_BITS(x, 8, 0);
	} else if (x < 0) {
		x -= NEG_1BYTE_MIN;
		*p++ = NEG_1BYTE_MARKER | GET_BITS(x, 6, 0);
	} else
		/* For non-negative values, use the unsigned code above. */
		return (__wt_vpack_uint(session, pp, maxlen, (uint64_t)x));

	WT_ASSERT(session, (size_t)(p - *pp) < maxlen);
	*pp = p;
	return (0);
}

/*
 * __wt_vunpack_uint
 *      Variable-sized unpacking for unsigned integers
 */
static inline int
__wt_vunpack_uint(SESSION *session, uint8_t **pp, size_t maxlen, uint64_t *xp)
{
	uint8_t *p;

	p = *pp;
	switch (*p & 0xf0) {
	case POS_1BYTE_MARKER:
	case POS_1BYTE_MARKER | 0x10:
	case POS_1BYTE_MARKER | 0x20:
	case POS_1BYTE_MARKER | 0x30:
		*xp = GET_BITS(*p, 6, 0);
		p += 1;
		break;
	case POS_2BYTE_MARKER:
	case POS_2BYTE_MARKER | 0x10:
		*xp = POS_1BYTE_MAX + 1 + ((GET_BITS(*p, 5, 0) << 8) | p[1]);
		p += 2;
		break;
	case POS_MULTI_MARKER:
		return (__wt_unpack_bigint(session, pp, maxlen, xp));
	default:
		WT_ASSERT(session, *p != *p);
		return (EINVAL);
	}

	*pp = p;
	return (0);
}

/*
 * __wt_vunpack_int
 *      Variable-sized packing for signed integers
 */
static inline int
__wt_vunpack_int(SESSION *session, uint8_t **pp, size_t maxlen, int64_t *xp)
{
	uint8_t *p;

	p = *pp;
	switch (*p & 0xf0) {
	case NEG_MULTI_MARKER:
		WT_RET(__wt_unpack_bigint(session, pp, maxlen, (uint64_t *)xp));
		*xp += INT64_MIN;
		return (0);
	case NEG_2BYTE_MARKER:
	case NEG_2BYTE_MARKER | 0x10:
		*xp = NEG_2BYTE_MIN + ((GET_BITS(*p, 5, 0) << 8) | p[1]);
		p += 2;
		break;
	case NEG_1BYTE_MARKER:
	case NEG_1BYTE_MARKER | 0x10:
	case NEG_1BYTE_MARKER | 0x20:
	case NEG_1BYTE_MARKER | 0x30:
		*xp = NEG_1BYTE_MIN + GET_BITS(*p, 6, 0);
		p += 1;
		break;
	default:
		/* Identical to the unsigned case. */
		return (__wt_vunpack_uint(session,
		    pp, maxlen, (uint64_t *)xp));
	}

	WT_ASSERT(session, (size_t)(p - *pp) < maxlen);
	*pp = p;
	return (0);
}
