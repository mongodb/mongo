/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#if defined(HAVE_X86INTRIN_H)
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
 *     application is looking at when we call its comparison function.
 */
static WT_INLINE int
__wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item)
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
    for (; len > 0; --len, ++userp, ++treep)
        if (*userp != *treep)
            return (*userp < *treep ? -1 : 1);

    /* Contents are equal up to the smallest length. */
    return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

/*
 * __wt_compare --
 *     The same as __wt_lex_compare, but using the application's collator function when configured.
 */
static WT_INLINE int
__wt_compare(WT_SESSION_IMPL *session, WT_COLLATOR *collator, const WT_ITEM *user_item,
  const WT_ITEM *tree_item, int *cmpp)
{
    if (collator == NULL) {
        *cmpp = __wt_lex_compare(user_item, tree_item);
        return (0);
    }
    return (collator->compare(collator, &session->iface, user_item, tree_item, cmpp));
}

/*
 * __wt_compare_bounds --
 *     Return if the cursor key is within the bounded range. If upper is True, this indicates a next
 *     call and the key is checked against the upper bound. If upper is False, this indicates a prev
 *     call and the key is then checked against the lower bound.
 */
static WT_INLINE int
__wt_compare_bounds(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_ITEM *key, uint64_t recno,
  bool upper, bool *key_out_of_bounds)
{
    uint64_t recno_bound;
    int cmpp;

    cmpp = 0;
    recno_bound = 0;

    WT_STAT_CONN_DSRC_INCR(session, cursor_bounds_comparisons);

    if (upper) {
        WT_ASSERT(session, WT_DATA_IN_ITEM(&cursor->upper_bound));
        if (CUR2BT(cursor)->type == BTREE_ROW)
            WT_RET(
              __wt_compare(session, CUR2BT(cursor)->collator, key, &cursor->upper_bound, &cmpp));
        else
            /* Unpack the raw recno buffer into integer variable. */
            WT_RET(__wt_struct_unpack(
              session, cursor->upper_bound.data, cursor->upper_bound.size, "q", &recno_bound));

        if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER_INCLUSIVE))
            *key_out_of_bounds =
              CUR2BT(cursor)->type == BTREE_ROW ? (cmpp > 0) : (recno > recno_bound);
        else
            *key_out_of_bounds =
              CUR2BT(cursor)->type == BTREE_ROW ? (cmpp >= 0) : (recno >= recno_bound);
    } else {
        WT_ASSERT(session, WT_DATA_IN_ITEM(&cursor->lower_bound));
        if (CUR2BT(cursor)->type == BTREE_ROW)
            WT_RET(
              __wt_compare(session, CUR2BT(cursor)->collator, key, &cursor->lower_bound, &cmpp));
        else
            /* Unpack the raw recno buffer into integer variable. */
            WT_RET(__wt_struct_unpack(
              session, cursor->lower_bound.data, cursor->lower_bound.size, "q", &recno_bound));

        if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER_INCLUSIVE))
            *key_out_of_bounds =
              CUR2BT(cursor)->type == BTREE_ROW ? (cmpp < 0) : (recno < recno_bound);
        else
            *key_out_of_bounds =
              CUR2BT(cursor)->type == BTREE_ROW ? (cmpp <= 0) : (recno <= recno_bound);
    }
    return (0);
}

/*
 * __wt_lex_compare_skip --
 *     Lexicographic comparison routine, skipping leading bytes. Returns: < 0 if user_item is
 *     lexicographically < tree_item = 0 if user_item is lexicographically = tree_item > 0 if
 *     user_item is lexicographically > tree_item We use the names "user" and "tree" so it's clear
 *     in the btree code which the application is looking at when we call its comparison function.
 */
static WT_INLINE int
__wt_lex_compare_skip(
  WT_SESSION_IMPL *session, const WT_ITEM *user_item, const WT_ITEM *tree_item, size_t *matchp)
{
    size_t len, usz, tsz;
    const uint8_t *userp, *treep;
    int ret_val;

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
    ret_val = 0;
    /*
     * Use the non-vectorized version for the remaining bytes and for the small key sizes.
     */
    for (; len > 0; --len, ++userp, ++treep, ++*matchp)
        if (*userp != *treep) {
            ret_val = *userp < *treep ? -1 : 1;
            break;
        }

    /* Contents are equal up to the smallest length. */
    if (ret_val == 0)
        ret_val = ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);

#ifdef HAVE_DIAGNOSTIC
    /*
     * There are various optimizations in the code to skip comparing prefixes that are known to be
     * the same. If configured, check that the prefixes actually match.
     */
    if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_PREFIX_COMPARE)) {
        int full_cmp_ret;
        full_cmp_ret = __wt_lex_compare(user_item, tree_item);
        WT_ASSERT_ALWAYS(NULL, full_cmp_ret == ret_val,
          "Comparison that skipped prefix returned different result than a full comparison");
    }
#else
    WT_UNUSED(session);
#endif

    return (ret_val);
}

/*
 * __wt_compare_skip --
 *     The same as __wt_lex_compare_skip, but using the application's collator function when
 *     configured.
 */
static WT_INLINE int
__wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator, const WT_ITEM *user_item,
  const WT_ITEM *tree_item, int *cmpp, size_t *matchp)
{
    if (collator == NULL) {
        *cmpp = __wt_lex_compare_skip(session, user_item, tree_item, matchp);
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
static WT_INLINE int
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
