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
__wt_bt_sync(WT_TOC *toc)
{
	IDB *idb;
	WT_SRVR *srvr;
	u_int i;
	int ret;

	idb = toc->db->idb;
	ret = 0;

	/* Flush all secondary servers. */
	WT_SRVR_FOREACH(idb, srvr, i) {
		/*
		 * BUG!!!
		 * We have to schedule a flush of each server's cache.
		 * we can't just call the cache flush routine with a
		 * different server's cache!  For now, just cheat.
		 */
		WT_TRET(__wt_cache_sync(toc, srvr->cache));
	}

	/* Flush the primary server's cache for this database. */
	WT_TRET(__wt_cache_sync(toc, idb->cache));

	return (ret);
}
