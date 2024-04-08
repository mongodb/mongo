/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#ifdef HAVE_X86INTRIN_H
#if !defined(_MSC_VER) && !defined(_lint)
#include <x86intrin.h>
#endif
#endif

#define WT_VECTOR_SIZE 16 /* chunk size */
#define WT_COMPARE_SHORT_MAXLEN 16

#ifdef HAVE_X86INTRIN_H

#define WT_VECTOR_MASK 0xFFFF
/*
 * __lex_compare_gt_16 --
 *     Lexicographic comparison routine for data greater than 16 bytes. Returns: < 0 if user_item is
 *     lexicographically < tree_item, = 0 if user_item is lexicographically = tree_item, > 0 if
 *     user_item is lexicographically > tree_item. We use the names "user" and "tree" so it's clear
 *     in the btree code which the application is looking at when we call its comparison function.
 */
static WT_INLINE int
__lex_compare_gt_16(const uint8_t *ustartp, const uint8_t *tstartp, size_t len, int lencmp)
{
    __m128i res_eq, t, u;
    int32_t eq_bits;
    size_t i, final_bytes;

    final_bytes = len - WT_VECTOR_SIZE;

    /*
     * Compare 16 bytes at a time until we find a difference or run out of 16 byte chunks to
     * compare.
     */
    for (i = 0; i < final_bytes; i += WT_VECTOR_SIZE) {
        u = _mm_loadu_si128((const __m128i *)(ustartp + i));
        t = _mm_loadu_si128((const __m128i *)(tstartp + i));
        res_eq = _mm_cmpeq_epi8(u, t);
        if ((eq_bits = _mm_movemask_epi8(res_eq)) != WT_VECTOR_MASK)
            goto final128;
    }

    /*
     * Rewind until there is exactly 16 bytes left. We know we started with at least 16, so we are
     * still in bounds.
     */
    i = final_bytes;
    u = _mm_loadu_si128((const __m128i *)(ustartp + i));
    t = _mm_loadu_si128((const __m128i *)(tstartp + i));
    res_eq = _mm_cmpeq_epi8(u, t);
    eq_bits = _mm_movemask_epi8(res_eq);

    if (eq_bits == WT_VECTOR_MASK)
        return (lencmp);
    else {
final128:
        /* The initial matching bytes correspond to trailing 1 bits in eq_bits. */
#ifndef _MSC_VER
        i += (size_t)__builtin_ctz(~(uint32_t)eq_bits);
#else
        unsigned long res;
        _BitScanForward(&res, ~(uint32_t)eq_bits);
        i += res;
#endif

        /* C zero-extends the bytes to 32 bit integers before the calculation. */
        return ((int)(ustartp[i] - tstartp[i]));
    }
}
#else
/*
 * __lex_compare_gt_16 --
 *     Lexicographic comparison routine for data greater than 16 bytes. Returns: < 0 if user_item is
 *     lexicographically < tree_item, = 0 if user_item is lexicographically = tree_item, > 0 if
 *     user_item is lexicographically > tree_item. We use the names "user" and "tree" so it's clear
 *     in the btree code which the application is looking at when we call its comparison function.
 *     Some platforms like ARM offer dedicated instructions for reading 16 bytes at a time, allowing
 *     for faster comparisons.
 */
static WT_INLINE int
__lex_compare_gt_16(const uint8_t *ustartp, const uint8_t *tstartp, size_t len, int lencmp)
{
    struct {
        uint64_t a, b;
    } tdata, udata;
    uint64_t t64, u64;
    const uint8_t *tendp, *treep, *uendp, *userp;
    bool firsteq;

    uendp = ustartp + len;
    tendp = tstartp + len;

    /*
     * Compare 16 bytes at a time until we find a difference or run out of 16 byte chunks to
     * compare.
     */
    for (userp = ustartp, treep = tstartp; uendp - userp > WT_VECTOR_SIZE;
         userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
        memcpy(&udata, userp, WT_VECTOR_SIZE);
        memcpy(&tdata, treep, WT_VECTOR_SIZE);
        if (udata.a != tdata.a || udata.b != tdata.b)
            goto final128;
    }

    /*
     * Rewind until there is exactly 16 bytes left. We know we started with at least 16, so we are
     * still in bounds.
     */
    memcpy(&udata, uendp - WT_VECTOR_SIZE, WT_VECTOR_SIZE);
    memcpy(&tdata, tendp - WT_VECTOR_SIZE, WT_VECTOR_SIZE);

final128:
    firsteq = udata.a == tdata.a;
    u64 = firsteq ? udata.b : udata.a;
    t64 = firsteq ? tdata.b : tdata.a;

#ifndef WORDS_BIGENDIAN
    u64 = __wt_bswap64(u64);
    t64 = __wt_bswap64(t64);
#endif

    return (u64 < t64 ? -1 : u64 > t64 ? 1 : lencmp);
}
#endif

/*
 * __lex_compare_le_16 --
 *     Lexicographic comparison routine for data less than or equal to 16 bytes. Returns: < 0 if
 *     user_item is lexicographically < tree_item, = 0 if user_item is lexicographically =
 *     tree_item, > 0 if user_item is lexicographically > tree_item. We use the names "user" and
 *     "tree" so it's clear in the btree code which the application is looking at when we call its
 *     comparison function. Some platforms like ARM offer dedicated instructions for reading 16
 *     bytes at a time, allowing for faster comparisons.
 */
static WT_INLINE int
__lex_compare_le_16(const uint8_t *ustartp, const uint8_t *tstartp, size_t len, int lencmp)
{
    uint64_t ta, tb, ua, ub, u64, t64;
    const uint8_t *tendp, *uendp;

    uendp = ustartp + len;
    tendp = tstartp + len;
    if (len >= sizeof(uint64_t)) {
        /*
         * len >= 64 bits. len is implicitly less than or equal to 128bits since the function
         * accepts 16 bytes or less.
         */
        memcpy(&ua, ustartp, sizeof(uint64_t));
        memcpy(&ta, tstartp, sizeof(uint64_t));
        memcpy(&ub, uendp - sizeof(uint64_t), sizeof(uint64_t));
        memcpy(&tb, tendp - sizeof(uint64_t), sizeof(uint64_t));
    } else if (len & sizeof(uint32_t)) {
        /* len >= 32 bits */
        uint32_t ta32, tb32, ua32, ub32;
        memcpy(&ua32, ustartp, sizeof(uint32_t));
        memcpy(&ta32, tstartp, sizeof(uint32_t));
        memcpy(&ub32, uendp - sizeof(uint32_t), sizeof(uint32_t));
        memcpy(&tb32, tendp - sizeof(uint32_t), sizeof(uint32_t));
        ua = ua32;
        ta = ta32;
        ub = ub32;
        tb = tb32;
    } else if (len & sizeof(uint16_t)) {
        /* len >= 16 bits */
        uint16_t ta16, tb16, ua16, ub16;
        memcpy(&ua16, ustartp, sizeof(uint16_t));
        memcpy(&ta16, tstartp, sizeof(uint16_t));
        memcpy(&ub16, uendp - sizeof(uint16_t), sizeof(uint16_t));
        memcpy(&tb16, tendp - sizeof(uint16_t), sizeof(uint16_t));
        ua = ua16;
        ta = ta16;
        ub = ub16;
        tb = tb16;
    } else if (len) {
        uint8_t ta8, ua8;
        memcpy(&ua8, ustartp, sizeof(uint8_t));
        memcpy(&ta8, tstartp, sizeof(uint8_t));
        return (ua8 < ta8 ? -1 : ua8 > ta8 ? 1 : lencmp);
    } else
        return (lencmp);

    u64 = ua == ta ? ub : ua;
    t64 = ua == ta ? tb : ta;

#ifndef WORDS_BIGENDIAN
    u64 = __wt_bswap64(u64);
    t64 = __wt_bswap64(t64);
#endif

    return (u64 < t64 ? -1 : u64 > t64 ? 1 : lencmp);
}

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
    size_t len, tsz, usz;
    int lencmp, ret_val;

    usz = user_item->size;
    tsz = tree_item->size;
    if (usz < tsz) {
        len = usz;
        lencmp = -1;
    } else if (usz > tsz) {
        len = tsz;
        lencmp = 1;
    } else {
        len = usz;
        lencmp = 0;
    }

    if (len > WT_VECTOR_SIZE)
        ret_val = __lex_compare_gt_16(
          (const uint8_t *)user_item->data, (const uint8_t *)tree_item->data, len, lencmp);
    else
        ret_val = __lex_compare_le_16(
          (const uint8_t *)user_item->data, (const uint8_t *)tree_item->data, len, lencmp);

    return (ret_val);
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

    WT_STAT_CONN_DATA_INCR(session, cursor_bounds_comparisons);

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

#ifdef HAVE_X86INTRIN_H
/*
 * __lex_compare_skip_gt_16 --
 *     Lexicographic comparison routine for data greater than 16 bytes, skipping leading bytes.
 *     Returns: < 0 if user_item is lexicographically < tree_item = 0 if user_item is
 *     lexicographically = tree_item > 0 if user_item is lexicographically > tree_item We use the
 *     names "user" and "tree" so it's clear in the btree code which the application is looking at
 *     when we call its comparison function.
 */
static WT_INLINE int
__lex_compare_skip_gt_16(
  const uint8_t *ustartp, const uint8_t *tstartp, size_t len, int lencmp, size_t *matchp)
{
    __m128i res_eq, t, u;
    int32_t eq_bits;
    size_t match, final_bytes, final_match;

    match = *matchp;
    final_bytes = len - WT_VECTOR_SIZE;

    /*
     * Compare 16 bytes at a time until we find a difference or run out of 16 byte chunks to
     * compare.
     */
    for (; match < final_bytes; match += WT_VECTOR_SIZE) {
        u = _mm_loadu_si128((const __m128i *)(ustartp + match));
        t = _mm_loadu_si128((const __m128i *)(tstartp + match));
        res_eq = _mm_cmpeq_epi8(u, t);
        if ((eq_bits = _mm_movemask_epi8(res_eq)) != WT_VECTOR_MASK)
            goto final128;
    }

    /*
     * Rewind until there is exactly 16 bytes left. We know we started with at least 16, so we are
     * still in bound.
     */
    match = final_bytes;
    u = _mm_loadu_si128((const __m128i *)(ustartp + final_bytes));
    t = _mm_loadu_si128((const __m128i *)(tstartp + final_bytes));
    res_eq = _mm_cmpeq_epi8(u, t);
    eq_bits = _mm_movemask_epi8(res_eq);

    if (eq_bits == WT_VECTOR_MASK) {
        *matchp = len;
        return (lencmp);
    } else {
final128:
        /* The initial matching bytes correspond to trailing 1 bits in eq_bits. */
#ifndef _MSC_VER
        final_match = (size_t)__builtin_ctz(~(uint32_t)eq_bits);
#else
        unsigned long res;
        _BitScanForward(&res, ~(uint32_t)eq_bits);
        final_match = res;
#endif
        match += final_match;
        *matchp = match;

        /* C zero-extends the bytes to 32 bit integers before the calculation. */
        return ((int)(ustartp[match] - tstartp[match]));
    }
}
#else
/*
 * __lex_compare_skip_gt_16 --
 *     Lexicographic comparison routine for data greater than 16 bytes, skipping leading bytes.
 *     Returns: < 0 if user_item is lexicographically < tree_item = 0 if user_item is
 *     lexicographically = tree_item > 0 if user_item is lexicographically > tree_item We use the
 *     names "user" and "tree" so it's clear in the btree code which the application is looking at
 *     when we call its comparison function. Some platforms like ARM offer dedicated instructions
 *     for reading 16 bytes at a time, allowing for faster comparisons.
 */
static WT_INLINE int
__lex_compare_skip_gt_16(
  const uint8_t *ustartp, const uint8_t *tstartp, size_t len, int lencmp, size_t *matchp)
{
    struct {
        uint64_t a, b;
    } tdata, udata;
    size_t match;
    uint64_t t64, u64;
    int leading_zero_bytes;
    const uint8_t *tendp, *treep, *uendp, *userp;
    bool firsteq;

    match = *matchp;
    uendp = ustartp + len;
    tendp = tstartp + len;

    /*
     * Compare 16 bytes at a time until we find a difference or run out of 16 byte chunks to
     * compare.
     */
    for (userp = ustartp + match, treep = tstartp + match; uendp - userp > WT_VECTOR_SIZE;
         userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
        memcpy(&udata, userp, WT_VECTOR_SIZE);
        memcpy(&tdata, treep, WT_VECTOR_SIZE);
        if (udata.a != tdata.a || udata.b != tdata.b) {
            match = (size_t)(userp - ustartp);
            goto final128;
        }
    }

    /*
     * Rewind until there is exactly 16 bytes left. We know we started with at least 16, so we are
     * still in bound.
     */
    match = len - WT_VECTOR_SIZE;
    memcpy(&udata, uendp - WT_VECTOR_SIZE, WT_VECTOR_SIZE);
    memcpy(&tdata, tendp - WT_VECTOR_SIZE, WT_VECTOR_SIZE);

final128:
    firsteq = udata.a == tdata.a;
    u64 = firsteq ? udata.b : udata.a;
    t64 = firsteq ? tdata.b : tdata.a;
    match += firsteq * sizeof(uint64_t);

#ifndef WORDS_BIGENDIAN
    u64 = __wt_bswap64(u64);
    t64 = __wt_bswap64(t64);
#endif

    WT_LEADING_ZEROS(u64 ^ t64, leading_zero_bytes);
    match += (size_t)leading_zero_bytes;
    *matchp = match;

    return (u64 < t64 ? -1 : u64 > t64 ? 1 : lencmp);
}
#endif

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
    size_t len, tsz, usz;
    int lencmp, ret_val;

    usz = user_item->size;
    tsz = tree_item->size;
    if (usz < tsz) {
        len = usz;
        lencmp = -1;
    } else if (usz > tsz) {
        len = tsz;
        lencmp = 1;
    } else {
        len = usz;
        lencmp = 0;
    }

    if (len > WT_VECTOR_SIZE) {
        ret_val = __lex_compare_skip_gt_16(
          (const uint8_t *)user_item->data, (const uint8_t *)tree_item->data, len, lencmp, matchp);

#ifdef HAVE_DIAGNOSTIC
        /*
         * There are various optimizations in the code to skip comparing prefixes that are known to
         * be the same. If configured, check that the prefixes actually match.
         */
        if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_PREFIX_COMPARE)) {
            int full_cmp_ret;
            full_cmp_ret = __wt_lex_compare(user_item, tree_item);
            WT_ASSERT_ALWAYS(session, full_cmp_ret == ret_val,
              "Comparison that skipped prefix returned different result than a full comparison");
        }
#else
        WT_UNUSED(session);
#endif
    } else
        /*
         * We completely ignore match when len <= 16 because it wouldn't reduce the amount of work
         * done, and would add overhead.
         */
        ret_val = __lex_compare_le_16(
          (const uint8_t *)user_item->data, (const uint8_t *)tree_item->data, len, lencmp);

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
    size_t len, tsz, usz;
    int lencmp;

    usz = user_item->size;
    tsz = tree_item->size;
    if (usz < tsz) {
        len = usz;
        lencmp = -1;
    } else if (usz > tsz) {
        len = tsz;
        lencmp = 1;
    } else {
        len = usz;
        lencmp = 0;
    }

    return (__lex_compare_le_16(
      (const uint8_t *)user_item->data, (const uint8_t *)tree_item->data, len, lencmp));
}
