/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

static inline int __wt_txn_id_check(WT_SESSION_IMPL *session);
static inline void __wt_txn_read_last(WT_SESSION_IMPL *session);

/*
 * __txn_next_op --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__txn_next_op(WT_SESSION_IMPL *session, WT_TXN_OP **opp)
{
	WT_TXN *txn;

	txn = &session->txn;
	*opp = NULL;

	/* 
	 * We're about to perform an update.
	 * Make sure we have allocated a transaction ID.
	 */
	WT_RET(__wt_txn_id_check(session));
	WT_ASSERT(session, F_ISSET(txn, TXN_HAS_ID));

	WT_RET(__wt_realloc_def(session, &txn->mod_alloc,
	    txn->mod_count + 1, &txn->mod));

	*opp = &txn->mod[txn->mod_count++];
	WT_CLEAR(**opp);
	(*opp)->fileid = S2BT(session)->id;
	return (0);
}

/*
 * __wt_txn_unmodify --
 *	If threads race making updates, they may discard the last referenced
 *	WT_UPDATE item while the transaction is still active.  This function
 *	removes the last update item from the "log".
 */
static inline void
__wt_txn_unmodify(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_HAS_ID)) {
		WT_ASSERT(session, txn->mod_count > 0);
		txn->mod_count--;
	}
}

/*
 * __wt_txn_modify --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__wt_txn_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_DECL_RET;
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = F_ISSET(session, WT_SESSION_LOGGING_INMEM) ?
	    TXN_OP_INMEM : TXN_OP_BASIC;
	op->u.upd = upd;
	upd->txnid = session->txn.id;
	return (ret);
}

/*
 * __wt_txn_modify_ref --
 *	Remember a WT_REF object modified by the current transaction.
 */
static inline int
__wt_txn_modify_ref(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = TXN_OP_REF;
	op->u.ref = ref;
	return (__wt_txn_log_op(session, NULL));
}

/*
 * __wt_txn_visible_all --
 *	Check if a given transaction ID is "globally visible".	This is, if
 *	all sessions in the system will see the transaction ID.
 */
static inline int
__wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id)
{
	uint64_t oldest_id;

	oldest_id = S2C(session)->txn_global.oldest_id;
	return (TXNID_LT(id, oldest_id));
}

/*
 * __wt_txn_visible --
 *	Can the current transaction see the given ID?
 */
static inline int
__wt_txn_visible(WT_SESSION_IMPL *session, uint64_t id)
{
	WT_TXN *txn;

	txn = &session->txn;

	/*
	 * Eviction only sees globally visible updates, or if there is a
	 * checkpoint transaction running, use its transaction.
	*/
	if (txn->isolation == TXN_ISO_EVICTION)
		return (__wt_txn_visible_all(session, id));

	/* Nobody sees the results of aborted transactions. */
	if (id == WT_TXN_ABORTED)
		return (0);

	/* Changes with no associated transaction are always visible. */
	if (id == WT_TXN_NONE)
		return (1);

	/*
	 * Read-uncommitted transactions see all other changes.
	 *
	 * All metadata reads are at read-uncommitted isolation.  That's
	 * because once a schema-level operation completes, subsequent
	 * operations must see the current version of checkpoint metadata, or
	 * they may try to read blocks that may have been freed from a file.
	 * Metadata updates use non-transactional techniques (such as the
	 * schema and metadata locks) to protect access to in-flight updates.
	 */
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED ||
	    S2BT_SAFE(session) == session->metafile)
		return (1);

	/* Transactions see their own changes. */
	if (id == txn->id)
		return (1);

	/*
	 * TXN_ISO_SNAPSHOT, TXN_ISO_READ_COMMITTED: the ID is visible if it is
	 * not the result of a concurrent transaction, that is, if was
	 * committed before the snapshot was taken.
	 *
	 * The order here is important: anything newer than the maximum ID we
	 * saw when taking the snapshot should be invisible, even if the
	 * snapshot is empty.
	 */
	if (TXNID_LE(txn->snap_max, id))
		return (0);
	if (txn->snapshot_count == 0 || TXNID_LT(id, txn->snap_min))
		return (1);

	return (bsearch(&id, txn->snapshot, txn->snapshot_count,
	    sizeof(uint64_t), __wt_txnid_cmp) == NULL);
}

/*
 * __wt_txn_read --
 *	Get the first visible update in a list (or NULL if none are visible).
 */
static inline WT_UPDATE *
__wt_txn_read(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	while (upd != NULL && !__wt_txn_visible(session, upd->txnid))
		upd = upd->next;

	return (upd);
}

/*
 * __wt_txn_autocommit_check --
 *      If an auto-commit transaction is required, start one.
*/
static inline int
__wt_txn_autocommit_check(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_AUTOCOMMIT)) {
		F_CLR(txn, TXN_AUTOCOMMIT);
		return (__wt_txn_begin(session, NULL));
	}
	return (0);
}

/*
 * __wt_txn_new_id --
 *	Allocate a new transaction ID.
 */
static inline uint64_t
__wt_txn_new_id(WT_SESSION_IMPL *session)
{
	/*
	 * We want the global value to lead the allocated values, so that any
	 * allocated transaction ID eventually becomes globally visible.  When
	 * there are no transactions running, the oldest_id will reach the
	 * global current ID, so we want post-increment semantics.  Our atomic
	 * add primitive does pre-increment, so adjust the result here.
	 */
	return (WT_ATOMIC_ADD8(S2C(session)->txn_global.current, 1) - 1);
}

/*
 * __wt_txn_id_check --
 *	A transaction is going to do an update, start an auto commit
 *      transaction if required and allocate a transaction ID.
 */
static inline int
__wt_txn_id_check(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;

	WT_ASSERT(session, F_ISSET(txn, TXN_RUNNING));
	if (!F_ISSET(txn, TXN_HAS_ID)) {
		conn = S2C(session);
		txn_global = &conn->txn_global;
		txn_state = &txn_global->states[session->id];

		WT_ASSERT(session, txn_state->id == WT_TXN_NONE);

		/*
		 * Allocate a transaction ID.
		 *
		 * We use an atomic compare and swap to ensure that we get a
		 * unique ID that is published before the global counter is
		 * updated.
		 *
		 * If two threads race to allocate an ID, only the latest ID
		 * will proceed.  The winning thread can be sure its snapshot
		 * contains all of the earlier active IDs.  Threads that race
		 * and get an earlier ID may not appear in the snapshot, but
		 * they will loop and allocate a new ID before proceeding to
		 * make any updates.
		 *
		 * This potentially wastes transaction IDs when threads race to
		 * begin transactions: that is the price we pay to keep this
		 * path latch free.
		 */
		do {
			txn_state->id = txn->id = txn_global->current;
		} while (!WT_ATOMIC_CAS8(
		    txn_global->current, txn->id, txn->id + 1));

		/*
		 * If we have used 64-bits of transaction IDs, there is nothing
		 * more we can do.
		 */
		if (txn->id == WT_TXN_ABORTED)
			WT_RET_MSG(session, ENOMEM, "Out of transaction IDs");
		F_SET(txn, TXN_HAS_ID);
	}

	return (0);
}

/*
 * __wt_txn_update_check --
 *	Check if the current transaction can update an item.
 */
static inline int
__wt_txn_update_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (txn->isolation == TXN_ISO_SNAPSHOT)
		while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
			if (upd->txnid != WT_TXN_ABORTED) {
				WT_STAT_FAST_DATA_INCR(
				    session, txn_update_conflict);
				return (WT_ROLLBACK);
			}
			upd = upd->next;
		}

	return (0);
}

/*
 * __wt_txn_read_last --
 *	Called when the last page for a session is released.
 */
static inline void
__wt_txn_read_last(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;

	/* Release the snap_min ID we put in the global table. */
	if (!F_ISSET(txn, TXN_RUNNING) ||
	    txn->isolation != TXN_ISO_SNAPSHOT)
		__wt_txn_release_snapshot(session);
}

/*
 * __wt_txn_cursor_op --
 *	Called for each cursor operation.
 */
static inline void
__wt_txn_cursor_op(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/*
	 * If there is no transaction running (so we don't have an ID), and no
	 * snapshot allocated, put an ID in the global table to prevent any
	 * update that we are reading from being trimmed to save memory.  Do a
	 * read before the write because this shared data is accessed a lot.
	 *
	 * !!!
	 * Note:  We are updating the global table unprotected, so the
	 * oldest_id may move past this ID if a scan races with this
	 * value being published.  That said, read-uncommitted operations
	 * always take the most recent version of a value, so for that version
	 * to be freed, two newer versions would have to be committed.	Putting
	 * this snap_min ID in the table prevents the oldest ID from moving
	 * further forward, so that once a read-uncommitted cursor is
	 * positioned on a value, it can't be freed.
	 */
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED &&
	    !F_ISSET(txn, TXN_HAS_ID) &&
	    TXNID_LT(txn_state->snap_min, txn_global->last_running))
		txn_state->snap_min = txn_global->last_running;

	if (txn->isolation != TXN_ISO_READ_UNCOMMITTED &&
	    !F_ISSET(txn, TXN_HAS_SNAPSHOT))
		__wt_txn_refresh(session, 1);
}

/*
 * __wt_txn_am_oldest --
 *	Am I the oldest transaction in the system?
 */
static inline int
__wt_txn_am_oldest(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	uint64_t id;
	uint32_t i, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	if (txn->id == WT_TXN_NONE)
		return (0);

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = txn_global->states;
	    i < session_cnt;
	    i++, s++)
		if ((id = s->id) != WT_TXN_NONE &&
		    TXNID_LT(id, txn->id))
			return (0);

	return (1);
}
