/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

static inline int __wt_txn_id_check(WT_SESSION_IMPL *session);
static inline void __wt_txn_read_last(WT_SESSION_IMPL *session);

typedef enum {
	WT_VISIBLE_FALSE=0,     /* Not a visible update */
	WT_VISIBLE_PREPARE=1,   /* Prepared update */
	WT_VISIBLE_TRUE=2       /* A visible update */
} WT_VISIBLE_TYPE;
#ifdef HAVE_TIMESTAMPS
/*
 * __wt_txn_timestamp_flags --
 *	Set txn related timestamp flags.
 */
static inline void
__wt_txn_timestamp_flags(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	if (session->dhandle == NULL)
		return;
	btree = S2BT(session);
	if (btree == NULL)
		return;
	if (FLD_ISSET(btree->assert_flags, WT_ASSERT_COMMIT_TS_ALWAYS))
		F_SET(&session->txn, WT_TXN_TS_COMMIT_ALWAYS);
	if (FLD_ISSET(btree->assert_flags, WT_ASSERT_COMMIT_TS_KEYS))
		F_SET(&session->txn, WT_TXN_TS_COMMIT_KEYS);
	if (FLD_ISSET(btree->assert_flags, WT_ASSERT_COMMIT_TS_NEVER))
		F_SET(&session->txn, WT_TXN_TS_COMMIT_NEVER);
}

#if WT_TIMESTAMP_SIZE == 8
#define	WT_WITH_TIMESTAMP_READLOCK(session, l, e)       e

/*
 * __wt_timestamp_cmp --
 *	Compare two timestamps.
 */
static inline int
__wt_timestamp_cmp(const wt_timestamp_t *ts1, const wt_timestamp_t *ts2)
{
	return (ts1->val == ts2->val ? 0 : (ts1->val > ts2->val ? 1 : -1));
}

/*
 * __wt_timestamp_set --
 *	Set a timestamp.
 */
static inline void
__wt_timestamp_set(wt_timestamp_t *dest, const wt_timestamp_t *src)
{
	dest->val = src->val;
}

/*
 * __wt_timestamp_subone --
 *	Subtract one from a timestamp.
 */
static inline void
__wt_timestamp_subone(wt_timestamp_t *ts)
{
	ts->val -= 1;
}

/*
 * __wt_timestamp_iszero --
 *	Check if a timestamp is equal to the special "zero" time.
 */
static inline bool
__wt_timestamp_iszero(const wt_timestamp_t *ts)
{
	return (ts->val == 0);
}

/*
 * __wt_timestamp_set_inf --
 *	Set a timestamp to the maximum value.
 */
static inline void
__wt_timestamp_set_inf(wt_timestamp_t *ts)
{
	ts->val = UINT64_MAX;
}

/*
 * __wt_timestamp_set_zero --
 *	Zero out a timestamp.
 */
static inline void
__wt_timestamp_set_zero(wt_timestamp_t *ts)
{
	ts->val = 0;
}

#else /* WT_TIMESTAMP_SIZE != 8 */

#define	WT_WITH_TIMESTAMP_READLOCK(s, l, e)	do {                    \
	__wt_readlock((s), (l));                                        \
	e;                                                              \
	__wt_readunlock((s), (l));                                      \
} while (0)

/*
 * __wt_timestamp_cmp --
 *	Compare two timestamps.
 */
static inline int
__wt_timestamp_cmp(const wt_timestamp_t *ts1, const wt_timestamp_t *ts2)
{
	return (memcmp(ts1->ts, ts2->ts, WT_TIMESTAMP_SIZE));
}

/*
 * __wt_timestamp_set --
 *	Set a timestamp.
 */
static inline void
__wt_timestamp_set(wt_timestamp_t *dest, const wt_timestamp_t *src)
{
	(void)memcpy(dest->ts, src->ts, WT_TIMESTAMP_SIZE);
}

/*
 * __wt_timestamp_iszero --
 *	Check if a timestamp is equal to the special "zero" time.
 */
static inline bool
__wt_timestamp_iszero(const wt_timestamp_t *ts)
{
	static const wt_timestamp_t zero_timestamp;

	return (memcmp(ts->ts, &zero_timestamp, WT_TIMESTAMP_SIZE) == 0);
}

/*
 * __wt_timestamp_set_inf --
 *	Set a timestamp to the maximum value.
 */
static inline void
__wt_timestamp_set_inf(wt_timestamp_t *ts)
{
	memset(ts->ts, 0xff, WT_TIMESTAMP_SIZE);
}

/*
 * __wt_timestamp_set_zero --
 *	Zero out a timestamp.
 */
static inline void
__wt_timestamp_set_zero(wt_timestamp_t *ts)
{
	memset(ts->ts, 0x00, WT_TIMESTAMP_SIZE);
}

/*
 * __wt_timestamp_subone --
 *	Subtract one from a timestamp.
 */
static inline void
__wt_timestamp_subone(wt_timestamp_t *ts)
{
	uint8_t *tsb;

	/*
	 * Complicated path for arbitrary-sized timestamps: start with the
	 * least significant byte, subtract one, continue to more significant
	 * bytes on underflow.
	 */
	for (tsb = ts->ts + WT_TIMESTAMP_SIZE - 1; tsb >= ts->ts; --tsb)
		if (--*tsb != 0xff)
			break;
}

#endif /* WT_TIMESTAMP_SIZE == 8 */

#else /* !HAVE_TIMESTAMPS */

#define	__wt_timestamp_set(dest, src)
#define	__wt_timestamp_set_inf(ts)
#define	__wt_timestamp_set_zero(ts)
#define	__wt_timestamp_subone(ts)
#define	__wt_txn_clear_commit_timestamp(session)
#define	__wt_txn_clear_read_timestamp(session)
#define	__wt_txn_timestamp_flags(session)

#endif /* HAVE_TIMESTAMPS */

/*
 * __txn_next_op --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__txn_next_op(WT_SESSION_IMPL *session, WT_TXN_OP **opp)
{
	WT_TXN *txn;

	*opp = NULL;

	txn = &session->txn;

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

#ifdef HAVE_TIMESTAMPS
/*
 * __wt_txn_update_needs_timestamp --
 *	Decide whether to copy a commit timestamp into an update. If the op
 *	structure doesn't have a populated update or ref field or in prepared
 *      state there won't be any check for an existing timestamp.
 */
static inline bool
__wt_txn_update_needs_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
	WT_TXN *txn;
	wt_timestamp_t *timestamp;

	txn = &session->txn;

	/*
	 * The timestamp is in the page deleted structure for truncates, or
	 * in the update for other operations.
	 */
	if (op->type == WT_TXN_OP_REF_DELETE)
		timestamp = op->u.ref == NULL || op->u.ref->page_del == NULL ?
		    NULL : &op->u.ref->page_del->timestamp;
	else
		timestamp = op->u.upd == NULL ? NULL : &op->u.upd->timestamp;

	/*
	 * Updates in the metadata never get timestamps (either now or at
	 * commit): metadata cannot be read at a point in time, only the most
	 * recently committed data matches files on disk.
	 */
	return (op->fileid != WT_METAFILE_ID &&
	    F_ISSET(txn, WT_TXN_HAS_TS_COMMIT) &&
	    (timestamp == NULL || __wt_timestamp_iszero(timestamp) ||
	    F_ISSET(txn, WT_TXN_PREPARE)));
}
#endif

/*
 * __wt_txn_modify --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__wt_txn_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_TXN *txn;
	WT_TXN_OP *op;

	txn = &session->txn;

	if (F_ISSET(txn, WT_TXN_READONLY))
		WT_RET_MSG(session, WT_ROLLBACK,
		    "Attempt to update in a read-only transaction");

	WT_RET(__txn_next_op(session, &op));
	op->type = F_ISSET(session, WT_SESSION_LOGGING_INMEM) ?
	    WT_TXN_OP_INMEM : WT_TXN_OP_BASIC;
#ifdef HAVE_TIMESTAMPS
	if (__wt_txn_update_needs_timestamp(session, op))
		__wt_timestamp_set(&upd->timestamp, &txn->commit_timestamp);
#endif
	op->u.upd = upd;
	upd->txnid = session->txn.id;
	return (0);
}

/*
 * __wt_txn_modify_page_delete --
 *	Remember a page truncated by the current transaction.
 */
static inline int
__wt_txn_modify_page_delete(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_OP *op;

	txn = &session->txn;

	WT_RET(__txn_next_op(session, &op));
	op->type = WT_TXN_OP_REF_DELETE;

#ifdef HAVE_TIMESTAMPS
	if (__wt_txn_update_needs_timestamp(session, op))
		__wt_timestamp_set(
		    &ref->page_del->timestamp, &txn->commit_timestamp);
#endif
	op->u.ref = ref;
	ref->page_del->txnid = txn->id;

	WT_ERR(__wt_txn_log_op(session, NULL));
	return (0);

err:	__wt_txn_unmodify(session);
	return (ret);
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
	uint64_t checkpoint_pinned, oldest_id;
	bool include_checkpoint_txn;

	txn_global = &S2C(session)->txn_global;
	btree = S2BT_SAFE(session);

	/*
	 * The metadata is tracked specially because of optimizations for
	 * checkpoints.
	 */
	if (session->dhandle != NULL && WT_IS_METADATA(session->dhandle))
		return (txn_global->metadata_pinned);

	/*
	 * Take a local copy of these IDs in case they are updated while we are
	 * checking visibility.
	 */
	oldest_id = txn_global->oldest_id;
	include_checkpoint_txn = btree == NULL ||
	    (!F_ISSET(btree, WT_BTREE_LOOKASIDE) &&
	    btree->checkpoint_gen != __wt_gen(session, WT_GEN_CHECKPOINT));
	if (!include_checkpoint_txn)
		return (oldest_id);

	/*
	 * The read of the transaction ID pinned by a checkpoint needs to be
	 * carefully ordered: if a checkpoint is starting and we have to start
	 * checking the pinned ID, we take the minimum of it with the oldest
	 * ID, which is what we want.
	 */
	WT_READ_BARRIER();

	/*
	 * Checkpoint transactions often fall behind ordinary application
	 * threads.  Take special effort to not keep changes pinned in cache
	 * if they are only required for the checkpoint and it has already
	 * seen them.
	 *
	 * If there is no active checkpoint or this handle is up to date with
	 * the active checkpoint then it's safe to ignore the checkpoint ID in
	 * the visibility check.
	 */
	checkpoint_pinned = txn_global->checkpoint_state.pinned_id;
	if (checkpoint_pinned == WT_TXN_NONE ||
	    WT_TXNID_LT(oldest_id, checkpoint_pinned))
		return (oldest_id);

	return (checkpoint_pinned);
}

/*
 * __txn_visible_all_id --
 *	Check if a given transaction ID is "globally visible".	This is, if
 *	all sessions in the system will see the transaction ID including the
 *	ID that belongs to a running checkpoint.
 */
static inline bool
__txn_visible_all_id(WT_SESSION_IMPL *session, uint64_t id)
{
	uint64_t oldest_id;

	oldest_id = __wt_txn_oldest_id(session);

	return (WT_TXNID_LT(id, oldest_id));
}

/*
 * __wt_txn_visible_all --
 *	Check if a given transaction is "globally visible". This is, if all
 *	sessions in the system will see the transaction ID including the ID
 *	that belongs to a running checkpoint.
 */
static inline bool
__wt_txn_visible_all(
    WT_SESSION_IMPL *session, uint64_t id, const wt_timestamp_t *timestamp)
{
	if (!__txn_visible_all_id(session, id))
		return (false);

#ifdef HAVE_TIMESTAMPS
	{
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	int cmp;

	/* Timestamp check. */
	if (timestamp == NULL || __wt_timestamp_iszero(timestamp))
		return (true);

	/*
	 * If no oldest timestamp has been supplied, updates have to stay in
	 * cache until we are shutting down.
	 */
	if (!txn_global->has_pinned_timestamp)
		return (F_ISSET(S2C(session), WT_CONN_CLOSING));

	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    cmp = __wt_timestamp_cmp(timestamp, &txn_global->pinned_timestamp));

	/*
	 * We can discard updates with timestamps less than or equal to the
	 * pinned timestamp.  This is different to the situation for
	 * transaction IDs, because we know that updates with timestamps are
	 * definitely committed (and in this case, that the transaction ID is
	 * globally visible).
	 */
	return (cmp <= 0);
	}
#else
	WT_UNUSED(timestamp);
	return (true);
#endif
}

/*
 * __wt_txn_upd_visible_all --
 *	Is the given update visible to all (possible) readers?
 */
static inline bool
__wt_txn_upd_visible_all(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	return (__wt_txn_visible_all(
	    session, upd->txnid, WT_TIMESTAMP_NULL(&upd->timestamp)));
}

/*
 * __txn_visible_id --
 *	Can the current transaction see the given ID?
 */
static inline bool
__txn_visible_id(WT_SESSION_IMPL *session, uint64_t id)
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
		return (__txn_visible_all_id(session, id));

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
 * __wt_txn_visible --
 *	Can the current transaction see the given ID / timestamp?
 */
static inline bool
__wt_txn_visible(
    WT_SESSION_IMPL *session, uint64_t id, const wt_timestamp_t *timestamp)
{
	if (!__txn_visible_id(session, id))
		return (false);

	/* Transactions read their writes, regardless of timestamps. */
	if (F_ISSET(&session->txn, WT_TXN_HAS_ID) && id == session->txn.id)
		return (true);

#ifdef HAVE_TIMESTAMPS
	{
	WT_TXN *txn = &session->txn;

	/* Timestamp check. */
	if (!F_ISSET(txn, WT_TXN_HAS_TS_READ) || timestamp == NULL)
		return (true);

	return (__wt_timestamp_cmp(timestamp, &txn->read_timestamp) <= 0);
	}
#else
	WT_UNUSED(timestamp);
	return (true);
#endif
}

/*
 * __wt_txn_upd_visible_type --
 *      Visible type of given update for the current transaction.
 */
static inline WT_VISIBLE_TYPE
__wt_txn_upd_visible_type(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	uint8_t prepare_state, previous_state;
	bool upd_visible;

	for (;;__wt_yield()) {
		/* Prepare state change is in progress, yield and try again. */
		WT_ORDERED_READ(prepare_state, upd->prepare_state);
		if (prepare_state == WT_PREPARE_LOCKED)
			continue;

		upd_visible = __wt_txn_visible(
		    session, upd->txnid, WT_TIMESTAMP_NULL(&upd->timestamp));

		/*
		 * The visibility check is only valid if the update does not
		 * change state.  If the state does change, recheck visibility.
		 */
		previous_state = prepare_state;
		WT_ORDERED_READ(prepare_state, upd->prepare_state);
		if (previous_state == prepare_state)
			break;

		WT_STAT_CONN_INCR(session, prepared_transition_blocked_page);
	}

	if (!upd_visible)
		return (WT_VISIBLE_FALSE);

	/* Ignore the prepared update, if transaction configuration says so. */
	if (prepare_state == WT_PREPARE_INPROGRESS)
		return (F_ISSET(&session->txn, WT_TXN_IGNORE_PREPARE) ?
		    WT_VISIBLE_FALSE : WT_VISIBLE_PREPARE);

	return (WT_VISIBLE_TRUE);
}

/*
 * __wt_txn_upd_visible --
 *	Can the current transaction see the given update.
 */
static inline bool
__wt_txn_upd_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	return (__wt_txn_upd_visible_type(session, upd) == WT_VISIBLE_TRUE);
}

/*
 * __wt_txn_read --
 *	Get the first visible update in a list (or NULL if none are visible).
 */
static inline int
__wt_txn_read(WT_SESSION_IMPL *session, WT_UPDATE *upd, WT_UPDATE **updp)
{
	static WT_UPDATE tombstone = {
		.txnid = WT_TXN_NONE, .type = WT_UPDATE_TOMBSTONE
	};
	WT_VISIBLE_TYPE upd_visible;
	bool skipped_birthmark;

	*updp = NULL;
	for (skipped_birthmark = false; upd != NULL; upd = upd->next) {
		/* Skip reserved place-holders, they're never visible. */
		if (upd->type != WT_UPDATE_RESERVE) {
			upd_visible = __wt_txn_upd_visible_type(session, upd);
			if (upd_visible == WT_VISIBLE_TRUE)
				break;
			if (upd_visible == WT_VISIBLE_PREPARE)
				return (WT_PREPARE_CONFLICT);
		}
		/* An invisible birthmark is equivalent to a tombstone. */
		if (upd->type == WT_UPDATE_BIRTHMARK)
			skipped_birthmark = true;
	}

	if (upd == NULL && skipped_birthmark)
		upd = &tombstone;

	*updp = upd == NULL || upd->type == WT_UPDATE_BIRTHMARK ? NULL : upd;
	return (0);
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

		/* Stall here if the cache is completely full. */
		WT_RET(__wt_cache_eviction_check(session, false, true, NULL));

		__wt_txn_get_snapshot(session);
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
	 * WT_TXN_HAS_SNAPSHOT.  We don't have any transaction information at
	 * this point, so assume the transaction will be read-only.  The dirty
	 * cache check will be performed when the transaction completes, if
	 * necessary.
	 */
	if (F_ISSET(txn, WT_TXN_RUNNING) &&
	    !F_ISSET(txn, WT_TXN_HAS_ID) && txn_state->pinned_id == WT_TXN_NONE)
		WT_RET(__wt_cache_eviction_check(session, false, true, NULL));

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
	WT_TXN_STATE *txn_state;
	uint64_t id;

	txn_global = &S2C(session)->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);

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
	 * We want the global value to lead the allocated values, so that any
	 * allocated transaction ID eventually becomes globally visible.  When
	 * there are no transactions running, the oldest_id will reach the
	 * global current ID, so we want post-increment semantics.  Our atomic
	 * add primitive does pre-increment, so adjust the result here.
	 *
	 * We rely on atomic reads of the current ID to create snapshots, so
	 * for unlocked reads to be well defined, we must use an atomic
	 * increment here.
	 */
	__wt_spin_lock(session, &txn_global->id_lock);
	id = txn_global->current;

	if (publish) {
		session->txn.id = id;
		WT_PUBLISH(txn_state->id, id);
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
 *	A transaction is going to do an update, allocate a transaction ID.
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
		WT_RET_MSG(session, WT_ERROR, "out of transaction IDs");
	F_SET(txn, WT_TXN_HAS_ID);

	return (0);
}

/*
 * __wt_txn_search_check --
 *	Check if the current transaction can search.
 */
static inline int
__wt_txn_search_check(WT_SESSION_IMPL *session)
{
#ifdef  HAVE_TIMESTAMPS
	WT_BTREE *btree;
	WT_TXN *txn;

	txn = &session->txn;
	btree = S2BT(session);
	/*
	 * If the user says a table should always use a read timestamp,
	 * verify this transaction has one.  Same if it should never have
	 * a read timestamp.
	 */
	if (FLD_ISSET(btree->assert_flags, WT_ASSERT_READ_TS_ALWAYS) &&
	    !F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		WT_RET_MSG(session, EINVAL, "read_timestamp required and "
		    "none set on this transaction");
	if (FLD_ISSET(btree->assert_flags, WT_ASSERT_READ_TS_NEVER) &&
	    F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		WT_RET_MSG(session, EINVAL, "no read_timestamp required and "
		    "timestamp set on this transaction");
#endif
	WT_UNUSED(session);
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
	bool ignore_prepare_set;

	txn = &session->txn;
	if (txn->isolation != WT_ISO_SNAPSHOT)
		return (0);

	/*
	 * Clear the ignore prepare setting of txn, as it is not supposed, to
	 * affect the visibility for update operations.
	 */
	ignore_prepare_set = F_ISSET(txn, WT_TXN_IGNORE_PREPARE);
	F_CLR(txn, WT_TXN_IGNORE_PREPARE);
	for (;upd != NULL && !__wt_txn_upd_visible(session, upd);
	    upd = upd->next) {
		if (upd->txnid != WT_TXN_ABORTED) {
			if (ignore_prepare_set)
				F_SET(txn, WT_TXN_IGNORE_PREPARE);
			WT_STAT_CONN_INCR(session, txn_update_conflict);
			WT_STAT_DATA_INCR(session, txn_update_conflict);
			return (__wt_txn_rollback_required(session,
			    "conflict between concurrent operations"));
		}
	}

	if (ignore_prepare_set)
		F_SET(txn, WT_TXN_IGNORE_PREPARE);
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
	    txn->isolation != WT_ISO_SNAPSHOT) && txn->forced_iso == 0)
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
		if (txn_state->pinned_id == WT_TXN_NONE)
			txn_state->pinned_id = txn_global->last_running;
		if (txn_state->metadata_pinned == WT_TXN_NONE)
			txn_state->metadata_pinned = txn_state->pinned_id;
	} else if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
		__wt_txn_get_snapshot(session);
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

/*
 * __wt_txn_activity_check --
 *	Check whether there are any running transactions.
 */
static inline int
__wt_txn_activity_check(WT_SESSION_IMPL *session, bool *txn_active)
{
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;

	/*
	 * Ensure the oldest ID is as up to date as possible so we can use a
	 * simple check to find if there are any running transactions.
	 */
	WT_RET(__wt_txn_update_oldest(session,
	    WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

	*txn_active = (txn_global->oldest_id != txn_global->current ||
	    txn_global->metadata_pinned != txn_global->current);

	return (0);
}
