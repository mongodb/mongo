/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __clsm_close_bulk --
 *	WT_CURSOR->close method for LSM bulk cursors.
 */
static int
__clsm_close_bulk(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_LSM_TREE *lsm_tree;

	clsm = (WT_CURSOR_LSM *)cursor;
	lsm_tree = clsm->lsm_tree;
	F_SET(lsm_tree->chunk[0], WT_LSM_CHUNK_ONDISK);

	WT_RET(__wt_clsm_close(cursor));
	return (0);
}
/*
 * __clsm_insert_bulk --
 *	WT_CURSOR->insert method for LSM bulk cursors.
 */
static int
__clsm_insert_bulk(WT_CURSOR *cursor)
{
	WT_CURSOR *bulk_cursor;
	WT_CURSOR_LSM *clsm;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	lsm_tree = clsm->lsm_tree;
	session = (WT_SESSION_IMPL *)clsm->iface.session;

	WT_ASSERT(session, lsm_tree->nchunks == 1 && clsm->nchunks == 1);
	++lsm_tree->chunk[0]->count;
	bulk_cursor = *clsm->cursors;
	bulk_cursor->set_key(bulk_cursor, &cursor->key);
	bulk_cursor->set_value(bulk_cursor, &cursor->value);
	WT_RET(bulk_cursor->insert(bulk_cursor));

	return (0);
}

/*
 * __wt_clsm_open_bulk --
 *	WT_SESSION->open_cursor method for LSM bulk cursors.
 */
int
__wt_clsm_open_bulk(WT_CURSOR_LSM *clsm, const char *cfg[])
{
	WT_CURSOR *cursor, *bulk_cursor;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;

	bulk_cursor = NULL;
	cursor = &clsm->iface;
	lsm_tree = clsm->lsm_tree;
	session = (WT_SESSION_IMPL *)clsm->iface.session;

	F_SET(clsm, WT_CLSM_BULK);

	/* Bulk cursors are limited to insert and close. */
	__wt_cursor_set_notsup(cursor);
	cursor->insert = __clsm_insert_bulk;
	cursor->close = __clsm_close_bulk;

	/* Setup the first chunk in the tree. */
	WT_RET(__wt_clsm_request_switch(clsm));
	WT_RET(__wt_clsm_await_switch(clsm));

	/*
	 * Grab and release the LSM tree lock to ensure that the first chunk
	 * has been fully created before proceeding. We have the LSM tree
	 * open exclusive, so that saves us from needing the lock generally.
	 */
	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));
	WT_RET(__wt_lsm_tree_readunlock(session, lsm_tree));

	/*
	 * Open a bulk cursor on the first chunk, it's not a regular LSM chunk
	 * cursor, but use the standard storage locations. Allocate the space
	 * for a bloom filter - it makes cleanup simpler. Cleaned up by
	 * cursor close on error.
	 */
	WT_RET(__wt_calloc_one(session, &clsm->blooms));
	clsm->bloom_alloc = 1;
	WT_RET(__wt_calloc_one(session, &clsm->cursors));
	clsm->cursor_alloc = 1;
	clsm->nchunks = 1;

	/*
	 * Open a bulk cursor on the first chunk in the tree - take a read
	 * lock on the LSM tree while we are opening the chunk, to ensure
	 * that the first chunk has been fully created before we succeed.
	 * Pass through the application config to ensure the tree is open
	 * for bulk access.
	 */
	WT_RET(__wt_open_cursor(session,
	    lsm_tree->chunk[0]->uri, &clsm->iface, cfg, &bulk_cursor));
	clsm->cursors[0] = bulk_cursor;
	/* LSM cursors are always raw */
	F_SET(bulk_cursor, WT_CURSTD_RAW);

	return (0);
}

