/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_lex_compare --
 *	Lexicographic comparison routine.
 */
int
__wt_bt_lex_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt)
{
	u_int32_t len;
	u_int8_t *userp, *treep;

	/*lint -esym(715,db)
	 *
	 * The DB * argument isn't used by the default routine, but is
	 * a standard argument for user-specified comparison functions.
	 */

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
__wt_bt_int_compare(DB *db, const DBT *user_dbt, const DBT *tree_dbt)
{
	u_int64_t user_int, tree_int;

	/*
	 * The DBT must hold the low-order bits in machine integer order.
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
	memcpy(&user_int, user_dbt->data, (size_t)db->btree_compare_int);
	memcpy(&tree_int, tree_dbt->data, (size_t)db->btree_compare_int);

	return (user_int == tree_int ? 0 : (user_int < tree_int ? -1 : 1));
}
