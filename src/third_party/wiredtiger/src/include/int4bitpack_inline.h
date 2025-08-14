/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-present WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*!!!
 *    Functions for Golomb-like small integer encoding and decoding into 4-bit chunks.
 *
 *    The packing format is:
 *    - The encoded data is split into 4-bit chunks: F-v-v-v.
 *    - If the first bit (MSB) is 0, this is the last chunk.
 *    - If the MSB is 1, the next chunk is a part of the same number.
 *    - The remaining 3 bits encode the value.
 *    - For chunks beyond the first, the actual value is one greater than what's decoded.
 *    - The sequence is LSB-first, meaning the least significant chunk comes first.
 *      * Rationale: simplifies encoding and decoding.
 *    - The low half of the byte holds the first chunk, the high half holds the second.
 *      * Rationale: small integers encode to their own value making debugging easier.
 */

/*
 * Internal functions and structures.
 */

typedef struct __4b_pack_context {
    uint8_t **pp;
    uint8_t *end;
    uint8_t *start; /* start of output buffer for safety when updating high nibble */
    int nibble; /* 0: next write to low nibble (append new byte), 1: next write to high nibble */
} WT_4B_PACK_CONTEXT;

typedef struct __4b_unpack_context {
    const uint8_t **pp;
    const uint8_t *end;
    const uint8_t *start; /* start of input buffer for safety */
    uint64_t result;
    int shift;
    int nibble; /* 0: next read from low nibble of current byte, 1: next read from high nibble */
} WT_4B_UNPACK_CONTEXT;

/*
 * __4b_pack_init --
 *     Internal helpers: initialize context.
 */
static WT_INLINE void
__4b_pack_init(WT_4B_PACK_CONTEXT *ctx, uint8_t **pp, uint8_t *end)
{
    ctx->pp = pp;
    ctx->end = end;
    ctx->start = *pp;
    ctx->nibble = 0; /* start aligned: low nibble first */
}

/*
 * __4b_unpack_init --
 *     Internal helpers: initialize context.
 */
static WT_INLINE void
__4b_unpack_init(WT_4B_UNPACK_CONTEXT *ctx, const uint8_t **pp, const uint8_t *end)
{
    ctx->pp = pp;
    ctx->end = end;
    ctx->start = *pp;
    ctx->result = 0;
    ctx->shift = 0;
    ctx->nibble = 0; /* start aligned: low nibble first */
}

/*
 * __4b_pack_put_chunk --
 *     Write a 4-bit chunk into the buffer managed by the pack context.
 */
static WT_INLINE int
__4b_pack_put_chunk(WT_4B_PACK_CONTEXT *ctx, uint8_t chunk)
{
    chunk &= 0x0f;
    if (ctx->nibble == 0) {
        /* Need a fresh byte for the low nibble. */
        if (ctx->end != NULL && *ctx->pp >= ctx->end)
            return (ENOMEM);
        *(*ctx->pp) = chunk;
        (*ctx->pp)++;
        ctx->nibble = 1;
    } else {
        /* Update the previously appended byte's high nibble. */
        if (*ctx->pp == ctx->start)
            return (EINVAL); /* should not happen */
        *(*ctx->pp - 1) = (uint8_t)((*(*ctx->pp - 1)) | (uint8_t)(chunk << 4));
        ctx->nibble = 0;
    }
    return (0);
}

/*
 * __4b_unpack_get_chunk --
 *     Read a 4-bit chunk from the buffer managed by the unpack context. Returns 0 on success,
 *     EINVAL if there is no more input.
 */
static WT_INLINE int
__4b_unpack_get_chunk(WT_4B_UNPACK_CONTEXT *ctx, uint8_t *chunk)
{
    const uint8_t *p = *ctx->pp;
    if (ctx->end != NULL && p >= ctx->end)
        return (EINVAL);
    if (ctx->nibble == 0) {
        *chunk = (uint8_t)(*p & 0x0f);
        ctx->nibble = 1;
    } else {
        *chunk = (uint8_t)((*p >> 4) & 0x0f);
        ctx->nibble = 0;
        /* Advance to the next byte after consuming the high nibble. */
        (*ctx->pp)++;
        return (0);
    }
    /* For low nibble, we do not advance the pointer yet. */
    return (0);
}

/*
 * __4b_pack_posint_ctx --
 *     Encode one positive integer into the provided context.
 */
static WT_INLINE int
__4b_pack_posint_ctx(WT_4B_PACK_CONTEXT *ctx, uint64_t x)
{
    for (;;) {
        uint8_t chunk = (uint8_t)(x & 0x7U);
        x >>= 3;
        if (x != 0)
            chunk |= 0x8U; /* continuation */
        WT_RET(__4b_pack_put_chunk(ctx, chunk));
        if (x == 0)
            break;
        x -= 1; /* defer one for subsequent "+1 on decode" */
    }
    return (0);
}

/*
 * __4b_unpack_posint_ctx --
 *     Decode one positive integer from the provided context.
 */
static WT_INLINE int
__4b_unpack_posint_ctx(WT_4B_UNPACK_CONTEXT *ctx, uint64_t *out)
{
    uint64_t n = 0;
    int shift = 0;

    for (;;) {
        uint8_t chunk;
        if (__4b_unpack_get_chunk(ctx, &chunk) != 0)
            return (EINVAL);
        uint64_t val = (uint64_t)(chunk & 0x7U);
        if (shift != 0)
            val += 1;        /* carry from saved "+1 on decode" rule */
        n += (val << shift); /* use + to allow carry */
        shift += 3;
        if ((chunk & 0x8U) == 0)
            break; /* last chunk */
    }
    *out = n;

    return (0);
}

/*
 * __4b_nibbles_for_posint --
 *     Compute number of nibbles needed to encode a single positive integer.
 */
static WT_INLINE size_t
__4b_nibbles_for_posint(uint64_t x)
{
    size_t nibbles = 0;
    for (;;) {
        uint64_t nxt = x >> 3; /* consume 3 LSBs */
        ++nibbles;
        if (nxt == 0)
            break;
        x = nxt - 1; /* save one for the "+1 on decode" rule for subsequent chunks */
    }
    return (nibbles);
}

/*!!!
 * Interface functions.
 *
 * We're using functions with specific typed arguments rather than "..." for type safety.
 *
 * Possible extensions:
 * - Add functions for packing and unpacking more numbers.
 * - Add functions for packing and unpacking arrays
 *   (see encode_array() and decode_array() in test/packing/int4bpack-test.c).
 * - Optional: Add support for negative integers.
 */

/*
 * __wt_4b_pack_posint1 --
 *     Packs 1 positive variable-length integer in the specified location.
 */
static WT_INLINE int
__wt_4b_pack_posint1(uint8_t **pp, uint8_t *end, uint64_t x1)
{
    WT_4B_PACK_CONTEXT ctx;

    __4b_pack_init(&ctx, pp, end);
    WT_RET(__4b_pack_posint_ctx(&ctx, x1));
    return (0);
}

/*
 * __wt_4b_pack_posint2 --
 *     Packs 2 positive variable-length integers in the specified location.
 */
static WT_INLINE int
__wt_4b_pack_posint2(uint8_t **pp, uint8_t *end, uint64_t x1, uint64_t x2)
{
    WT_4B_PACK_CONTEXT ctx;

    __4b_pack_init(&ctx, pp, end);
    WT_RET(__4b_pack_posint_ctx(&ctx, x1));
    WT_RET(__4b_pack_posint_ctx(&ctx, x2));
    return (0);
}

/*
 * __wt_4b_unpack_posint1 --
 *     Unpacks 1 positive variable-length integer from the specified location.
 */
static WT_INLINE int
__wt_4b_unpack_posint1(const uint8_t **pp, const uint8_t *end, uint64_t *x1)
{
    WT_4B_UNPACK_CONTEXT ctx;

    __4b_unpack_init(&ctx, pp, end);
    WT_RET(__4b_unpack_posint_ctx(&ctx, x1));
    return (0);
}

/*
 * __wt_4b_unpack_posint2 --
 *     Unpacks 2 positive variable-length integers from the specified location.
 */
static WT_INLINE int
__wt_4b_unpack_posint2(const uint8_t **pp, const uint8_t *end, uint64_t *x1, uint64_t *x2)
{
    WT_4B_UNPACK_CONTEXT ctx;

    __4b_unpack_init(&ctx, pp, end);
    WT_RET(__4b_unpack_posint_ctx(&ctx, x1));
    WT_RET(__4b_unpack_posint_ctx(&ctx, x2));
    return (0);
}

/*
 * __wt_4b_size_posint1 --
 *     Return the packed size of 1 unsigned integer in bytes.
 */
static WT_INLINE size_t
__wt_4b_size_posint1(uint64_t x1)
{
    return (__4b_nibbles_for_posint(x1) + 1) >> 1; /* ceil(n1 / 2) */
}

/*
 * __wt_4b_size_posint2 --
 *     Return the packed size of 2 unsigned integers in bytes.
 */
static WT_INLINE size_t
__wt_4b_size_posint2(uint64_t x1, uint64_t x2)
{
    return (__4b_nibbles_for_posint(x1) + __4b_nibbles_for_posint(x2) + 1) >>
      1; /* ceil((n1+n2)/2) */
}

/*
 * __wt_encode_signed_as_positive --
 *     Encode signed integers as positive using a zigzag-like scheme.
 */
static WT_INLINE uint64_t
__wt_encode_signed_as_positive(int64_t x)
{
    return ((x < 0) ? ((((uint64_t)(-(x + 1))) << 1) | 1U) : ((uint64_t)x << 1));
}

/*
 * __wt_decode_positive_as_signed --
 *     Decode signed integers as positive using a zigzag-like scheme.
 */
static WT_INLINE int64_t
__wt_decode_positive_as_signed(uint64_t x)
{
    return ((x & 1U) ? (-(int64_t)(x >> 1) - 1) : (int64_t)(x >> 1));
}
