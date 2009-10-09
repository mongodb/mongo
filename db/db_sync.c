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
 * __wt_db_sync --
 *	Flush a database to the backing file.
 */
int
__wt_db_sync(WT_TOC *toc)
{
	wt_args_db_sync_unpack;

	db = toc->db;

	WT_DB_FCHK(db, "Db.sync", flags, WT_APIMASK_DB_SYNC);

	/* Close the underlying Btree. */
	return (__wt_bt_sync(toc));
}
