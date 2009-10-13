/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_server_stop(WT_TOC *);

/*
 * __wt_bt_close --
 *	Close a Btree.
 */
int
__wt_bt_close(WT_TOC *toc)
{
	IDB *idb;
	IENV *ienv;
	int ret;

	idb = toc->db->idb;
	ienv = toc->env->ienv;
	ret = 0;

	/* Discard our root page reference, we're tossing the cache. */
	idb->root_page = NULL;

	/* Exit any servers we've started for this database. */
	if (!F_ISSET(ienv, WT_SINGLE_THREADED))
		WT_TRET(__wt_bt_server_stop(toc));

	/* Discard the primary cache. */
	WT_TRET(__wt_cache_destroy(toc, &idb->cache));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(toc, idb->fh));
	idb->fh = NULL;

	return (ret);
}

/*
 * __wt_bt_server_stop --
 *	Join server threads for the second-level subtrees.
 */
static int
__wt_bt_server_stop(WT_TOC *toc)
{
	ENV *env;
	WT_SRVR *srvr;
	IDB *idb;
	u_int i;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	WT_SRVR_FOREACH(idb, srvr, i) {
		/* Stop the server and let it know. */
		srvr->running = 0;
		WT_FLUSH_MEMORY;

		__wt_free(env, srvr->ops);
		WT_TRET(__wt_cache_destroy(toc, &srvr->cache));
		__wt_free(env, srvr->stats);

		__wt_thread_join(srvr->tid);
	}

	return (ret);
}
