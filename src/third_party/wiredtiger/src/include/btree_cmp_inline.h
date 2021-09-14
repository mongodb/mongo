/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifdef HAVE_X86INTRIN_H
#if !defined(_MSC_VER) && !defined(_lint)
#include <x86intrin.h>
#endif
#endif

#if defined(HAVE_ARM_NEON_INTRIN_H)
#include <arm_neon.h>
#endif
/* 16B alignment */
#define WT_ALIGNED_16(p) (((uintptr_t)(p)&0x0f) == 0)
#define WT_VECTOR_SIZE 16 /* chunk size */

/*
 * __wt_lex_compare --
 *     Lexicographic comparison routine. Returns: < 0 if user_item is lexicographically < tree_item,
 *     = 0 if user_item is lexicographically = tree_item, > 0 if user_item is lexicographically >
 *     tree_item. We use the names "user" and "tree" so it's clear in the btree code which the
 *     application is looking at when we call its comparison function. If prefix is specified, 0 can
 *     be returned when the user_item is equal to the tree_item for the minimum size.
 */
static inline int
__wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item, bool prefix)
{
    size_t len, usz, tsz;
    const uint8_t *userp, *treep;

    usz = user_item->size;
    tsz = tree_item->size;
    len = WT_MIN(usz, tsz);

    userp = (const uint8_t *)user_item->data;
    treep = (const uint8_t *)tree_item->data;

#ifdef HAVE_X86INTRIN_H
    /* Use vector instructions if we'll execute at least 2 of them. */
    if (len >= WT_VECTOR_SIZE * 2) {
        size_t remain;
        __m128i res_eq, u, t;

        remain = len % WT_VECTOR_SIZE;
        len -= remain;
        if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
            for (; len > 0;
                 len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
                u = _mm_load_si128((const __m128i *)userp);
                t = _mm_load_si128((const __m128i *)treep);
                res_eq = _mm_cmpeq_epi8(u, t);
                if (_mm_movemask_epi8(res_eq) != 65535)
                    break;
            }
        else
            for (; len > 0;
                 len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
                u = _mm_loadu_si128((const __m128i *)userp);
                t = _mm_loadu_si128((const __m128i *)treep);
                res_eq = _mm_cmpeq_epi8(u, t);
                if (_mm_movemask_epi8(res_eq) != 65535)
                    break;
            }
        len += remain;
    }
#elif defined(HAVE_ARM_NEON_INTRIN_H)
    /* Use vector instructions if we'll execute at least 1 of them. */
    if (len >= WT_VECTOR_SIZE) {
        size_t remain;
        uint8x16_t res_eq, u, t;
        remain = len % WT_VECTOR_SIZE;
        len -= remain;
        for (; len > 0; len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
            u = vld1q_u8(userp);
            t = vld1q_u8(treep);
            res_eq = vceqq_u8(u, t);
            if (vminvq_u8(res_eq) != 255)
                break;
        }
        len += remain;
    }
#endif
    /*
     * Use the non-vectorized version for the remaining bytes and for the small key sizes.
     */
    for (; len > 0; --len, ++userp, ++treep) {
        /*
         * When prefix is enabled and we are performing lexicographic comparison on schema formats s
         * or S, we only want to compare the characters before either of them reach a NUL character.
         * For format S, a NUL character is always at the end of the string, while for the format s,
         * NUL characters are set for the remaining unused bytes. If we are at the end of the user
         * item (which is the prefix here), there is a prefix match. Otherwise, the tree item is
         * lexicographically smaller than the prefix.
         */
        if (prefix && (*userp == '\0' || *treep == '\0'))
            return (*userp == '\0' ? 0 : 1);
        if (*userp != *treep)
            return (*userp < *treep ? -1 : 1);
    }

    /*
     * Contents are equal up to the smallest length. In the case of a prefix match, we consider the
     * tree item and the prefix equal only if the tree item is bigger in size.
     */
    if (usz == tsz || (prefix && usz < tsz))
        return (0);
    return ((usz < tsz) ? -1 : 1);
}

/*
 * __wt_compare --
 *     The same as __wt_lex_compare, but using the application's collator function when configured.
 */
static inline int
__wt_compare(WT_SESSION_IMPL *session, WT_COLLATOR *collator, const WT_ITEM *user_item,
  const WT_ITEM *tree_item, int *cmpp)
{
    if (collator == NULL) {
        *cmpp = __wt_lex_compare(user_item, tree_item, false);
        return (0);
    }
    return (collator->compare(collator, &session->iface, user_item, tree_item, cmpp));
}

/*
 * __wt_prefix_match --
 *     Check if the prefix item is equal to the leading bytes of the tree item.
 */
static inline int
__wt_prefix_match(const WT_ITEM *prefix, const WT_ITEM *tree_item)
{
    return (__wt_lex_compare(prefix, tree_item, true));
}

/*
 * __wt_lex_compare_skip --
 *     Lexicographic comparison routine, skipping leading bytes. Returns: < 0 if user_item is
 *     lexicographically < tree_item = 0 if user_item is lexicographically = tree_item > 0 if
 *     user_item is lexicographically > tree_item We use the names "user" and "tree" so it's clear
 *     in the btree code which the application is looking at when we call its comparison function.
 */
static inline int
__wt_lex_compare_skip(const WT_ITEM *user_item, const WT_ITEM *tree_item, size_t *matchp)
{
    size_t len, usz, tsz;
    const uint8_t *userp, *treep;

    usz = user_item->size;
    tsz = tree_item->size;
    len = WT_MIN(usz, tsz) - *matchp;

    userp = (const uint8_t *)user_item->data + *matchp;
    treep = (const uint8_t *)tree_item->data + *matchp;

#ifdef HAVE_X86INTRIN_H
    /* Use vector instructions if we'll execute at least 2 of them. */
    if (len >= WT_VECTOR_SIZE * 2) {
        size_t remain;
        __m128i res_eq, u, t;

        remain = len % WT_VECTOR_SIZE;
        len -= remain;
        if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
            for (; len > 0; len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
                 *matchp += WT_VECTOR_SIZE) {
                u = _mm_load_si128((const __m128i *)userp);
                t = _mm_load_si128((const __m128i *)treep);
                res_eq = _mm_cmpeq_epi8(u, t);
                if (_mm_movemask_epi8(res_eq) != 65535)
                    break;
            }
        else
            for (; len > 0; len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
                 *matchp += WT_VECTOR_SIZE) {
                u = _mm_loadu_si128((const __m128i *)userp);
                t = _mm_loadu_si128((const __m128i *)treep);
                res_eq = _mm_cmpeq_epi8(u, t);
                if (_mm_movemask_epi8(res_eq) != 65535)
                    break;
            }
        len += remain;
    }
#elif defined(HAVE_ARM_NEON_INTRIN_H)
    /* Use vector instructions if we'll execute  at least 1 of them. */
    if (len >= WT_VECTOR_SIZE) {
        size_t remain;
        uint8x16_t res_eq, u, t;
        remain = len % WT_VECTOR_SIZE;
        len -= remain;
        if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
            for (; len > 0; len -= WT_VECTOR_SIZE, userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
                 *matchp += WT_VECTOR_SIZE) {
                u = vld1q_u8(userp);
                t = vld1q_u8(treep);
                res_eq = vceqq_u8(u, t);
                if (vminvq_u8(res_eq) != 255)
                    break;
            }
        len += remain;
    }
#endif
    /*
     * Use the non-vectorized version for the remaining bytes and for the small key sizes.
     */
    for (; len > 0; --len, ++userp, ++treep, ++*matchp)
        if (*userp != *treep)
            return (*userp < *treep ? -1 : 1);

    /* Contents are equal up to the smallest length. */
    return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

/*
 * __wt_compare_skip --
 *     The same as __wt_lex_compare_skip, but using the application's collator function when
 *     configured.
 */
static inline int
__wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator, const WT_ITEM *user_item,
  const WT_ITEM *tree_item, int *cmpp, size_t *matchp)
{
    if (collator == NULL) {
        *cmpp = __wt_lex_compare_skip(user_item, tree_item, matchp);
        return (0);
    }
    return (collator->compare(collator, &session->iface, user_item, tree_item, cmpp));
}

/*
 * __wt_lex_compare_short --
 *     Lexicographic comparison routine for short keys. Returns: < 0 if user_item is
 *     lexicographically < tree_item = 0 if user_item is lexicographically = tree_item > 0 if
 *     user_item is lexicographically > tree_item We use the names "user" and "tree" so it's clear
 *     in the btree code which the application is looking at when we call its comparison function.
 */
static inline int
__wt_lex_compare_short(const WT_ITEM *user_item, const WT_ITEM *tree_item)
{
    size_t len, usz, tsz;
    const uint8_t *userp, *treep;

    usz = user_item->size;
    tsz = tree_item->size;
    len = WT_MIN(usz, tsz);

    userp = (const uint8_t *)user_item->data;
    treep = (const uint8_t *)tree_item->data;

/*
 * The maximum packed uint64_t is 9B, catch row-store objects using packed record numbers as keys.
 *
 * Don't use a #define to compress this case statement: gcc7 complains about implicit fallthrough
 * and doesn't support explicit fallthrough comments in macros.
 */
#define WT_COMPARE_SHORT_MAXLEN 9
    switch (len) {
    case 9:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 8:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 7:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 6:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 5:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 4:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 3:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 2:
        if (*userp != *treep)
            break;
        ++userp;
        ++treep;
    /* FALLTHROUGH */
    case 1:
        if (*userp != *treep)
            break;

        /* Contents are equal up to the smallest length. */
        return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
    }
    return (*userp < *treep ? -1 : 1);
}
