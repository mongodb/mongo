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
	WT_STOC *stoc;
	int ret, tret;

	env = toc->env;
	stoc = db->idb->stoc;
	ret = 0;

	WT_DB_FCHK_NOTFATAL(db, "Db.close", flags, WT_APIMASK_DB_CLOSE, ret);

	/* Close the underlying Btree. */
	if ((tret = __wt_bt_close(db)) != 0 && ret == 0)
		ret = tret;

	/* Discard the cache. */
	if ((tret = __wt_cache_close(db)) != 0 && ret == 0)
		ret = tret;

	/* Discard the server thread. */
	if (WT_GLOBAL(single_threaded)) {
		stoc->running = 0;
		WT_FLUSH_MEMORY;
		(void)pthread_join(stoc->tid, NULL);
	}

	/* Remove from the environment's list. */
	TAILQ_REMOVE(&env->dbqh, db, q);

	/* Re-cycle the underlying IDB structure. */
	if ((tret = __wt_idb_destroy(db, 1)) != 0 && ret == 0)
		ret = tret;

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
