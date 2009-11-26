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
 * __wt_bt_sync --
 *	Sync a Btree.
 */
int
__wt_bt_sync(DB *db, void (*f)(const char *, u_int32_t))
{
	ENV *env;
	WT_TOC *toc;
	int ret;

	env = db->env;
	ret = 0;

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.sync");

	ret = __wt_cache_sync(toc, db->idb->fh, f);

	WT_TRET(toc->close(toc, 0));

	return (ret);
}
