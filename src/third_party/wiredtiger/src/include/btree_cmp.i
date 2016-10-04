/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifdef HAVE_X86INTRIN_H
#if !defined(_MSC_VER)
#include <x86intrin.h>
#endif
						/* 16B alignment */
#define	WT_ALIGNED_16(p)	(((uintptr_t)(p) & 0x0f) == 0)
#define	WT_VECTOR_SIZE		16		/* chunk size */
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
	size_t len, usz, tsz;
	const uint8_t *userp, *treep;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz);

	userp = user_item->data;
	treep = tree_item->data;

#ifdef HAVE_X86INTRIN_H
	/* Use vector instructions if we'll execute at least 2 of them. */
	if (len >= WT_VECTOR_SIZE * 2) {
		size_t remain;
		__m128i res_eq, u, t;

		remain = len % WT_VECTOR_SIZE;
		len -= remain;
		if (WT_ALIGNED_16(userp) && WT_ALIGNED_16(treep))
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
				u = _mm_load_si128((const __m128i *)userp);
				t = _mm_load_si128((const __m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		else
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE) {
				u = _mm_loadu_si128((const __m128i *)userp);
				t = _mm_loadu_si128((const __m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		len += remain;
	}
#endif
	/*
	 * Use the non-vectorized version for the remaining bytes and for the
	 * small key sizes.
	 */
	for (; len > 0; --len, ++userp, ++treep)
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
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
			    *matchp += WT_VECTOR_SIZE) {
				u = _mm_load_si128((const __m128i *)userp);
				t = _mm_load_si128((const __m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		else
			for (; len > 0;
			    len -= WT_VECTOR_SIZE,
			    userp += WT_VECTOR_SIZE, treep += WT_VECTOR_SIZE,
			    *matchp += WT_VECTOR_SIZE) {
				u = _mm_loadu_si128((const __m128i *)userp);
				t = _mm_loadu_si128((const __m128i *)treep);
				res_eq = _mm_cmpeq_epi8(u, t);
				if (_mm_movemask_epi8(res_eq) != 65535)
					break;
			}
		len += remain;
	}
#endif
	/*
	 * Use the non-vectorized version for the remaining bytes and for the
	 * small key sizes.
	 */
	for (; len > 0; --len, ++userp, ++treep, ++*matchp)
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

/*
 * __wt_lex_compare_short --
 *	Lexicographic comparison routine for short keys.
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
__wt_lex_compare_short(const WT_ITEM *user_item, const WT_ITEM *tree_item)
{
	size_t len, usz, tsz;
	const uint8_t *userp, *treep;

	usz = user_item->size;
	tsz = tree_item->size;
	len = WT_MIN(usz, tsz);

	userp = user_item->data;
	treep = tree_item->data;

	/*
	 * The maximum packed uint64_t is 9B, catch row-store objects using
	 * packed record numbers as keys.
	 */
#define	WT_COMPARE_SHORT_MAXLEN 9
#undef	WT_COMPARE_SHORT
#define	WT_COMPARE_SHORT(n)						\
	case n:								\
		if (*userp != *treep)					\
			break;						\
		++userp, ++treep
	switch (len) {
	WT_COMPARE_SHORT(9);
	WT_COMPARE_SHORT(8);
	WT_COMPARE_SHORT(7);
	WT_COMPARE_SHORT(6);
	WT_COMPARE_SHORT(5);
	WT_COMPARE_SHORT(4);
	WT_COMPARE_SHORT(3);
	WT_COMPARE_SHORT(2);
	case 1:
		if (*userp != *treep)
			break;

		/* Contents are equal up to the smallest length. */
		return ((usz == tsz) ?  0 : (usz < tsz) ? -1 : 1);
	}
	return (*userp < *treep ? -1 : 1);
}
