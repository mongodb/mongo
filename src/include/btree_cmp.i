/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifdef __SSE2__
#include "3rdparty/sse/emmintrin.h"

#define	WT_ALIGNED_16(p)	(!(p) & 0xff)	/* 2B alignment */
#define	WT_VECTOR_SIZE		16		/* chunk size */
#define	WT_MIN_KEY_VECTORIZE	32		/* minimum vectorized key */
#endif

/*
 * __wt_lex_compare --
 *	Lexicographic comparison routine.
 *
 * Returns:
 *	< 0 if user_item is lexicographically < tree_item
 *	= 0 if user_item is lexicographically = tree_item
 *	> 0 if user_item is lexicographically > tree_item
 *
 * We use the names "user" and "tree" so it's clear in the btree code which
 * the application is looking at when we call its comparison function.
 */
static inline int
__wt_lex_compare(const WT_ITEM *user_item, const WT_ITEM *tree_item)
{
	const uint8_t *userp, *treep;
	ssize_t remainder;
	size_t len, usz, tsz;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz);
	remainder = len;

#ifdef __SSE2__
	if (len >= WT_MIN_KEY_VECTORIZE) {
		__m128i res_eq, res_less, u, t;
		uint8_t *val_res_eq, *val_res_less;
		u_int j;

		remainder = len % WT_VECTOR_SIZE;
		len -= remainder;

		userp = (uint8_t *)user_item->data;
		treep = (uint8_t *)tree_item->data;
		if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
				u = _mm_load_si128((__m128i *)userp);
				t = _mm_load_si128((__m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		else
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
				u = _mm_loadu_si128((__m128i *)userp);
				t = _mm_loadu_si128((__m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}

		if (len != 0) {
			res_less = _mm_cmplt_epi8(u, t);
			val_res_eq = (uint8_t *)&res_eq;
			val_res_less = (uint8_t *)&res_less;
			for (j = 0; j < WT_VECTOR_SIZE; j++) {
				if (val_res_eq[j] == 0)
					return (val_res_less[j] == 0 ? 1 : -1);
			}
		}
	}
#endif
	/*
	 * Use the non-vectorized version for the remaining bytes and for the
	 * small key sizes.
	 */
	for (userp = user_item->data, treep = tree_item->data;
	    remainder > 0;
	    --remainder, ++userp, ++treep)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

/*
 * __wt_compare --
 *	The same as __wt_lex_compare, but using the application's collator
 * function when configured.
 */
static inline int
__wt_compare(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
    const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp)
{
	if (collator == NULL) {
		*cmpp = __wt_lex_compare(user_item, tree_item);
		return (0);
	}
	return (collator->compare(
	    collator, &session->iface, user_item, tree_item, cmpp));
}

/*
 * __wt_lex_compare_skip --
 *	Lexicographic comparison routine, skipping leading bytes.
 *
 * Returns:
 *	< 0 if user_item is lexicographically < tree_item
 *	= 0 if user_item is lexicographically = tree_item
 *	> 0 if user_item is lexicographically > tree_item
 *
 * We use the names "user" and "tree" so it's clear in the btree code which
 * the application is looking at when we call its comparison function.
 */
static inline int
__wt_lex_compare_skip(
    const WT_ITEM *user_item, const WT_ITEM *tree_item, size_t *matchp)
{
	const uint8_t *userp, *treep;
	ssize_t remainder;
	size_t len, usz, tsz;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz) - *matchp;
	remainder = len;

#ifdef __SSE2__
	if (len >= WT_MIN_KEY_VECTORIZE) {
		__m128i res_eq, res_less, u, t;
		uint8_t *val_res_eq, *val_res_less;
		u_int j;

		remainder = len % WT_VECTOR_SIZE;
		len -= remainder;

		userp = (uint8_t *)user_item->data + *matchp;
		treep = (uint8_t *)tree_item->data + *matchp;
		if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
			    *matchp += WT_VECTOR_SIZE) {
				u = _mm_load_si128((__m128i *)userp);
				t = _mm_load_si128((__m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		else
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
			    *matchp += WT_VECTOR_SIZE) {
				u = _mm_loadu_si128((__m128i *)userp);
				t = _mm_loadu_si128((__m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}

		if (len != 0) {
			res_less = _mm_cmplt_epi8(u, t);
			val_res_eq = (uint8_t *)&res_eq;
			val_res_less = (uint8_t *)&res_less;
			for (j = 0; j < WT_VECTOR_SIZE; j++) {
				if (val_res_eq[j] == 0)
					return (val_res_less[j] == 0 ? 1 : -1);
				++*matchp;
			}
		}
	}
#endif
	/*
	 * Use the non-vectorized version for the remaining bytes and for the
	 * small key sizes.
	 */
	for (userp = (uint8_t *)user_item->data + *matchp,
	    treep = (uint8_t *)tree_item->data + *matchp;
	    remainder > 0;
	    --remainder, ++userp, ++treep, ++*matchp)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return ((usz == tsz) ? 0 : (usz < tsz) ? -1 : 1);
}

/*
 * __wt_compare_skip --
 *	The same as __wt_lex_compare_skip, but using the application's collator
 * function when configured.
 */
static inline int
__wt_compare_skip(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
    const WT_ITEM *user_item, const WT_ITEM *tree_item, int *cmpp,
    size_t *matchp)
{
	if (collator == NULL) {
		*cmpp = __wt_lex_compare_skip(user_item, tree_item, matchp);
		return (0);
	}
	return (collator->compare(
	    collator, &session->iface, user_item, tree_item, cmpp));
}
