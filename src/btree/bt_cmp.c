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
__wt_bt_lex_compare(BTREE *btree, const WT_DATAITEM *user_dbt, const WT_DATAITEM *tree_dbt)
{
	uint32_t len;
	const uint8_t *userp, *treep;

	/*
	 * The BTREE * argument isn't used by the default routine, but is
	 * a standard argument for user-specified comparison functions.
	 */
	WT_UNUSED(btree);

	/*
	 * Return:
	 *	< 0 if user_dbt is lexicographically < tree_dbt
	 *	= 0 if user_dbt is lexicographically = tree_dbt
	 *	> 0 if user_dbt is lexicographically > tree_dbt
	 *
	 * We use the names "user" and "tree" so it's clear which the
	 * application is looking at when we call its comparison func.
	 */
	if ((len = user_dbt->size) > tree_dbt->size)
		len = tree_dbt->size;
	for (userp = user_dbt->data,
	    treep = tree_dbt->data; len > 0; --len, ++userp, ++treep)
		if (*userp != *treep)
			return (*userp < *treep ? -1 : 1);

	/* Contents are equal up to the smallest length. */
	return (user_dbt->size == tree_dbt->size ? 0 :
	    (user_dbt->size < tree_dbt->size ? -1 : 1));
}

/*
 * __wt_bt_int_compare --
 *	Integer comparison routine.
 */
int
__wt_bt_int_compare(BTREE *btree, const WT_DATAITEM *user_dbt, const WT_DATAITEM *tree_dbt)
{
	uint64_t user_int, tree_int;

	/*
	 * The WT_DATAITEM must hold the low-order bits in machine integer order.
	 *
	 * Return:
	 *	< 0 if user_dbt is < tree_dbt
	 *	= 0 if user_dbt is = tree_dbt
	 *	> 0 if user_dbt is > tree_dbt
	 *
	 * We use the names "user" and "tree" so it's clear which the
	 * application is looking at when we call its comparison func.
	 */
	user_int = tree_int = 0;
	memcpy(&user_int, user_dbt->data, (size_t)btree->btree_compare_int);
	memcpy(&tree_int, tree_dbt->data, (size_t)btree->btree_compare_int);

	return (user_int == tree_int ? 0 : (user_int < tree_int ? -1 : 1));
}
