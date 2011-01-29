/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_env_sync --
 *	Flush the environment's cache.
 */
int
__wt_env_sync(ENV *env, void (*f)(const char *, uint64_t))
{
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	TAILQ_FOREACH(idb, &ienv->dbqh, q)
		WT_TRET(idb->db->sync(idb->db, f, 0));

	return (ret);
}
