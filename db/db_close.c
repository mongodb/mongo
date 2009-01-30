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
__wt_db_close(wt_args_db_close *argp)
{
	wt_args_db_close_unpack;
	ENV *env;
	int ret, tret;

	env = db->env;
	ret = 0;

	DB_FLAG_CHK_NOTFATAL(db, "Db.close", flags, WT_APIMASK_DB_CLOSE, ret);

	/* Close the underlying Btree. */
	if ((tret = __wt_bt_close(db)) != 0 && ret == 0)
		ret = tret;

	/* Disconnect from the list. */
	TAILQ_REMOVE(&env->dbqh, db, q);

	/* Re-cycle the underlying IDB structure. */
	if ((tret = __wt_idb_destroy(db, 1)) != 0 && ret == 0)
		ret = tret;

	/* Close any private environment. */
	if (F_ISSET(env, WT_PRIVATE_ENV) &&
	    (tret = env->close(env, 0)) != 0 && ret == 0)
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
