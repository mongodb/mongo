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
 * __wt_db_close --
 *	Close a DB handle.
 */
int
__wt_db_close(WT_TOC *toc)
{
	wt_args_db_close_unpack;
	ENV *env;
	IENV *ienv;
	WT_STOC *stoc;
	int ret;

	env = toc->env;
	ienv = env->ienv;
	stoc = db->idb->stoc;
	ret = 0;

	WT_DB_FCHK_NOTFATAL(db, "Db.close", flags, WT_APIMASK_DB_CLOSE, ret);

	/* Close the underlying Btree. */
	WT_TRET(__wt_bt_close(db));

	/* Discard the cache. */
	WT_TRET(__wt_cache_close(db));

	/* Discard the server thread. */
	if (!F_ISSET(ienv, WT_SINGLE_THREADED)) {
		stoc->running = 0;
		WT_FLUSH_MEMORY;
		__wt_thread_join(stoc->tid);
	}

	/* Remove from the environment's list. */
	TAILQ_REMOVE(&env->dbqh, db, q);

	/* Re-cycle the underlying IDB structure. */
	WT_TRET(__wt_idb_destroy(db, 1));

	/*
	 * Reset the methods that are permitted.
	 * If anything failed, we're done with this handle.
	 */
	if (ret == 0)
		__wt_db_config_methods(db);
	else
		__wt_db_config_methods_lockout(db);

	return (ret);
}
