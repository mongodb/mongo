/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
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
 * First byte  | Next |                        |
 * byte        | bytes| Min Value              | Max Value
 * ------------+------+------------------------+--------------------------------
 * [00 00xxxx] | free | N/A                    | N/A
 * [00 01llll] | llll | -2^64                  | -2^13 - 2^6
 * [00 1xxxxx] | 1    | -2^13 - 2^6            | -2^6 - 1
 * [01 xxxxxx] | 0    | -2^6                   | -1
 * [10 xxxxxx] | 0    | 0                      | 2^6 - 1
 * [11 0xxxxx] | 1    | 2^6                    | 2^13 + 2^6 - 1
 * [11 10llll] | llll | 2^13 + 2^6             | 2^64 - 1
 * [11 11xxxx] | free | N/A                    | N/A
 */

#define NEG_MULTI_MARKER (uint8_t)0x10
#define NEG_2BYTE_MARKER (uint8_t)0x20
#define NEG_1BYTE_MARKER (uint8_t)0x40
#define POS_1BYTE_MARKER (uint8_t)0x80
#define POS_2BYTE_MARKER (uint8_t)0xc0
#define POS_MULTI_MARKER (uint8_t)0xe0

#define NEG_1BYTE_MIN (-(1 << 6))
#define NEG_2BYTE_MIN (-(1 << 13) + NEG_1BYTE_MIN)
#define POS_1BYTE_MAX ((1 << 6) - 1)
#define POS_2BYTE_MAX ((1 << 13) + POS_1BYTE_MAX)

/* Extract bits <start> to <end> from a value (counting from LSB == 0). */
#define GET_BITS(x, start, end) (((uint64_t)(x) & ((1U << (start)) - 1U)) >> (end))

/*
 * Size checks: return ENOMEM if not enough room when writing, EINVAL if the length is wrong when
 * reading (presumably the value is corrupted).
 */
#define WT_SIZE_CHECK_PACK(l, maxl) WT_RET_TEST((maxl) != 0 && (size_t)(l) > (maxl), ENOMEM)
#define WT_SIZE_CHECK_UNPACK(l, maxl) WT_RET_TEST((maxl) != 0 && (size_t)(l) > (maxl), EINVAL)

/* Count the leading zero bytes. */
#if defined(__GNUC__)
#define WT_LEADING_ZEROS(x, i) ((i) = ((x) == 0) ? (int)sizeof(x) : __builtin_clzll(x) >> 3)
#elif defined(_MSC_VER)
#define WT_LEADING_ZEROS(x, i)              \
    do {                                    \
        if ((x) == 0)                       \
            (i) = (int)sizeof(x);           \
        else {                              \
            unsigned long __index;          \
            _BitScanReverse64(&__index, x); \
            __index = 63 ^ __index;         \
            (i) = (int)(__index >> 3);      \
        }                                   \
    } while (0)
#else
#define WT_LEADING_ZEROS(x, i)                         \
    do {                                               \
        uint64_t __x = (x);                            \
        uint64_t __m = (uint64_t)0xff << 56;           \
        for ((i) = 0; !(__x & __m) && (i) != 8; (i)++) \
            __m >>= 8;                                 \
    } while (0)
#endif

/*
 * __wt_vpack_posint --
 *     Packs a positive variable-length integer in the specified location.
 */
static inline int
__wt_vpack_posint(uint8_t **pp, size_t maxlen, uint64_t x)
{
    uint8_t *p;
    int len, lz, shift;

    WT_LEADING_ZEROS(x, lz);
    len = (int)sizeof(x) - lz;
    WT_SIZE_CHECK_PACK(len + 1, maxlen);
    p = *pp;

    /* There are four bits we can use in the first byte. */
    *p++ |= (len & 0xf);

    for (shift = (len - 1) << 3; len != 0; --len, shift -= 8)
        *p++ = (uint8_t)(x >> shift);

    *pp = p;
    return (0);
}

/*
 * __wt_vpack_negint --
 *     Packs a negative variable-length integer in the specified location.
 */
static inline int
__wt_vpack_negint(uint8_t **pp, size_t maxlen, uint64_t x)
{
    uint8_t *p;
    int len, lz, shift;

    WT_LEADING_ZEROS(~x, lz);
    len = (int)sizeof(x) - lz;
    WT_SIZE_CHECK_PACK(len + 1, maxlen);
    p = *pp;

    /*
     * There are four size bits we can use in the first byte. For negative numbers, we store the
     * number of leading 0xff bytes to maintain ordering (if this is not obvious, it may help to
     * remember that -1 is the largest negative number).
     */
    *p++ |= (lz & 0xf);

    for (shift = (len - 1) << 3; len != 0; shift -= 8, --len)
        *p++ = (uint8_t)(x >> shift);

    *pp = p;
    return (0);
}

/*
 * __wt_vunpack_posint --
 *     Reads a variable-length positive integer from the specified location.
 */
static inline int
__wt_vunpack_posint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
{
    uint64_t x;
    uint8_t len;
    const uint8_t *p;

    /* There are four length bits in the first byte. */
    p = *pp;
    len = (*p++ & 0xf);
    WT_SIZE_CHECK_UNPACK(len + 1, maxlen);

    for (x = 0; len != 0; --len)
        x = (x << 8) | *p++;

    *retp = x;
    *pp = p;
    return (0);
}

/*
 * __wt_vunpack_negint --
 *     Reads a variable-length negative integer from the specified location.
 */
static inline int
__wt_vunpack_negint(const uint8_t **pp, size_t maxlen, uint64_t *retp)
{
    uint64_t x;
    uint8_t len;
    const uint8_t *p;

    /* There are four length bits in the first byte. */
    p = *pp;
    len = (int)sizeof(x) - (*p++ & 0xf);
    WT_SIZE_CHECK_UNPACK(len + 1, maxlen);

    for (x = UINT64_MAX; len != 0; --len)
        x = (x << 8) | *p++;

    *retp = x;
    *pp = p;
    return (0);
}

/*
 * __wt_vpack_uint --
 *     Variable-sized packing for unsigned integers
 */
static inline int
__wt_vpack_uint(uint8_t **pp, size_t maxlen, uint64_t x)
{
    uint8_t *p;

    WT_SIZE_CHECK_PACK(1, maxlen);
    p = *pp;
    if (x <= POS_1BYTE_MAX)
        *p++ = POS_1BYTE_MARKER | GET_BITS(x, 6, 0);
    else if (x <= POS_2BYTE_MAX) {
        WT_SIZE_CHECK_PACK(2, maxlen);
        x -= POS_1BYTE_MAX + 1;
        *p++ = POS_2BYTE_MARKER | GET_BITS(x, 13, 8);
        *p++ = GET_BITS(x, 8, 0);
    } else if (x == POS_2BYTE_MAX + 1) {
        /*
         * This is a special case where we could store the value with just a single byte, but we
         * append a zero byte so that the encoding doesn't get shorter for this one value.
         */
        *p++ = POS_MULTI_MARKER | 0x1;
        *p++ = 0;
    } else {
        x -= POS_2BYTE_MAX + 1;
        *p = POS_MULTI_MARKER;
        return (__wt_vpack_posint(pp, maxlen, x));
    }

    *pp = p;
    return (0);
}

/*
 * __wt_vpack_int --
 *     Variable-sized packing for signed integers
 */
static inline int
__wt_vpack_int(uint8_t **pp, size_t maxlen, int64_t x)
{
    uint8_t *p;

    WT_SIZE_CHECK_PACK(1, maxlen);
    p = *pp;
    if (x < NEG_2BYTE_MIN) {
        *p = NEG_MULTI_MARKER;
        return (__wt_vpack_negint(pp, maxlen, (uint64_t)x));
    }
    if (x < NEG_1BYTE_MIN) {
        WT_SIZE_CHECK_PACK(2, maxlen);
        x -= NEG_2BYTE_MIN;
        *p++ = NEG_2BYTE_MARKER | GET_BITS(x, 13, 8);
        *p++ = GET_BITS(x, 8, 0);
    } else if (x < 0) {
        x -= NEG_1BYTE_MIN;
        *p++ = NEG_1BYTE_MARKER | GET_BITS(x, 6, 0);
    } else
        /* For non-negative values, use the unsigned code above. */
        return (__wt_vpack_uint(pp, maxlen, (uint64_t)x));

    *pp = p;
    return (0);
}

/*
 * __wt_vunpack_uint --
 *     Variable-sized unpacking for unsigned integers
 */
static inline int
__wt_vunpack_uint(const uint8_t **pp, size_t maxlen, uint64_t *xp)
{
    const uint8_t *p;

    WT_SIZE_CHECK_UNPACK(1, maxlen);
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
        WT_SIZE_CHECK_UNPACK(2, maxlen);
        *xp = GET_BITS(*p++, 5, 0) << 8;
        *xp |= *p++;
        *xp += POS_1BYTE_MAX + 1;
        break;
    case POS_MULTI_MARKER:
        WT_RET(__wt_vunpack_posint(pp, maxlen, xp));
        *xp += POS_2BYTE_MAX + 1;
        return (0);
    default:
        return (EINVAL);
    }

    *pp = p;
    return (0);
}

/*
 * __wt_vunpack_int --
 *     Variable-sized packing for signed integers
 */
static inline int
__wt_vunpack_int(const uint8_t **pp, size_t maxlen, int64_t *xp)
{
    const uint8_t *p;

    WT_SIZE_CHECK_UNPACK(1, maxlen);
    p = *pp;
    switch (*p & 0xf0) {
    case NEG_MULTI_MARKER:
        WT_RET(__wt_vunpack_negint(pp, maxlen, (uint64_t *)xp));
        return (0);
    case NEG_2BYTE_MARKER:
    case NEG_2BYTE_MARKER | 0x10:
        WT_SIZE_CHECK_UNPACK(2, maxlen);
        *xp = (int64_t)(GET_BITS(*p++, 5, 0) << 8);
        *xp |= *p++;
        *xp += NEG_2BYTE_MIN;
        break;
    case NEG_1BYTE_MARKER:
    case NEG_1BYTE_MARKER | 0x10:
    case NEG_1BYTE_MARKER | 0x20:
    case NEG_1BYTE_MARKER | 0x30:
        *xp = NEG_1BYTE_MIN + (int64_t)GET_BITS(*p, 6, 0);
        p += 1;
        break;
    default:
        /* Identical to the unsigned case. */
        return (__wt_vunpack_uint(pp, maxlen, (uint64_t *)xp));
    }

    *pp = p;
    return (0);
}

/*
 * __wt_vsize_posint --
 *     Return the packed size of a positive variable-length integer.
 */
static inline size_t
__wt_vsize_posint(uint64_t x)
{
    int lz;

    WT_LEADING_ZEROS(x, lz);
    return ((size_t)(WT_INTPACK64_MAXSIZE - lz));
}

/*
 * __wt_vsize_negint --
 *     Return the packed size of a negative variable-length integer.
 */
static inline size_t
__wt_vsize_negint(uint64_t x)
{
    int lz;

    WT_LEADING_ZEROS(~x, lz);
    return (size_t)(WT_INTPACK64_MAXSIZE - lz);
}

/*
 * __wt_vsize_uint --
 *     Return the packed size of an unsigned integer.
 */
static inline size_t
__wt_vsize_uint(uint64_t x)
{
    if (x <= POS_1BYTE_MAX)
        return (1);
    if (x <= POS_2BYTE_MAX + 1)
        return (2);
    x -= POS_2BYTE_MAX + 1;
    return (__wt_vsize_posint(x));
}

/*
 * __wt_vsize_int --
 *     Return the packed size of a signed integer.
 */
static inline size_t
__wt_vsize_int(int64_t x)
{
    if (x < NEG_2BYTE_MIN)
        return (__wt_vsize_negint((uint64_t)x));
    if (x < NEG_1BYTE_MIN)
        return (2);
    if (x < 0)
        return (1);
    /* For non-negative values, use the unsigned code above. */
    return (__wt_vsize_uint((uint64_t)x));
}
