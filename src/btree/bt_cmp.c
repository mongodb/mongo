/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_bt_lex_compare --
 *	Lexicographic comparison routine.
 */
int
__wt_bt_lex_compare(
    WT_BTREE *btree, const WT_ITEM *user_item, const WT_ITEM *tree_item)
{
	uint32_t len;
	const uint8_t *userp, *treep;

	/*
	 * The WT_BTREE * argument isn't used by the default routine, but is
	 * a standard argument for user-specified comparison functions.
	 */
	WT_UNUSED(btree);

	/*
	 * Return:
	 *	< 0 if user_item is lexicographically < tree_item
	 *	= 0 if user_item is lexicographically = tree_item
	 *	> 0 if user_item is lexicographically > tree_item
	 *
	 * We use the names "user" and "tree" so it's clear which the
	 * application is looking at when we call its comparison func.
	 */
	if ((len = user_item->size) > tree_item->size)
		len = tree_item->size;
	for (userp = user_item->data,
	    treep = tree_item->data; len > 0; --len, ++userp, ++treep)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return (user_item->size == tree_item->size ? 0 :
	    (user_item->size < tree_item->size ? -1 : 1));
}
