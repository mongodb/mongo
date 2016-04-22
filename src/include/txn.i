/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
	WT_ASSERT(session, F_ISSET(txn, WT_TXN_HAS_ID));

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
	if (F_ISSET(txn, WT_TXN_HAS_ID)) {
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
	WT_TXN *txn;

	txn = &session->txn;

	if (F_ISSET(txn, WT_TXN_READONLY))
		WT_RET_MSG(session, WT_ROLLBACK,
		    "Attempt to update in a read only transaction");

	WT_RET(__txn_next_op(session, &op));
	op->type = F_ISSET(session, WT_SESSION_LOGGING_INMEM) ?
	    WT_TXN_OP_INMEM : WT_TXN_OP_BASIC;
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
	op->type = WT_TXN_OP_REF;
	op->u.ref = ref;
	return (__wt_txn_log_op(session, NULL));
}

/*
 * __wt_txn_oldest_id --
 *	Return the oldest transaction ID that has to be kept for the current
 *	tree.
 */
static inline uint64_t
__wt_txn_oldest_id(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_TXN_GLOBAL *txn_global;
	uint64_t checkpoint_gen, checkpoint_pinned, oldest_id;

	txn_global = &S2C(session)->txn_global;
	btree = S2BT_SAFE(session);

	/*
	 * Take a local copy of these IDs in case they are updated while we are
	 * checking visibility.  Only the generation needs to be carefully
	 * ordered: if a checkpoint is starting and the generation is bumped,
	 * we take the minimum of the other two IDs, which is what we want.
	 */
	oldest_id = txn_global->oldest_id;
	WT_ORDERED_READ(checkpoint_gen, txn_global->checkpoint_gen);
	checkpoint_pinned = txn_global->checkpoint_pinned;

	/*
	 * Checkpoint transactions often fall behind ordinary application
	 * threads.  Take special effort to not keep changes pinned in cache
	 * if they are only required for the checkpoint and it has already
	 * seen them.
	 *
	 * If there is no active checkpoint, this session is doing the
	 * checkpoint, or this handle is up to date with the active checkpoint
	 * then it's safe to ignore the checkpoint ID in the visibility check.
	 */
	if (checkpoint_pinned == WT_TXN_NONE ||
	    WT_TXNID_LT(oldest_id, checkpoint_pinned) ||
	    WT_SESSION_IS_CHECKPOINT(session) ||
	    (btree != NULL && btree->checkpoint_gen == checkpoint_gen))
		return (oldest_id);

	return (checkpoint_pinned);
}

/*
 * __wt_txn_committed --
 *	Return if a transaction has been committed.
 */
static inline bool
__wt_txn_committed(WT_SESSION_IMPL *session, uint64_t id)
{
	return (WT_TXNID_LT(id, S2C(session)->txn_global.last_running));
}

/*
 * __wt_txn_visible_all --
 *	Check if a given transaction ID is "globally visible".	This is, if
 *	all sessions in the system will see the transaction ID including the
 *	ID that belongs to a running checkpoint.
 */
static inline bool
__wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id)
{
	uint64_t oldest_id;

	oldest_id = __wt_txn_oldest_id(session);

	return (WT_TXNID_LT(id, oldest_id));
}

/*
 * __wt_txn_visible --
 *	Can the current transaction see the given ID?
 */
static inline bool
__wt_txn_visible(WT_SESSION_IMPL *session, uint64_t id)
{
	WT_TXN *txn;
	bool found;

	txn = &session->txn;

	/* Changes with no associated transaction are always visible. */
	if (id == WT_TXN_NONE)
		return (true);

	/* Nobody sees the results of aborted transactions. */
	if (id == WT_TXN_ABORTED)
		return (false);

	/* Read-uncommitted transactions see all other changes. */
	if (txn->isolation == WT_ISO_READ_UNCOMMITTED)
		return (true);

	/*
	 * If we don't have a transactional snapshot, only make stable updates
	 * visible.
	 */
	if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
		return (__wt_txn_visible_all(session, id));

	/* Transactions see their own changes. */
	if (id == txn->id)
		return (true);

	/*
	 * WT_ISO_SNAPSHOT, WT_ISO_READ_COMMITTED: the ID is visible if it is
	 * not the result of a concurrent transaction, that is, if was
	 * committed before the snapshot was taken.
	 *
	 * The order here is important: anything newer than the maximum ID we
	 * saw when taking the snapshot should be invisible, even if the
	 * snapshot is empty.
	 */
	if (WT_TXNID_LE(txn->snap_max, id))
		return (false);
	if (txn->snapshot_count == 0 || WT_TXNID_LT(id, txn->snap_min))
		return (true);

	WT_BINARY_SEARCH(id, txn->snapshot, txn->snapshot_count, found);
	return (!found);
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
 * __wt_txn_begin --
 *	Begin a transaction.
 */
static inline int
__wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_TXN *txn;

	txn = &session->txn;
	txn->isolation = session->isolation;
	txn->txn_logsync = S2C(session)->txn_logsync;

	if (cfg != NULL)
		WT_RET(__wt_txn_config(session, cfg));

	/*
	 * Allocate a snapshot if required. Named snapshot transactions already
	 * have an ID setup.
	 */
	if (txn->isolation == WT_ISO_SNAPSHOT &&
	    !F_ISSET(txn, WT_TXN_NAMED_SNAPSHOT)) {
		if (session->ncursors > 0)
			WT_RET(__wt_session_copy_values(session));

		/*
		 * We're about to allocate a snapshot: if we need to block for
		 * eviction, it's better to do it beforehand.
		 */
		WT_RET(__wt_cache_eviction_check(session, false, NULL));
		WT_RET(__wt_txn_get_snapshot(session));
	}

	F_SET(txn, WT_TXN_RUNNING);
	if (F_ISSET(S2C(session), WT_CONN_READONLY))
		F_SET(txn, WT_TXN_READONLY);

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
	if (F_ISSET(txn, WT_TXN_AUTOCOMMIT)) {
		F_CLR(txn, WT_TXN_AUTOCOMMIT);
		return (__wt_txn_begin(session, NULL));
	}
	return (0);
}

/*
 * __wt_txn_idle_cache_check --
 *	If there is no transaction active in this thread and we haven't checked
 *	if the cache is full, do it now.  If we have to block for eviction,
 *	this is the best time to do it.
 */
static inline int
__wt_txn_idle_cache_check(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = WT_SESSION_TXN_STATE(session);

	/*
	 * Check the published snap_min because read-uncommitted never sets
	 * WT_TXN_HAS_SNAPSHOT.
	 */
	if (F_ISSET(txn, WT_TXN_RUNNING) &&
	    !F_ISSET(txn, WT_TXN_HAS_ID) && txn_state->snap_min == WT_TXN_NONE)
		WT_RET(__wt_cache_eviction_check(session, false, NULL));

	return (0);
}

/*
 * __wt_txn_id_alloc --
 *	Allocate a new transaction ID.
 */
static inline uint64_t
__wt_txn_id_alloc(WT_SESSION_IMPL *session, bool publish)
{
	WT_TXN_GLOBAL *txn_global;
	uint64_t id;

	txn_global = &S2C(session)->txn_global;

	/*
	 * Allocating transaction IDs involves several steps.
	 *
	 * Firstly, we do an atomic increment to allocate a unique ID.  The
	 * field we increment is not used anywhere else.
	 *
	 * Then we optionally publish the allocated ID into the global
	 * transaction table.  It is critical that this becomes visible before
	 * the global current value moves past our ID, or some concurrent
	 * reader could get a snapshot that makes our changes visible before we
	 * commit.
	 *
	 * Lastly, we spin to update the current ID.  This is the only place
	 * that the current ID is updated, and it is in the same cache line as
	 * the field we allocate from, so we should usually succeed on the
	 * first try.
	 *
	 * We want the global value to lead the allocated values, so that any
	 * allocated transaction ID eventually becomes globally visible.  When
	 * there are no transactions running, the oldest_id will reach the
	 * global current ID, so we want post-increment semantics.  Our atomic
	 * add primitive does pre-increment, so adjust the result here.
	 */
	 __wt_spin_lock(session, &txn_global->id_lock);
	id = txn_global->current;

	if (publish) {
		session->txn.id = id;
		WT_PUBLISH(WT_SESSION_TXN_STATE(session)->id, id);
	}

	/*
	 * Even though we are in a spinlock, readers are not.  We rely on
	 * atomic reads of the current ID to create snapshots, so for unlocked
	 * reads to be well defined, we must use an atomic increment here.
	 */
	(void)__wt_atomic_addv64(&txn_global->current, 1);
	__wt_spin_unlock(session, &txn_global->id_lock);
	return (id);
}

/*
 * __wt_txn_id_check --
 *	A transaction is going to do an update, start an auto commit
 *	transaction if required and allocate a transaction ID.
 */
static inline int
__wt_txn_id_check(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;

	WT_ASSERT(session, F_ISSET(txn, WT_TXN_RUNNING));

	if (F_ISSET(txn, WT_TXN_HAS_ID))
		return (0);

	/* If the transaction is idle, check that the cache isn't full. */
	WT_RET(__wt_txn_idle_cache_check(session));

	(void)__wt_txn_id_alloc(session, true);

	/*
	 * If we have used 64-bits of transaction IDs, there is nothing
	 * more we can do.
	 */
	if (txn->id == WT_TXN_ABORTED)
		WT_RET_MSG(session, ENOMEM, "Out of transaction IDs");
	F_SET(txn, WT_TXN_HAS_ID);

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
	if (txn->isolation == WT_ISO_SNAPSHOT)
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

	/*
	 * Release the snap_min ID we put in the global table.
	 *
	 * If the isolation has been temporarily forced, don't touch the
	 * snapshot here: it will be restored by WT_WITH_TXN_ISOLATION.
	 */
	if ((!F_ISSET(txn, WT_TXN_RUNNING) ||
	    txn->isolation != WT_ISO_SNAPSHOT) &&
	    txn->forced_iso == 0)
		__wt_txn_release_snapshot(session);
}

/*
 * __wt_txn_cursor_op --
 *	Called for each cursor operation.
 */
static inline int
__wt_txn_cursor_op(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);

	/*
	 * We are about to read data, which means we need to protect against
	 * updates being freed from underneath this cursor. Read-uncommitted
	 * isolation protects values by putting a transaction ID in the global
	 * table to prevent any update that we are reading from being freed.
	 * Other isolation levels get a snapshot to protect their reads.
	 *
	 * !!!
	 * Note:  We are updating the global table unprotected, so the global
	 * oldest_id may move past our snap_min if a scan races with this value
	 * being published. That said, read-uncommitted operations always see
	 * the most recent update for each record that has not been aborted
	 * regardless of the snap_min value published here.  Even if there is a
	 * race while publishing this ID, it prevents the oldest ID from moving
	 * further forward, so that once a read-uncommitted cursor is
	 * positioned on a value, it can't be freed.
	 */
	if (txn->isolation == WT_ISO_READ_UNCOMMITTED) {
		if (txn_state->snap_min == WT_TXN_NONE)
			txn_state->snap_min = txn_global->last_running;
	} else if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
		WT_RET(__wt_txn_get_snapshot(session));

	return (0);
}

/*
 * __wt_txn_am_oldest --
 *	Am I the oldest transaction in the system?
 */
static inline bool
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
		return (false);

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = txn_global->states; i < session_cnt; i++, s++)
		if ((id = s->id) != WT_TXN_NONE && WT_TXNID_LT(id, txn->id))
			return (false);

	return (true);
}
