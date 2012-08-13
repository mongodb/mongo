/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_worker --
 *	The worker thread for an LSM tree, responsible for writing in-memory
 *	trees to disk and merging on-disk trees.
 */
void *
__wt_lsm_worker(void *arg)
{
	WT_CONNECTION *wt_conn;
	WT_CONNECTION_IMPL *conn;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;
	int i, nchunks, progress;

	lsm_tree = arg;
	conn = lsm_tree->conn;
	wt_conn = &conn->iface;

	if (wt_conn->open_session(wt_conn, NULL, NULL, &wt_session) != 0)
		return (NULL);

	session = (WT_SESSION_IMPL *)wt_session;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
		progress = 0;

		/*
		 * Write checkpoints in all completed files, then find
		 * something to merge.
		 */
		for (i = 0, nchunks = lsm_tree->nchunks - 1; i < nchunks; i++) {
			chunk = &lsm_tree->chunk[i];
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				continue;

			/* XXX durability: need to checkpoint the metadata? */
			if (__wt_schema_worker(session,
			    chunk->uri, __wt_checkpoint, NULL, 0) == 0) {
				F_SET(chunk, WT_LSM_CHUNK_ONDISK);
				progress = 1;
			}
		}

		if (__wt_lsm_major_merge(session, lsm_tree) == 0)
			progress = 1;

		if (!progress)
			__wt_sleep(1, 0);
	}

	(void)wt_session->close(wt_session, NULL);

	return (NULL);
}
