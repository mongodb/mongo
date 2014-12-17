/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_txnid_cmp --
 *	Compare transaction IDs for sorting / searching.
 */
int
__wt_txnid_cmp(const void *v1, const void *v2)
{
	uint64_t id1, id2;

	id1 = *(uint64_t *)v1;
	id2 = *(uint64_t *)v2;

	return ((id1 == id2) ? 0 : TXNID_LT(id1, id2) ? -1 : 1);
}

/*
 * __txn_sort_snapshot --
 *	Sort a snapshot for faster searching and set the min/max bounds.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session, uint32_t n, uint64_t snap_max)
{
	WT_TXN *txn;

	txn = &session->txn;

	if (n > 1)
		qsort(txn->snapshot, n, sizeof(uint64_t), __wt_txnid_cmp);
	txn->snapshot_count = n;
	txn->snap_max = snap_max;
	txn->snap_min = (n > 0 && TXNID_LE(txn->snapshot[0], snap_max)) ?
	    txn->snapshot[0] : snap_max;
	F_SET(txn, TXN_HAS_SNAPSHOT);
	WT_ASSERT(session, n == 0 || txn->snap_min != WT_TXN_NONE);
}

/*
 * __wt_txn_release_snapshot --
 *	Release the snapshot in the current transaction.
 */
void
__wt_txn_release_snapshot(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = &S2C(session)->txn_global.states[session->id];

	if (txn_state->snap_min != WT_TXN_NONE) {
		WT_ASSERT(session,
		    session->txn.isolation == TXN_ISO_READ_UNCOMMITTED ||
		    !__wt_txn_visible_all(session, txn_state->snap_min));
		txn_state->snap_min = WT_TXN_NONE;
	}
	F_CLR(txn, TXN_HAS_SNAPSHOT);
}

/*
 * __wt_txn_update_oldest --
 *	Sweep the running transactions to update the oldest ID required.
 */
void
__wt_txn_update_oldest(WT_SESSION_IMPL *session)
{
	/*
	 * !!!
	 * If a data-source is calling the WT_EXTENSION_API.transaction_oldest
	 * method (for the oldest transaction ID not yet visible to a running
	 * transaction), and then comparing that oldest ID against committed
	 * transactions to see if updates for a committed transaction are still
	 * visible to running transactions, the oldest transaction ID may be
	 * the same as the last committed transaction ID, if the transaction
	 * state wasn't refreshed after the last transaction committed.  Push
	 * past the last committed transaction.
	 */
	__wt_txn_refresh(session, 0);
}

/*
 * __wt_txn_refresh --
 *	Allocate a transaction ID and/or a snapshot.
 */
void
__wt_txn_refresh(WT_SESSION_IMPL *session, int get_snapshot)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s, *txn_state;
	uint64_t current_id, id, oldest_id;
	uint64_t prev_oldest_id, snap_min;
	uint32_t i, n, oldest_session, session_cnt;
	int32_t count;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	current_id = snap_min = txn_global->current;
	prev_oldest_id = txn_global->oldest_id;

	/* For pure read-only workloads, avoid scanning. */
	if (prev_oldest_id == current_id) {
		if (get_snapshot) {
			txn_state->snap_min = current_id;
			__txn_sort_snapshot(session, 0, current_id);
		}
		/* Check that the oldest ID has not moved in the meantime. */
		if (prev_oldest_id == txn_global->oldest_id &&
		    txn_global->scan_count == 0)
			return;
	}

	/*
	 * We're going to scan.  Increment the count of scanners to prevent the
	 * oldest ID from moving forwards.  Spin if the count is negative,
	 * which indicates that some thread is moving the oldest ID forwards.
	 */
	do {
		if ((count = txn_global->scan_count) < 0)
			WT_PAUSE();
	} while (count < 0 ||
	    !WT_ATOMIC_CAS4(txn_global->scan_count, count, count + 1));

	/* The oldest ID cannot change until the scan count goes to zero. */
	prev_oldest_id = txn_global->oldest_id;
	current_id = oldest_id = snap_min = txn_global->current;
	oldest_session = 0;

	/* Walk the array of concurrent transactions. */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = n = 0, s = txn_global->states; i < session_cnt; i++, s++) {
		/*
		 * Build our snapshot of any concurrent transaction IDs.
		 *
		 * Ignore our own ID: we always read our own updates.
		 *
		 * Also ignore the ID if it is older than the oldest ID we saw.
		 * This can happen if we race with a thread that is allocating
		 * an ID -- the ID will not be used because the thread will
		 * keep spinning until it gets a valid one.
		 */
		if (s != txn_state &&
		    (id = s->id) != WT_TXN_NONE &&
		    TXNID_LE(prev_oldest_id, id)) {
			if (get_snapshot)
				txn->snapshot[n++] = id;
			if (TXNID_LT(id, snap_min))
				snap_min = id;
		}

		/*
		 * Ignore the session's own snap_min: we are about to update
		 * it.
		 */
		if (get_snapshot && s == txn_state)
			continue;

		/*
		 * !!!
		 * Note: Don't ignore snap_min values older than the previous
		 * oldest ID.  Read-uncommitted operations publish snap_min
		 * values without incrementing scan_count to protect the global
		 * table.  See the comment in __wt_txn_cursor_op for
		 * more details.
		 */
		if ((id = s->snap_min) != WT_TXN_NONE &&
		    TXNID_LT(id, oldest_id)) {
			oldest_id = id;
			oldest_session = i;
		}
	}

	if (TXNID_LT(snap_min, oldest_id))
		oldest_id = snap_min;
	if (txn->id != WT_TXN_NONE && TXNID_LT(txn->id, oldest_id))
		oldest_id = txn->id;

	/*
	 * If we got a new snapshot, update the published snap_min for this
	 * session.
	 */
	if (get_snapshot) {
		WT_ASSERT(session, TXNID_LE(prev_oldest_id, snap_min));
		WT_ASSERT(session, prev_oldest_id == txn_global->oldest_id);
		txn_state->snap_min = snap_min;
	}

	/*
	 * Update the last running ID if we have a much newer value or we are
	 * forcing an update.
	 */
	if (!get_snapshot || snap_min > txn_global->last_running + 100)
		txn_global->last_running = snap_min;

	/*
	 * Update the oldest ID if we have a newer ID and we can get exclusive
	 * access.  During normal snapshot refresh, only do this if we have a
	 * much newer value.  Once we get exclusive access, do another pass to
	 * make sure nobody else is using an earlier ID.
	 */
	if (TXNID_LT(prev_oldest_id, oldest_id) &&
	    (!get_snapshot || oldest_id - prev_oldest_id > 100) &&
	    WT_ATOMIC_CAS4(txn_global->scan_count, 1, -1)) {
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = 0, s = txn_global->states; i < session_cnt; i++, s++) {
			if ((id = s->id) != WT_TXN_NONE &&
			    TXNID_LT(id, oldest_id))
				oldest_id = id;
			if ((id = s->snap_min) != WT_TXN_NONE &&
			    TXNID_LT(id, oldest_id))
				oldest_id = id;
		}
		if (TXNID_LT(txn_global->oldest_id, oldest_id))
			txn_global->oldest_id = oldest_id;
		txn_global->scan_count = 0;
	} else {
		if (WT_VERBOSE_ISSET(session, WT_VERB_TRANSACTION) &&
		    current_id - oldest_id > 10000 &&
		    txn_global->oldest_session != oldest_session) {
			(void)__wt_verbose(session, WT_VERB_TRANSACTION,
			    "old snapshot %" PRIu64
			    " pinned in session %d [%s]"
			    " with snap_min %" PRIu64 "\n",
			    oldest_id, oldest_session,
			    conn->sessions[oldest_session].lastop,
			    conn->sessions[oldest_session].txn.snap_min);
			txn_global->oldest_session = oldest_session;
		}
		WT_ASSERT(session, txn_global->scan_count > 0);
		(void)WT_ATOMIC_SUB4(txn_global->scan_count, 1);
	}

	if (get_snapshot)
		__txn_sort_snapshot(session, n, current_id);
}

/*
 * __wt_txn_begin --
 *	Begin a transaction.
 */
int
__wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;

	txn = &session->txn;

	WT_RET(__wt_config_gets_def(session, cfg, "isolation", 0, &cval));
	if (cval.len == 0)
		txn->isolation = session->isolation;
	else
		txn->isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    TXN_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-committed", cval.str, cval.len) ?
		    TXN_ISO_READ_COMMITTED : TXN_ISO_READ_UNCOMMITTED;

	/*
	 * The default sync setting is inherited from the connection, but can
	 * be overridden by an explicit "sync" setting for this transaction.
	 *
	 * !!! This is an unusual use of the config code: the "default" value
	 * we pass in is inherited from the connection.  If flush is not set in
	 * the connection-wide flag and not overridden here, we end up clearing
	 * all flags.
	 */
	txn->txn_logsync = S2C(session)->txn_logsync;
	WT_RET(__wt_config_gets_def(session, cfg, "sync",
	    FLD_ISSET(txn->txn_logsync, WT_LOG_FLUSH) ? 1 : 0, &cval));
	if (!cval.val)
		txn->txn_logsync = 0;

	F_SET(txn, TXN_RUNNING);
	if (txn->isolation == TXN_ISO_SNAPSHOT) {
		if (session->ncursors > 0)
			WT_RET(__wt_session_copy_values(session));
		__wt_txn_refresh(session, 1);
	}
	return (0);
}

/*
 * __wt_txn_release --
 *	Release the resources associated with the current transaction.
 */
void
__wt_txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	WT_ASSERT(session, txn->mod_count == 0);
	txn->notify = NULL;

	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/* Clear the transaction's ID from the global table. */
	if (F_ISSET(txn, TXN_HAS_ID)) {
		WT_ASSERT(session, txn_state->id != WT_TXN_NONE &&
		    txn->id != WT_TXN_NONE);
		WT_PUBLISH(txn_state->id, WT_TXN_NONE);
		txn->id = WT_TXN_NONE;
	}

	/* Free the scratch buffer allocated for logging. */
	__wt_logrec_free(session, &txn->logrec);

	/* Discard any memory from the session's split stash that we can. */
	if (session->split_stash_cnt > 0)
		__wt_split_stash_discard(session);

	/*
	 * Reset the transaction state to not running and release the snapshot.
	 */
	__wt_txn_release_snapshot(session);
	txn->isolation = session->isolation;
	F_CLR(txn, TXN_ERROR | TXN_HAS_ID | TXN_RUNNING);
}

/*
 * __wt_txn_commit --
 *	Commit the current transaction.
 */
int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_OP *op;
	u_int i;

	txn = &session->txn;
	WT_ASSERT(session, !F_ISSET(txn, TXN_ERROR));

	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/* Commit notification. */
	if (txn->notify != NULL)
		WT_TRET(txn->notify->notify(txn->notify,
		    (WT_SESSION *)session, txn->id, 1));

	/* If we are logging, write a commit log record. */
	if (ret == 0 && txn->mod_count > 0 &&
	    FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED) &&
	    !F_ISSET(session, WT_SESSION_NO_LOGGING))
		ret = __wt_txn_log_commit(session, cfg);

	/*
	 * If anything went wrong, roll back.
	 *
	 * !!!
	 * Nothing can fail after this point.
	 */
	if (ret != 0) {
		WT_TRET(__wt_txn_rollback(session, cfg));
		return (ret);
	}

	/* Free memory associated with updates. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++)
		__wt_txn_op_free(session, op);
	txn->mod_count = 0;

	/*
	 * We are about to release the snapshot: copy values into any
	 * positioned cursors so they don't point to updates that could be
	 * freed once we don't have a transaction ID pinned.
	 */
	if (session->ncursors > 0)
		WT_RET(__wt_session_copy_values(session));

	__wt_txn_release(session);
	return (0);
}

/*
 * __wt_txn_rollback --
 *	Roll back the current transaction.
 */
int
__wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_OP *op;
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/* Rollback notification. */
	if (txn->notify != NULL)
		WT_TRET(txn->notify->notify(txn->notify, (WT_SESSION *)session,
		    txn->id, 0));

	/* Rollback updates. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		/* Metadata updates are never rolled back. */
		if (op->fileid == WT_METAFILE_ID)
			continue;

		switch (op->type) {
		case TXN_OP_BASIC:
		case TXN_OP_INMEM:
			op->u.upd->txnid = WT_TXN_ABORTED;
			break;
		case TXN_OP_REF:
			__wt_delete_page_rollback(session, op->u.ref);
			break;
		case TXN_OP_TRUNCATE_COL:
		case TXN_OP_TRUNCATE_ROW:
			/*
			 * Nothing to do: these operations are only logged for
			 * recovery.  The in-memory changes will be rolled back
			 * with a combination of TXN_OP_REF and TXN_OP_INMEM
			 * operations.
			 */
			break;
		}

		/* Free any memory allocated for the operation. */
		__wt_txn_op_free(session, op);
	}
	txn->mod_count = 0;

	__wt_txn_release(session);
	return (ret);
}

/*
 * __wt_txn_init --
 *	Initialize a session's transaction data.
 */
int
__wt_txn_init(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	txn->id = WT_TXN_NONE;

	WT_RET(__wt_calloc_def(session,
	    S2C(session)->session_size, &txn->snapshot));

#ifdef HAVE_DIAGNOSTIC
	if (S2C(session)->txn_global.states != NULL) {
		WT_TXN_STATE *txn_state;
		txn_state = &S2C(session)->txn_global.states[session->id];
		WT_ASSERT(session, txn_state->snap_min == WT_TXN_NONE);
	}
#endif

	/*
	 * Take care to clean these out in case we are reusing the transaction
	 * for eviction.
	 */
	txn->mod = NULL;

	txn->isolation = session->isolation;
	return (0);
}

/*
 * __wt_txn_stats_update --
 *	Update the transaction statistics for return to the application.
 */
void
__wt_txn_stats_update(WT_SESSION_IMPL *session)
{
	WT_TXN_GLOBAL *txn_global;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS *stats;

	conn = S2C(session);
	txn_global = &conn->txn_global;
	stats = &conn->stats;

	WT_STAT_SET(stats, txn_pinned_range,
	    txn_global->current - txn_global->oldest_id);
}

/*
 * __wt_txn_destroy --
 *	Destroy a session's transaction data.
 */
void
__wt_txn_destroy(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	__wt_free(session, txn->mod);
	__wt_free(session, txn->snapshot);
}

/*
 * __wt_txn_global_init --
 *	Initialize the global transaction state.
 */
int
__wt_txn_global_init(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	u_int i;

	WT_UNUSED(cfg);
	conn = S2C(session);

	txn_global = &conn->txn_global;
	txn_global->current = 1;
	txn_global->oldest_id = 1;
	txn_global->last_running = 1;

	WT_RET(__wt_calloc_def(
	    session, conn->session_size, &txn_global->states));
	for (i = 0, s = txn_global->states; i < conn->session_size; i++, s++)
		s->id = s->snap_min = WT_TXN_NONE;

	return (0);
}

/*
 * __wt_txn_global_destroy --
 *	Destroy the global transaction state.
 */
void
__wt_txn_global_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	if (txn_global != NULL)
		__wt_free(session, txn_global->states);
}
