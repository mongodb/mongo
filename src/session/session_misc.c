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
	size_t i;

	/* Grow the list as necessary. */
	WT_RET(__wt_realloc_def(session,
	    &session->fotxn_size, session->fotxn_cnt + 1, &session->fotxn));

	/* Find an empty slot. */
	for (i = 0, fotxn = session->fotxn;
	    i < session->fotxn_size / sizeof(session->fotxn[0]);  ++i, ++fotxn)
		if (fotxn->p == NULL) {
			fotxn->txnid = S2C(session)->txn_global.current;
			fotxn->p = p;
			fotxn->len = len;
			break;
		}
	++session->fotxn_cnt;

	/* See if we can free any previous entries. */
	if (session->fotxn_cnt > 1)
		__wt_session_fotxn_discard(session, session, 0);

	return (0);
}

/*
 * __wt_session_fotxn_discard --
 *	Discard any memory from the session's free-on-transaction generation
 * list that we can.
 */
void
__wt_session_fotxn_discard(WT_SESSION_IMPL *session_safe,
    WT_SESSION_IMPL *session, int connection_close)
{
	WT_FOTXN *fotxn;
	uint64_t oldest_id;
	size_t i;

	/*
	 * This function is called during WT_CONNECTION.close to discard any
	 * memory that remains.  For that reason, we take two WT_SESSION_IMPL
	 * arguments: session_safe is still linked to the WT_CONNECTION and
	 * can be safely used for calls to other WiredTiger functions, while
	 * session is the WT_SESSION_IMPL we're cleaning up.
	 *
	 * Get the oldest transaction ID not yet visible to a running
	 * transaction.
	 */
	oldest_id = connection_close ?
	    WT_TXN_NONE : S2C(session_safe)->txn_global.oldest_id;

	for (i = 0, fotxn = session->fotxn;
	    session->fotxn_cnt > 0 &&
	    i < session->fotxn_size / sizeof(session->fotxn[0]);  ++i, ++fotxn)
		if (fotxn->p != NULL &&
		    (connection_close || TXNID_LT(fotxn->txnid, oldest_id))) {
			--session->fotxn_cnt;

			/*
			 * It's a bad thing if another thread is in this memory
			 * after we free it, make sure nothing good happens to
			 * that thread.
			 */
			__wt_overwrite_and_free_len(
			    session_safe, fotxn->p, fotxn->len);
		}
	if (connection_close)
		__wt_free(session_safe, session->fotxn);
}
