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
__wt_db_close(WT_STOC *stoc)
{
	wt_args_db_close_unpack;
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = stoc->env->ienv;
	idb = db->idb;
	ret = 0;

	WT_DB_FCHK_NOTFATAL(db, "Db.close", flags, WT_APIMASK_DB_CLOSE, ret);

	/* Close the underlying Btree. */
	WT_TRET(__wt_bt_close(stoc));

	/* Discard the cache. */
	WT_TRET(__wt_cache_close(stoc));

	/* Remove from the environment's list. */
	TAILQ_REMOVE(&ienv->dbqh, idb, q);

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
