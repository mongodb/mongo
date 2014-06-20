/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_session_fotxn_add --
 *	Add a new entry into the session's free-on-transaction generation list.
 */
int
__wt_session_fotxn_add(WT_SESSION_IMPL *session, void *p, size_t len)
{
	WT_FOTXN *fotxn;

	/*
	 * Make sure the current thread has a transaction pinned so that
	 * we don't immediately free the memory we are stashing.
	 */
	WT_ASSERT(session,
	    WT_SESSION_TXN_STATE(session)->snap_min != WT_TXN_NONE);

	/* Grow the list as necessary. */
	WT_RET(__wt_realloc_def(session,
	    &session->fotxn_size, session->fotxn_cnt + 1, &session->fotxn));

	fotxn = session->fotxn + session->fotxn_cnt++;
	fotxn->btree = S2BT(session);
	fotxn->txnid = __wt_txn_current_id(session) + 1;
	WT_ASSERT(session, !__wt_txn_visible_all(session, fotxn->txnid));
	WT_ASSERT(session, fotxn->p == NULL);
	fotxn->p = p;
	fotxn->len = len;

	WT_STAT_FAST_CONN_INCRV(session, rec_split_stashed_bytes, len);
	WT_STAT_FAST_CONN_ATOMIC_INCR(session, rec_split_stashed_objects);

	/* See if we can free any previous entries. */
	if (session->fotxn_cnt > 1)
		__wt_session_fotxn_discard(session);

	return (0);
}

/*
 * __wt_session_fotxn_discard --
 *	Discard any memory from the session's free-on-transaction generation
 *	list that we can.
 */
void
__wt_session_fotxn_discard(WT_SESSION_IMPL *session)
{
	WT_BTREE *prev_btree;
	WT_FOTXN *fotxn;
	size_t i;

	/* The last known tree that wasn't busy. */
	prev_btree = NULL;

	/* Bump the oldest transaction ID. */
	__wt_txn_update_oldest(session);

	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_cnt;
	    ++i, ++fotxn) {
		if (fotxn->p == NULL)
			continue;
		else if (fotxn->btree == prev_btree)
			;
		else if (__wt_btree_exclusive(session, fotxn->btree))
			prev_btree = fotxn->btree;
		else if (!__wt_txn_visible_all(session, fotxn->txnid))
			break;
		/*
		 * It's a bad thing if another thread is in this memory
		 * after we free it, make sure nothing good happens to
		 * that thread.
		 */
		__wt_overwrite_and_free_len(session, fotxn->p, fotxn->len);
		WT_STAT_FAST_CONN_INCRV(
		    session, rec_split_stashed_bytes, -fotxn->len);
		WT_STAT_FAST_CONN_ATOMIC_DECR(
		    session, rec_split_stashed_objects);
	}

	/*
	 * If there are enough free slots at the beginning of the list, shuffle
	 * everything down.
	 */
	if (i > 100 &&
	    (session->fotxn_cnt -= i) > 0) {
		memmove(session->fotxn, session->fotxn + i,
		    session->fotxn_cnt * sizeof(session->fotxn[0]));
		memset(session->fotxn + session->fotxn_cnt, 0,
		    i * sizeof(session->fotxn[0]));
	}
}

/*
 * __wt_session_fotxn_discard_all --
 *	Discard all memory from a session's free-on-transaction generation
 *	list.
 */
void
__wt_session_fotxn_discard_all(
    WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *session)
{
	WT_FOTXN *fotxn;
	size_t i;

	/*
	 * This function is called during WT_CONNECTION.close to discard any
	 * memory that remains.  For that reason, we take two WT_SESSION_IMPL
	 * arguments: session_safe is still linked to the WT_CONNECTION and
	 * can be safely used for calls to other WiredTiger functions, while
	 * session is the WT_SESSION_IMPL we're cleaning up.
	 */
	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_cnt;
	    ++i, ++fotxn)
		if (fotxn->p != NULL)
			__wt_free(session_safe, fotxn->p);

	__wt_free(session_safe, session->fotxn);
}
