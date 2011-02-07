/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_env_sync --
 *	Flush the environment's cache.
 */
int
__wt_env_sync(ENV *env, void (*f)(const char *, uint64_t))
{
	BTREE *btree;
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	TAILQ_FOREACH(btree, &ienv->dbqh, q)
		WT_TRET(btree->db->sync(btree->db, f, 0));

	return (ret);
}
