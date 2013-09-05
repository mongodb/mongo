/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

static inline void __wt_txn_read_first(WT_SESSION_IMPL *session);
static inline void __wt_txn_read_last(WT_SESSION_IMPL *session);

static inline int
__txn_next_op(WT_SESSION_IMPL *session, WT_TXN_OP **opp)
{
	WT_TXN *txn;

	txn = &session->txn;
	WT_ASSERT(session, F_ISSET(txn, TXN_RUNNING));
	WT_RET(__wt_realloc_def(session, &txn->mod_alloc,
	    txn->mod_count + 1, &txn->mod));

	*opp = &txn->mod[txn->mod_count++];
	WT_CLEAR(**opp);
	return (0);
}

/*
 * __wt_txn_modify --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__wt_txn_modify(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = F_ISSET(session, WT_SESSION_LOGGING_INMEM) ?
	    TXN_OP_INMEM : TXN_OP_BASIC;
	if (cbt->btree->type == BTREE_ROW)
		WT_RET(__wt_row_key_get(cbt, &op->u.op.key));
	op->u.op.ins = cbt->ins;
	op->u.op.upd = upd;
	op->fileid = S2BT(session)->id;
	upd->txnid = session->txn.id;
	return (0);
}

/*
 * __wt_txn_modify_ref --
 *	Mark a WT_REF object modified by the current transaction.
 */
static inline int
__wt_txn_modify_ref(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_TXN_OP *op;

	WT_RET(__txn_next_op(session, &op));
	op->type = TXN_OP_REF;
	op->u.ref = ref;
	ref->txnid = session->txn.id;
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
	if (F_ISSET(txn, TXN_RUNNING)) {
		WT_ASSERT(session, txn->mod_count > 0);
		txn->mod_count--;
	}
}

/*
 * __wt_txn_visible --
 *	Can the current transaction see the given ID?
 */
static inline int
__wt_txn_visible(WT_SESSION_IMPL *session, uint64_t id)
{
	WT_TXN *txn;

	/* Nobody sees the results of aborted transactions. */
	if (id == WT_TXN_ABORTED)
		return (0);

	/* Changes with no associated transaction are always visible. */
	if (id == WT_TXN_NONE)
		return (1);

	/* Transactions see their own changes. */
	txn = &session->txn;
	if (id == txn->id)
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
 * __wt_txn_visible_all --
 *	Check if a given transaction ID is "globally visible".	This is, if
 *	all sessions in the system will see the transaction ID.
 */
static inline int
__wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id)
{
	WT_TXN_GLOBAL *txn_global;
	uint64_t oldest_id;

	txn_global = &S2C(session)->txn_global;
	oldest_id = txn_global->oldest_id;
	return (TXNID_LT(id, oldest_id));
}

/*
 * __wt_txn_read_skip --
 *	Get the first visible update in a list (or NULL if none are visible),
 *	and report whether uncommitted changes were skipped.
 */
static inline WT_UPDATE *
__wt_txn_read_skip(WT_SESSION_IMPL *session, WT_UPDATE *upd, int *skipp)
{
	*skipp = 0;
	while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
		if (upd->txnid != WT_TXN_ABORTED)
			*skipp = 1;
		upd = upd->next;
	}

	return (upd);
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
				WT_DSTAT_INCR(session, txn_update_conflict);
				return (WT_DEADLOCK);
			}
			upd = upd->next;
		}

	return (0);
}

/*
 * __wt_txn_autocommit_check --
 *	If an auto-commit transaction is required, start one.
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
 * __wt_txn_read_first --
 *	Called for the first page read for a session.
 */
static inline void
__wt_txn_read_first(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/*
	 * If there is no transaction running, put an ID in the global table so
	 * the oldest reader in the system can be tracked.  This prevents any
	 * update the we are reading from being trimmed to save memory.
	 */
	WT_ASSERT(session, F_ISSET(txn, TXN_RUNNING) ||
	    (txn_state->id == WT_TXN_NONE &&
	    txn_state->snap_min == WT_TXN_NONE));

	if (txn->isolation == TXN_ISO_READ_COMMITTED ||
	    (!F_ISSET(txn, TXN_RUNNING) &&
	    txn->isolation == TXN_ISO_SNAPSHOT))
		__wt_txn_refresh(session, WT_TXN_NONE, 1);
	else if (!F_ISSET(txn, TXN_RUNNING))
		txn_state->snap_min = txn_global->current;
}

/*
 * __wt_txn_read_last --
 *	Called when the last page for a session is released.
 */
static inline void
__wt_txn_read_last(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = &S2C(session)->txn_global.states[session->id];

	/* Release the snap_min ID we put in the global table. */
	if (txn->isolation == TXN_ISO_READ_COMMITTED ||
	    (!F_ISSET(txn, TXN_RUNNING) &&
	    txn->isolation == TXN_ISO_SNAPSHOT))
		__wt_txn_release_snapshot(session);
	else if (!F_ISSET(txn, TXN_RUNNING))
		txn_state->snap_min = WT_TXN_NONE;
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
	uint64_t id, my_id;
	uint32_t i, session_cnt;

	/* Cache the result: if we're the oldest, don't keep checking. */
	txn = &session->txn;
	if (F_ISSET(txn, TXN_OLDEST))
		return (1);

	conn = S2C(session);
	txn_global = &conn->txn_global;

	/*
	 * Use this slightly convoluted way to get our ID, in case session->txn
	 * has been hijacked for eviction.
	 */
	s = &txn_global->states[session->id];
	if ((my_id = s->id) == WT_TXN_NONE)
		return (0);

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = txn_global->states;
	    i < session_cnt;
	    i++, s++)
		if ((id = s->id) != WT_TXN_NONE && TXNID_LT(id, my_id))
			return (0);

	F_SET(txn, TXN_OLDEST);
	return (1);
}
