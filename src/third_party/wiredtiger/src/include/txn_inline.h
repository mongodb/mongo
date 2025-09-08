/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_txn_context_prepare_check --
 *     Return an error if the current transaction is in the prepare state.
 */
static WT_INLINE int
__wt_txn_context_prepare_check(WT_SESSION_IMPL *session)
{
    if (F_ISSET(session->txn, WT_TXN_PREPARE_IGNORE_API_CHECK))
        return (0);
    if (F_ISSET(session->txn, WT_TXN_PREPARE))
        WT_RET_MSG(session, EINVAL, "not permitted in a prepared transaction");
    return (0);
}

/*
 * __wt_txn_context_check --
 *     Complain if a transaction is/isn't running.
 */
static WT_INLINE int
__wt_txn_context_check(WT_SESSION_IMPL *session, bool requires_txn)
{
    if (requires_txn && !F_ISSET(session->txn, WT_TXN_RUNNING))
        WT_RET_MSG(session, EINVAL, "only permitted in a running transaction");
    if (!requires_txn && F_ISSET(session->txn, WT_TXN_RUNNING))
        WT_RET_MSG(session, EINVAL, "not permitted in a running transaction");
    return (0);
}

/*
 * __wt_txn_log_op_check --
 *     Return if an operation should be logged.
 */
static WT_INLINE bool
__wt_txn_log_op_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG_MANAGER *log_mgr;

    conn = S2C(session);
    log_mgr = &conn->log_mgr;

    /*
     * Objects with checkpoint durability don't need logging unless we're in debug mode. That rules
     * out almost all log records, check it first.
     */
    if (!F_ISSET(S2BT(session), WT_BTREE_LOGGED) &&
      !FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_TABLE_LOGGING))
        return (false);

    /*
     * Correct the above check for logging being configured. Files are configured for logging to
     * turn off timestamps, so stop here if there aren't actually any log files.
     */
    if (!FLD_ISSET(log_mgr->flags, WT_LOG_ENABLED))
        return (false);

    /* No logging during recovery. */
    if (F_ISSET(conn, WT_CONN_RECOVERING))
        return (false);

    return (true);
}

/*
 * __wt_txn_err_set --
 *     Set an error in the current transaction.
 */
static WT_INLINE void
__wt_txn_err_set(WT_SESSION_IMPL *session, int ret)
{
    WT_TXN *txn;

    txn = session->txn;

    /*  Ignore standard errors that don't fail the transaction. */
    if (ret == WT_NOTFOUND || ret == WT_DUPLICATE_KEY || ret == WT_PREPARE_CONFLICT)
        return;

    /* Less commonly, it's not a running transaction. */
    if (!F_ISSET(txn, WT_TXN_RUNNING))
        return;

    /* The transaction has to be rolled back. */
    F_SET(txn, WT_TXN_ERROR);
}

/*
 * __wt_txn_op_set_recno --
 *     Set the latest transaction operation with the given recno.
 */
static WT_INLINE void
__wt_txn_op_set_recno(WT_SESSION_IMPL *session, uint64_t recno)
{
    WT_TXN *txn;
    WT_TXN_OP *op;

    txn = session->txn;

    WT_ASSERT(session, txn->mod_count > 0 && recno != WT_RECNO_OOB);
    op = txn->mod + txn->mod_count - 1;

    if (WT_SESSION_IS_CHECKPOINT(session) || WT_IS_HS(op->btree->dhandle) ||
      WT_IS_METADATA(op->btree->dhandle))
        return;

    WT_ASSERT(session, op->type == WT_TXN_OP_BASIC_COL || op->type == WT_TXN_OP_INMEM_COL);

    /*
     * Copy the recno into the transaction operation structure, so when update is evicted to the
     * history store, we have a chance of finding it again. Even though only prepared updates can be
     * evicted, at this stage we don't know whether this transaction will be prepared or not, hence
     * we are copying the key for all operations, so that we can use this key to fetch the update in
     * case this transaction is prepared.
     */
    op->u.op_col.recno = recno;
}

/*
 * __txn_op_need_set_key --
 *     Check if we need to copy the key to the most recent transaction operation.
 */
static WT_INLINE bool
__txn_op_need_set_key(WT_TXN *txn, WT_TXN_OP *op)
{
    /*
     * We save the key for resolving the prepared updates. However, if we have already set the
     * commit timestamp, the transaction cannot be prepared. Therefore, no need to save the key.
     */
    if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
        return (false);

    /* History store writes cannot be prepared. */
    if (WT_IS_HS(op->btree->dhandle))
        return (false);

    /* Metadata writes cannot be prepared. */
    if (WT_IS_METADATA(op->btree->dhandle))
        return (false);

    /* Auto transactions cannot be prepared. */
    if (F_ISSET(txn, WT_TXN_AUTOCOMMIT))
        return (false);

    return (true);
}

/*
 * __wt_txn_op_set_key --
 *     Copy the given key onto the most recent transaction operation. This function early exits if
 *     the transaction cannot prepare.
 */
static WT_INLINE int
__wt_txn_op_set_key(WT_SESSION_IMPL *session, const WT_ITEM *key)
{
    WT_TXN *txn;
    WT_TXN_OP *op;

    txn = session->txn;

    WT_ASSERT(session, txn->mod_count > 0 && key->data != NULL);

    op = txn->mod + txn->mod_count - 1;

    if (!__txn_op_need_set_key(txn, op))
        return (0);

    WT_ASSERT(session, op->type == WT_TXN_OP_BASIC_ROW || op->type == WT_TXN_OP_INMEM_ROW);

    /*
     * Copy the key into the transaction operation structure, so when update is evicted to the
     * history store, we have a chance of finding it again. Even though only prepared updates can be
     * evicted, at this stage we don't know whether this transaction will be prepared or not, hence
     * we are copying the key for all operations, so that we can use this key to fetch the update in
     * case this transaction is prepared.
     */
    return (__wt_buf_set(session, &op->u.op_row.key, key->data, key->size));
}

/*
 * __txn_apply_prepare_state_update --
 *     Change the prepared state of an update.
 */
static WT_INLINE void
__txn_apply_prepare_state_update(WT_SESSION_IMPL *session, WT_UPDATE *upd, bool commit)
{
    WT_TXN *txn;

    txn = session->txn;

    if (commit) {
        /*
         * In case of a prepared transaction, the order of modification of the prepare timestamp to
         * commit timestamp in the update chain will not affect the data visibility, a reader will
         * encounter a prepared update resulting in prepare conflict.
         *
         * As updating timestamp might not be an atomic operation, we will manage using state.
         *
         * TODO: we can remove the prepare locked state once we separate the prepared timestamp and
         * commit timestamp.
         */
        upd->prepare_state = WT_PREPARE_LOCKED;
        WT_RELEASE_BARRIER();
        upd->upd_start_ts = txn->commit_timestamp;
        upd->upd_durable_ts = txn->durable_timestamp;
        WT_RELEASE_WRITE(upd->prepare_state, WT_PREPARE_RESOLVED);
    } else {
        /* Set prepare timestamp and id. */
        upd->upd_start_ts = txn->prepare_timestamp;
        upd->prepare_ts = txn->prepare_timestamp;
        upd->prepared_id = txn->prepared_id;

        /*
         * By default durable timestamp is assigned with 0 which is same as WT_TS_NONE. Assign it
         * with WT_TS_NONE to make sure in case if we change the macro value it shouldn't be a
         * problem.
         */
        upd->upd_durable_ts = WT_TS_NONE;
        WT_RELEASE_WRITE(upd->prepare_state, WT_PREPARE_INPROGRESS);
    }
}

/*
 * __txn_apply_prepare_state_page_del --
 *     Change a prepared page deleted structure's prepared state.
 */
static WT_INLINE void
__txn_apply_prepare_state_page_del(WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del, bool commit)
{
    WT_TXN *txn;

    txn = session->txn;
    if (commit) {
        /*
         * The page deleted structure is only checked in tree walk. If it is prepared, we will
         * instantiate the leaf page and check the keys on it. Therefore, we don't need to worry
         * about reading the partial state and don't need to lock the state.
         */
        page_del->pg_del_start_ts = txn->commit_timestamp;
        page_del->pg_del_durable_ts = txn->durable_timestamp;
        WT_RELEASE_WRITE(page_del->prepare_state, WT_PREPARE_RESOLVED);
    } else {
        /* Set prepare timestamp. */
        page_del->pg_del_start_ts = txn->prepare_timestamp;
        page_del->prepare_ts = txn->prepare_timestamp;
        page_del->prepared_id = txn->prepared_id;
        /*
         * By default durable timestamp is assigned with 0 which is same as WT_TS_NONE. Assign it
         * with WT_TS_NONE to make sure in case if we change the macro value it shouldn't be a
         * problem.
         */
        page_del->pg_del_durable_ts = WT_TS_NONE;
        WT_RELEASE_WRITE(page_del->prepare_state, WT_PREPARE_INPROGRESS);
    }
}

/*
 * __txn_next_op --
 *     Mark a WT_UPDATE object modified by the current transaction.
 */
static WT_INLINE int
__txn_next_op(WT_SESSION_IMPL *session, WT_TXN_OP **opp)
{
    WT_BTREE *btree;
    WT_TXN *txn;
    WT_TXN_OP *op;
    uint64_t btree_txn_id_prev, txn_id;

    *opp = NULL;

    txn = session->txn;

    /*
     * We're about to perform an update. Make sure we have allocated a transaction ID.
     */
    WT_RET(__wt_txn_id_check(session));
    WT_ASSERT(session, F_ISSET(txn, WT_TXN_HAS_ID));

    WT_RET(__wt_realloc_def(session, &txn->mod_alloc, txn->mod_count + 1, &txn->mod));

    op = &txn->mod[txn->mod_count++];
    WT_CLEAR(*op);
    btree = S2BT(session);
    op->btree = btree;

    /*
     * Store the ID of the latest transaction that is making an update. It can be used to determine
     * if there is an active transaction on the btree. Only try to update the shared value if this
     * transaction is newer than the last transaction that updated it.
     */
    btree_txn_id_prev = btree->max_upd_txn;
    txn_id = txn->id;
    WT_ASSERT_ALWAYS(session, txn_id != WT_TXN_ABORTED,
      "Assert failure: session: %s: txn->id == WT_TXN_ABORTED", session->name);
    while (btree_txn_id_prev < txn_id) {
        if (__wt_atomic_cas64(&op->btree->max_upd_txn, btree_txn_id_prev, txn_id))
            break;
        btree_txn_id_prev = op->btree->max_upd_txn;
    }

    (void)__wt_atomic_addi32(&session->dhandle->session_inuse, 1);
    *opp = op;
    return (0);
}

/*
 * __wt_pending_prepared_next_op --
 *     Get the next transaction operation slot for a pending prepared transaction.
 */
static WT_INLINE int
__wt_pending_prepared_next_op(
  WT_SESSION_IMPL *session, WT_TXN_OP **opp, WT_PENDING_PREPARED_ITEM *prepared_item, WT_ITEM *key)
{
    WT_BTREE *btree;
    WT_TXN_OP *op;

    *opp = NULL;

    WT_RET(__wt_realloc_def(
      session, &prepared_item->mod_alloc, prepared_item->mod_count + 1, &prepared_item->mod));

    op = &prepared_item->mod[prepared_item->mod_count++];
    WT_CLEAR(*op);
    btree = S2BT(session);
    op->btree = btree;

    /*
     * Increment the session use count for the data handle. This counter always increases in
     * __txn_next_op decreased in __wt_txn_op_free so we need to match that here.
     */
    (void)__wt_atomic_addi32(&session->dhandle->session_inuse, 1);

    /*
     * Copy the key into the transaction operation structure, so when update is evicted to the
     * history store, we can still find it.
     */
    WT_RET(__wt_buf_set(session, &op->u.op_row.key, key->data, key->size));
    *opp = op;
    return (0);
}

/*
 * __txn_swap_snapshot --
 *     Swap the snapshot pointers.
 */
static WT_INLINE void
__txn_swap_snapshot(uint64_t **snap_a, uint64_t **snap_b)
{
    uint64_t *temp;

    temp = *snap_a;
    *snap_a = *snap_b;
    *snap_b = temp;
}

/*
 * __wt_txn_unmodify --
 *     If threads race making updates, they may discard the last referenced WT_UPDATE item while the
 *     transaction is still active. This function removes the last update item from the "log".
 */
static WT_INLINE void
__wt_txn_unmodify(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_OP *op;

    txn = session->txn;
    if (F_ISSET(txn, WT_TXN_HAS_ID)) {
        WT_ASSERT(session, txn->mod_count > 0);
        --txn->mod_count;
        op = txn->mod + txn->mod_count;
        __wt_txn_op_free(session, op);
    }
}

/*
 * __wt_txn_op_delete_apply_prepare_state --
 *     Apply the correct prepare state and the timestamp to the ref and to any updates in the page
 *     del update list.
 */
static WT_INLINE void
__wt_txn_op_delete_apply_prepare_state(WT_SESSION_IMPL *session, WT_TXN_OP *op, bool commit)
{
    WT_PAGE_DELETED *page_del;
    WT_REF_STATE previous_state;
    WT_UPDATE **updp;
    WT_REF *ref = op->u.ref;

    /* Lock the ref to ensure we don't race with page instantiation. */
    WT_REF_LOCK(session, ref, &previous_state);

    /*
     * Timestamps and prepare state are in the page deleted structure for truncates, or in the
     * updates list in the case of instantiated pages. We also need to update any page deleted
     * structure in the ref.
     *
     * Only two cases are possible. First: the state is WT_REF_DELETED. In this case page_del cannot
     * be NULL yet because an uncommitted operation cannot have reached global visibility. (Or at
     * least, global visibility in the sense we need to use it for truncations, in which prepared
     * and uncommitted transactions are not visible.)
     *
     * Otherwise: there is an uncommitted delete operation we're handling, so the page must have
     * been deleted at some point, and the tree can't be readonly. Therefore the page must have been
     * instantiated, the state must be WT_REF_MEM, and there should be an update list in
     * mod->inst_updates. (But just in case, allow the update list to be null.) There might be a
     * non-null page_del structure to update, depending on whether the page has been reconciled
     * since it was deleted and then instantiated.
     */
    if (previous_state != WT_REF_DELETED) {
        WT_ASSERT(session, previous_state == WT_REF_MEM);
        WT_ASSERT(session, ref->page != NULL && ref->page->modify != NULL);
        if ((updp = ref->page->modify->inst_updates) != NULL)
            for (; *updp != NULL; ++updp)
                __txn_apply_prepare_state_update(session, *updp, commit);
    }

    if ((page_del = ref->page_del) != NULL)
        __txn_apply_prepare_state_page_del(session, page_del, commit);

    if (WT_DELTA_INT_ENABLED(op->btree, S2C(session)))
        __wt_atomic_addv8(&ref->ref_changes, 1);

    WT_REF_UNLOCK(ref, previous_state);
}

/*
 * __txn_op_delete_commit_apply_page_del_timestamp --
 *     Apply the correct start and durable timestamps to the page delete structure.
 */
static WT_INLINE void
__txn_op_delete_commit_apply_page_del_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    WT_PAGE_DELETED *page_del;
    WT_TXN *txn;

    txn = session->txn;
    page_del = op->u.ref->page_del;

    if (page_del != NULL && page_del->pg_del_start_ts == WT_TS_NONE) {
        page_del->pg_del_start_ts = txn->commit_timestamp;
        page_del->pg_del_durable_ts = txn->durable_timestamp;
    }

    return;
}

/*
 * __wt_txn_op_delete_commit --
 *     Apply the correct start and durable timestamps to any updates in the page del update list.
 */
static WT_INLINE int
__wt_txn_op_delete_commit(
  WT_SESSION_IMPL *session, WT_TXN_OP *op, bool validate, bool assign_timestamp)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;
    WT_PAGE_DELETED *page_del;
    WT_REF *ref;
    WT_REF_STATE previous_state;
    WT_TXN *txn;
    WT_UPDATE **updp;
    bool addr_found;

    ref = op->u.ref;
    txn = session->txn;
    page_del = ref->page_del;

    /* Timestamps are ignored on logged files. */
    if (F_ISSET(op->btree, WT_BTREE_LOGGED))
        return (false);

    /*
     * Disable timestamp validation for transactions that are explicitly configured without a
     * timestamp.
     */
    if (F_ISSET(txn, WT_TXN_TS_NOT_SET))
        return (false);

    /* Lock the ref to ensure we don't race with page instantiation. */
    WT_REF_LOCK(session, ref, &previous_state);

    /*
     * Timestamps are in the page deleted structure for truncates, or in the updates in the case of
     * instantiated pages. We also need to update any page deleted structure in the ref. Both commit
     * and durable timestamps need to be updated.
     *
     * Only two cases are possible. First: the state is WT_REF_DELETED. In this case page_del cannot
     * be NULL yet because an uncommitted operation cannot have reached global visibility. (Or at
     * least, global visibility in the sense we need to use it for truncations, in which prepared
     * and uncommitted transactions are not visible.)
     *
     * Otherwise: there is an uncommitted delete operation we're handling, so the page must have
     * been deleted at some point, and the tree can't be readonly. Therefore the page must have been
     * instantiated, the state must be WT_REF_MEM, and there should be an update list in
     * mod->inst_updates. (But just in case, allow the update list to be null.) There might be a
     * non-null page_del structure to update, depending on whether the page has been reconciled
     * since it was deleted and then instantiated.
     */
    if (previous_state != WT_REF_DELETED) {
        WT_ASSERT(session, previous_state == WT_REF_MEM);
        WT_ASSERT(session, ref->page != NULL && ref->page->modify != NULL);
        if ((updp = ref->page->modify->inst_updates) != NULL) {
            /*
             * If we have already set the timestamp, no need to set the timestamp again. We have
             * either set the timestamp on all the updates, or we have set the timestamp on none of
             * the updates.
             */
            if (*updp != NULL) {
                do {
                    if (validate)
                        WT_ERR(__wt_txn_timestamp_usage_check(session, op,
                          (*updp)->upd_start_ts != WT_TS_NONE ? (*updp)->upd_start_ts :
                                                                txn->commit_timestamp,
                          (*updp)->prev_durable_ts));

                    if (assign_timestamp && (*updp)->upd_start_ts == WT_TS_NONE) {
                        (*updp)->upd_start_ts = txn->commit_timestamp;
                        (*updp)->upd_durable_ts = txn->durable_timestamp;
                    }
                    ++updp;
                } while (*updp != NULL);
            }
        }
    } else if (validate) {
        /*
         * Validate the commit timestamp against the page's maximum durable timestamp. While the ref
         * state is WT_REF_DELETED and locked, there are no concurrent threads that can free
         * ref->addr. However, we still need to be within the WT_GEN_SPLIT generation while
         * accessing ref->addr, as required by the calling function.
         */
        WT_ENTER_GENERATION(session, WT_GEN_SPLIT);
        WT_WITH_BTREE(session, op->btree, addr_found = __wt_ref_addr_copy(session, ref, &addr));
        if (addr_found)
            ret = __wt_txn_timestamp_usage_check(session, op,
              page_del->pg_del_start_ts != WT_TS_NONE ? page_del->pg_del_start_ts :
                                                        txn->commit_timestamp,
              WT_MAX(addr.ta.newest_start_durable_ts, addr.ta.newest_stop_durable_ts));
        WT_LEAVE_GENERATION(session, WT_GEN_SPLIT);
        WT_ERR(ret);
    }

    if (assign_timestamp)
        __txn_op_delete_commit_apply_page_del_timestamp(session, op);

    if (WT_DELTA_INT_ENABLED(op->btree, S2C(session)))
        __wt_atomic_addv8(&ref->ref_changes, 1);

err:
    WT_REF_UNLOCK(ref, previous_state);
    return (ret);
}

/*
 * __txn_should_assign_timestamp --
 *     We don't apply timestamps to updates in some cases, for example, if they were made by a
 *     transaction that doesn't assign a commit timestamp or they are updates on tables with
 *     write-ahead-logging enabled. It is important for correctness reasons not to assign any
 *     timestamps to an update that should not have them, so make the check explicit in this
 *     function.
 */
static WT_INLINE bool
__txn_should_assign_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    if (!F_ISSET(session->txn, WT_TXN_HAS_TS_COMMIT))
        return (false);
    if (F_ISSET(op->btree, WT_BTREE_LOGGED))
        return (false);

    return (true);
}

/*
 * __wt_txn_timestamp_usage_check --
 *     Check if a commit will violate timestamp rules.
 */
static WT_INLINE int
__wt_txn_timestamp_usage_check(
  WT_SESSION_IMPL *session, WT_TXN_OP *op, wt_timestamp_t op_ts, wt_timestamp_t prev_op_durable_ts)
{
    WT_BTREE *btree;
    WT_TXN *txn;
    uint16_t flags;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    const char *name;
    bool no_ts_ok, txn_has_ts;

    btree = op->btree;
    txn = session->txn;
    flags = btree->dhandle->ts_flags;
    name = btree->dhandle->name;
    txn_has_ts = F_ISSET(txn, WT_TXN_HAS_TS_COMMIT | WT_TXN_HAS_TS_DURABLE);

    /* Timestamps are ignored on logged files. */
    if (F_ISSET(btree, WT_BTREE_LOGGED))
        return (0);

    /*
     * Do not check for timestamp usage in recovery. We don't expect recovery to be using timestamps
     * when applying commits, and it is possible that timestamps may be out-of-order in log replay.
     */
    if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
        return (0);

    /* Check for disallowed timestamps. */
    if (LF_ISSET(WT_DHANDLE_TS_NEVER)) {
        if (!txn_has_ts)
            return (0);

        __wt_err(session, EINVAL,
          "%s: " WT_TS_VERBOSE_PREFIX "timestamp %s set when disallowed by table configuration",
          name, __wt_timestamp_to_string(op_ts, ts_string[0]));
        WT_IGNORE_RET(__wt_verbose_dump_txn_one(session, session, EINVAL, NULL));
#ifdef HAVE_DIAGNOSTIC
        __wt_abort(session);
#endif
        return (EINVAL);
    }

    /*
     * Ordered consistency requires all updates use timestamps, once they are first used, but this
     * test can be turned off on a per-transaction basis.
     */
    no_ts_ok = F_ISSET(txn, WT_TXN_TS_NOT_SET);
    if (!txn_has_ts && prev_op_durable_ts != WT_TS_NONE && !no_ts_ok) {
        __wt_err(session, EINVAL,
          "%s: " WT_TS_VERBOSE_PREFIX
          "no timestamp provided for an update to a table configured to always use timestamps "
          "once they are first used",
          name);
        WT_IGNORE_RET(__wt_verbose_dump_txn_one(session, session, EINVAL, NULL));
#ifdef HAVE_DIAGNOSTIC
        __wt_abort(session);
#endif
        return (EINVAL);
    }

    /* Ordered consistency requires all updates be in timestamp order. */
    if (txn_has_ts && prev_op_durable_ts > op_ts) {
        __wt_err(session, EINVAL,
          "%s: " WT_TS_VERBOSE_PREFIX
          "updating a value with a timestamp %s before the previous update %s",
          name, __wt_timestamp_to_string(op_ts, ts_string[0]),
          __wt_timestamp_to_string(prev_op_durable_ts, ts_string[1]));
        WT_IGNORE_RET(__wt_verbose_dump_txn_one(session, session, EINVAL, NULL));
#ifdef HAVE_DIAGNOSTIC
        __wt_abort(session);
#endif
        return (EINVAL);
    }

    return (0);
}

/*
 * __wt_txn_op_set_timestamp --
 *     Decide whether to copy a commit timestamp into an update. If the op structure doesn't have a
 *     populated update or ref field or is in prepared state there won't be any check for an
 *     existing timestamp.
 */
static WT_INLINE int
__wt_txn_op_set_timestamp(WT_SESSION_IMPL *session, WT_TXN_OP *op, bool validate)
{
    WT_TXN *txn;
    WT_UPDATE *upd;

    txn = session->txn;

    if (!__txn_should_assign_timestamp(session, op)) {
        if (validate) {
            if (op->type == WT_TXN_OP_REF_DELETE)
                WT_RET(__wt_txn_op_delete_commit(session, op, validate, false));
            else
                WT_RET(__wt_txn_timestamp_usage_check(
                  session, op, txn->commit_timestamp, op->u.op_upd->prev_durable_ts));
        }
        return (0);
    }

    if (F_ISSET(txn, WT_TXN_PREPARE)) {
        /*
         * We have a commit timestamp for a prepare transaction, this is only possible as part of a
         * transaction commit call.
         */
        if (op->type == WT_TXN_OP_REF_DELETE)
            __wt_txn_op_delete_apply_prepare_state(session, op, true);
        else {
            upd = op->u.op_upd;

            /* Resolve prepared update to be committed update. */
            __txn_apply_prepare_state_update(session, upd, true);
        }
    } else {
        if (op->type == WT_TXN_OP_REF_DELETE)
            WT_RET(__wt_txn_op_delete_commit(session, op, validate, true));
        else {
            /*
             * The timestamp is in the update for operations other than truncate. Both commit and
             * durable timestamps need to be updated.
             */
            upd = op->u.op_upd;
            if (validate)
                WT_RET(__wt_txn_timestamp_usage_check(session, op,
                  upd->upd_start_ts != WT_TS_NONE ? upd->upd_start_ts : txn->commit_timestamp,
                  upd->prev_durable_ts));
            if (upd->upd_start_ts == WT_TS_NONE) {
                upd->upd_start_ts = txn->commit_timestamp;
                upd->upd_durable_ts = txn->durable_timestamp;
            }
        }
    }

    return (0);
}

/*
 * __wt_txn_modify --
 *     Mark a WT_UPDATE object modified by the current transaction.
 */
static WT_INLINE int
__wt_txn_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_OP *op;

    txn = session->txn;

    if (F_ISSET(txn, WT_TXN_READONLY)) {
        if (F_ISSET(txn, WT_TXN_IGNORE_PREPARE))
            WT_RET_MSG(
              session, ENOTSUP, "Transactions with ignore_prepare=true cannot perform updates");
        WT_RET_MSG(session, WT_ROLLBACK, "Attempt to update in a read-only transaction");
    }

    WT_RET(__txn_next_op(session, &op));

    upd->txnid = session->txn->id;
    ret = __wt_op_modify(session, upd, op);
    if (ret != 0)
        __wt_txn_unmodify(session);

    return (ret);
}

/*
 * __wt_op_modify --
 *     Initialize a transaction operation for a prepared update.
 */
static WT_INLINE int
__wt_op_modify(WT_SESSION_IMPL *session, WT_UPDATE *upd, WT_TXN_OP *op)
{
    if (F_ISSET(session, WT_SESSION_LOGGING_INMEM)) {
        if (op->btree->type == BTREE_ROW)
            op->type = WT_TXN_OP_INMEM_ROW;
        else
            op->type = WT_TXN_OP_INMEM_COL;
    } else {
        if (op->btree->type == BTREE_ROW)
            op->type = WT_TXN_OP_BASIC_ROW;
        else
            op->type = WT_TXN_OP_BASIC_COL;
    }
    op->u.op_upd = upd;

    /* History store bypasses transactions, transaction modify should never be called on it. */
    WT_ASSERT(session, !WT_IS_HS((S2BT(session))->dhandle));

    return (__wt_txn_op_set_timestamp(session, op, false));
}

/*
 * __wt_txn_modify_page_delete --
 *     Remember a page truncated by the current transaction.
 */
static WT_INLINE int
__wt_txn_modify_page_delete(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;
    WT_TXN *txn;
    WT_TXN_OP *op;

    txn = session->txn;

    WT_RET(__txn_next_op(session, &op));
    op->type = WT_TXN_OP_REF_DELETE;
    op->u.ref = ref;

    /*
     * This access to the WT_PAGE_DELETED structure is safe; caller has the WT_REF locked, and in
     * fact just allocated the structure to fill in.
     */
    ref->page_del->txnid = txn->id;

    if (__txn_should_assign_timestamp(session, op))
        __txn_op_delete_commit_apply_page_del_timestamp(session, op);

    if (__wt_txn_log_op_check(session))
        WT_ERR(__wt_txn_log_op(session, NULL));
    return (0);

err:
    __wt_txn_unmodify(session);
    return (ret);
}

/*
 * __wt_txn_oldest_id --
 *     Return the oldest transaction ID that has to be kept for the current tree.
 */
static WT_INLINE uint64_t
__wt_txn_oldest_id(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    uint64_t checkpoint_pinned, oldest_id, recovery_ckpt_snap_min;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    /*
     * The metadata is tracked specially because of optimizations for checkpoints.
     */
    if (session->dhandle != NULL && WT_IS_METADATA(session->dhandle))
        return (__wt_atomic_loadv64(&txn_global->metadata_pinned));

    /*
     * Take a local copy of these IDs in case they are updated while we are checking visibility. The
     * read of the transaction ID pinned by a checkpoint needs to be carefully ordered: if a
     * checkpoint is starting and we have to start checking the pinned ID, we take the minimum of it
     * with the oldest ID, which is what we want. The logged tables are excluded as part of RTS, so
     * there is no need of holding their oldest_id
     */
    WT_ACQUIRE_READ_WITH_BARRIER(oldest_id, txn_global->oldest_id);

    if (!F_ISSET(conn, WT_CONN_RECOVERING) || session->dhandle == NULL ||
      F_ISSET(S2BT(session), WT_BTREE_LOGGED)) {
        /*
         * Checkpoint transactions often fall behind ordinary application threads. If there is an
         * active checkpoint, keep changes until checkpoint is finished.
         */
        checkpoint_pinned = __wt_atomic_loadv64(&txn_global->checkpoint_txn_shared.pinned_id);
        if (checkpoint_pinned == WT_TXN_NONE || oldest_id < checkpoint_pinned)
            return (oldest_id);
        return (checkpoint_pinned);
    } else {
        /*
         * Recovered checkpoint snapshot rarely fall behind ordinary application threads. Keep the
         * changes until the recovery is finished.
         */
        recovery_ckpt_snap_min = conn->recovery_ckpt_snap_min;
        if (recovery_ckpt_snap_min == WT_TXN_NONE || oldest_id < recovery_ckpt_snap_min)
            return (oldest_id);
        return (recovery_ckpt_snap_min);
    }
}

/*
 * __wt_txn_pinned_stable_timestamp --
 *     Get the first timestamp that can be written to the disk for precise checkpoint.
 */
static WT_INLINE void
__wt_txn_pinned_stable_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *pinned_stable_tsp)
{
    WT_CONNECTION_IMPL *conn;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t checkpoint_ts, pinned_stable_ts;
    bool has_stable_timestamp;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    /*
     * There is no need to go further if no stable timestamp has been set yet.
     */
    WT_ACQUIRE_READ(has_stable_timestamp, txn_global->has_stable_timestamp);
    if (!has_stable_timestamp) {
        *pinned_stable_tsp = WT_TS_NONE;
        return;
    }

    /*
     * It is important to ensure we only read the global stable timestamp once. Otherwise, we may
     * return a stable timestamp that is larger than the checkpoint timestamp. For example, the
     * first time we read the global stable timestamp as 100 and set it to the local variable
     * disaggregated_stable_ts. If the checkpoint timestamp is 110 and the second time we read the
     * global stable timestamp as 120, we will return 120 instead of the checkpoint timestamp 110.
     */
    WT_ACQUIRE_READ(pinned_stable_ts, txn_global->stable_timestamp);

    if (!F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT)) {
        *pinned_stable_tsp = pinned_stable_ts;
        return;
    }

    /*
     * The read of checkpoint timestamp needs to be carefully ordered: it needs to be after we have
     * read the stable timestamp, otherwise, we may read earlier checkpoint timestamp resulting more
     * data being pinned. If a checkpoint is starting and we have to use the checkpoint timestamp,
     * we take the minimum of it with the stable timestamp, which is what we want.
     */
    checkpoint_ts = txn_global->checkpoint_timestamp;

    if (checkpoint_ts != 0 && checkpoint_ts < pinned_stable_ts)
        *pinned_stable_tsp = checkpoint_ts;
    else
        *pinned_stable_tsp = pinned_stable_ts;
}

/*
 * __wt_txn_pinned_timestamp --
 *     Get the first timestamp that has to be kept for the current tree.
 */
static WT_INLINE void
__wt_txn_pinned_timestamp(WT_SESSION_IMPL *session, wt_timestamp_t *pinned_tsp)
{
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t checkpoint_ts, pinned_ts;
    bool has_pinned_timestamp;

    txn_global = &S2C(session)->txn_global;

    /*
     * There is no need to go further if no pinned timestamp has been set yet.
     */
    WT_ACQUIRE_READ(has_pinned_timestamp, txn_global->has_pinned_timestamp);
    if (!has_pinned_timestamp) {
        *pinned_tsp = WT_TS_NONE;
        return;
    }

    /* If we have a version cursor open, use the pinned timestamp when it is opened. */
    if (S2C(session)->version_cursor_count > 0) {
        *pinned_tsp = txn_global->version_cursor_pinned_timestamp;
        return;
    }

    /*
     * It is important to ensure we only read the global pinned timestamp once. Otherwise, we may
     * return a pinned timestamp that is larger than the checkpoint timestamp. For example, the
     * first time we read the global pinned timestamp as 100 and set it to the local variable
     * pinned_ts. If the checkpoint timestamp is 110 and the second time we read the global pinned
     * timestamp as 120, we will return 120 instead of the checkpoint timestamp 110.
     */
    WT_ACQUIRE_READ(pinned_ts, txn_global->pinned_timestamp);

    /*
     * The read of checkpoint timestamp needs to be carefully ordered: it needs to be after we have
     * read the pinned timestamp, otherwise, we may read earlier checkpoint timestamp resulting more
     * data being pinned. If a checkpoint is starting and we have to use the checkpoint timestamp,
     * we take the minimum of it with the oldest timestamp, which is what we want.
     */
    checkpoint_ts = txn_global->checkpoint_timestamp;

    if (checkpoint_ts != WT_TS_NONE && checkpoint_ts < pinned_ts)
        *pinned_tsp = checkpoint_ts;
    else
        *pinned_tsp = pinned_ts;
}

/*
 * __txn_visible_all_id --
 *     Check if a given transaction ID is "globally visible". This is, if all sessions in the system
 *     will see the transaction ID including the ID that belongs to a running checkpoint.
 */
static WT_INLINE bool
__txn_visible_all_id(WT_SESSION_IMPL *session, uint64_t id)
{
    WT_TXN *txn;
    uint64_t oldest_id;

    txn = session->txn;

    /* Make sure that checkpoint cursor transactions only read checkpoints, except for metadata. */
    WT_ASSERT(session,
      (session->dhandle != NULL && WT_IS_METADATA(session->dhandle)) ||
        WT_READING_CHECKPOINT(session) == F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT));

    /*
     * When reading from a checkpoint, all readers use the same snapshot, so a transaction is
     * globally visible if it is visible in that snapshot. Note that this can cause things that were
     * not globally visible yet when the checkpoint is taken to become globally visible in the
     * checkpoint. This is expected (it is like all the old running transactions exited) -- but note
     * that it's important that the inverse change (something globally visible when the checkpoint
     * was taken becomes not globally visible in the checkpoint) never happen as this violates basic
     * assumptions about visibility. (And, concretely, it can cause stale history store entries to
     * come back to life and produce wrong answers.)
     *
     * Note: we use the transaction to check this rather than testing WT_READING_CHECKPOINT because
     * reading the metadata while working with a checkpoint cursor will borrow the transaction; it
     * then ends up using it to read a non-checkpoint tree. This is believed to be ok because the
     * metadata is always read-uncommitted, but we want to still use the checkpoint-cursor
     * visibility logic. Using the regular visibility logic with a checkpoint cursor transaction can
     * be logically invalid (it is possible that way for something to be globally visible but
     * specifically invisible) and also can end up comparing transaction ids from different database
     * opens.
     */
    if (F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT))
        return (
          __wt_txn_visible_id_snapshot(id, txn->snapshot_data.snap_min, txn->snapshot_data.snap_max,
            txn->snapshot_data.snapshot, txn->snapshot_data.snapshot_count));
    oldest_id = __wt_txn_oldest_id(session);

    return (id < oldest_id);
}

/*
 * __wt_txn_timestamp_visible_all --
 *     Check whether a given timestamp is either globally visible or obsolete.
 */
static WT_INLINE bool
__wt_txn_timestamp_visible_all(WT_SESSION_IMPL *session, wt_timestamp_t timestamp)
{
    wt_timestamp_t pinned_ts;

    /* Compare the given timestamp to the pinned timestamp, if it exists. */
    __wt_txn_pinned_timestamp(session, &pinned_ts);

    return (pinned_ts != WT_TS_NONE && timestamp <= pinned_ts);
}

/*
 * __wt_txn_visible_all --
 *     Check whether a given time window is either globally visible or obsolete. For global
 *     visibility checks, the commit times are checked against the oldest possible readers in the
 *     system. If all possible readers could always see the time window - it is globally visible.
 *     For obsolete checks callers should generally pass in the durable timestamp, since it is
 *     guaranteed to be newer than or equal to the commit time, and content needs to be retained
 *     (not become obsolete) until both the commit and durable times are obsolete. If the commit
 *     time is used for this check, it's possible that a transaction is committed with a durable
 *     time and made obsolete before it can be included in a checkpoint - which leads to bugs in
 *     checkpoint correctness.
 */
static WT_INLINE bool
__wt_txn_visible_all(WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp)
{
    /*
     * When shutting down, the transactional system has finished running and all we care about is
     * eviction, make everything visible.
     */
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING))
        return (true);

    if (!__txn_visible_all_id(session, id))
        return (false);

    /* Timestamp check. */
    if (timestamp == WT_TS_NONE)
        return (true);

    /* Make sure that checkpoint cursor transactions only read checkpoints, except for metadata. */
    WT_ASSERT(session,
      (session->dhandle != NULL && WT_IS_METADATA(session->dhandle)) ||
        WT_READING_CHECKPOINT(session) == F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT));

    /* When reading a checkpoint, use the checkpoint state instead of the current state. */
    if (F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT))
        return (session->txn->checkpoint_oldest_timestamp != WT_TS_NONE &&
          timestamp <= session->txn->checkpoint_oldest_timestamp);

    return (__wt_txn_timestamp_visible_all(session, timestamp));
}

/*
 * __wt_txn_has_newest_and_visible_all --
 *     Check whether a given time window is either globally visible or obsolete. Note that both the
 *     id and the timestamp have to be greater than 0 to be considered.
 */
static WT_INLINE bool
__wt_txn_has_newest_and_visible_all(WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp)
{
    /* If there is no transaction or timestamp information available, there is nothing to do. */
    if (id == WT_TXN_NONE && timestamp == WT_TS_NONE)
        return (false);

    if (__wt_txn_visible_all(session, id, timestamp))
        return (true);

    return (false);
}

/*
 * __wt_txn_upd_visible_all --
 *     Is the given update visible to all (possible) readers?
 */
static WT_INLINE bool
__wt_txn_upd_visible_all(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
    uint8_t prepare_state;

    WT_ACQUIRE_READ(prepare_state, upd->prepare_state);

    if (prepare_state == WT_PREPARE_LOCKED || prepare_state == WT_PREPARE_INPROGRESS)
        return (false);

    /*
     * This function is used to determine when an update is obsolete: that should take into account
     * the durable timestamp which is greater than or equal to the start timestamp.
     */
    return (__wt_txn_visible_all(session, upd->txnid, upd->upd_durable_ts));
}

/*
 * __wt_txn_upd_value_visible_all --
 *     Is the given update value visible to all (possible) readers?
 */
static WT_INLINE bool
__wt_txn_upd_value_visible_all(WT_SESSION_IMPL *session, WT_UPDATE_VALUE *upd_value)
{
    WT_ASSERT(session, !WT_TIME_WINDOW_HAS_PREPARE(&upd_value->tw));
    return (upd_value->type == WT_UPDATE_TOMBSTONE ?
        __wt_txn_visible_all(session, upd_value->tw.stop_txn, upd_value->tw.durable_stop_ts) :
        __wt_txn_visible_all(session, upd_value->tw.start_txn, upd_value->tw.durable_start_ts));
}

/*
 * __wt_txn_tw_stop_visible --
 *     Is the given stop time window visible?
 */
static WT_INLINE bool
__wt_txn_tw_stop_visible(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    return (WT_TIME_WINDOW_HAS_STOP(tw) && !WT_TIME_WINDOW_HAS_STOP_PREPARE(tw) &&
      __wt_txn_visible(session, tw->stop_txn, tw->stop_ts, tw->durable_stop_ts));
}

/*
 * __wt_txn_tw_start_visible --
 *     Is the given start time window visible?
 */
static WT_INLINE bool
__wt_txn_tw_start_visible(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    if (WT_TIME_WINDOW_HAS_START_PREPARE(tw))
        return (false);
    return (__wt_txn_visible(session, tw->start_txn, tw->start_ts, tw->durable_start_ts));
}

/*
 * __wt_txn_tw_start_visible_all --
 *     Is the given start time window visible to all (possible) readers?
 */
static WT_INLINE bool
__wt_txn_tw_start_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    if (WT_TIME_WINDOW_HAS_START_PREPARE(tw))
        return (false);
    return (__wt_txn_visible_all(session, tw->start_txn, tw->durable_start_ts));
}

/*
 * __wt_txn_tw_stop_visible_all --
 *     Is the given stop time window visible to all (possible) readers?
 */
static WT_INLINE bool
__wt_txn_tw_stop_visible_all(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    return (WT_TIME_WINDOW_HAS_STOP(tw) && !WT_TIME_WINDOW_HAS_STOP_PREPARE(tw) &&
      __wt_txn_visible_all(session, tw->stop_txn, tw->durable_stop_ts));
}

/*
 * __wt_txn_visible_id_snapshot --
 *     Is the id visible in terms of the given snapshot?
 */
static WT_INLINE bool
__wt_txn_visible_id_snapshot(
  uint64_t id, uint64_t snap_min, uint64_t snap_max, uint64_t *snapshot, uint32_t snapshot_count)
{
    bool found;

    /*
     * WT_ISO_SNAPSHOT, WT_ISO_READ_COMMITTED: the ID is visible if it is not the result of a
     * concurrent transaction, that is, if was committed before the snapshot was taken.
     *
     * The order here is important: anything newer than or equal to the maximum ID we saw when
     * taking the snapshot should be invisible, even if the snapshot is empty.
     *
     * Snapshot data:
     *	ids >= snap_max not visible,
     *	ids < snap_min are visible,
     *	everything else is visible unless it is found in the snapshot.
     */
    if (snap_max <= id)
        return (false);
    if (snapshot_count == 0 || id < snap_min)
        return (true);

    WT_BINARY_SEARCH(id, snapshot, snapshot_count, found);
    return (!found);
}

/*
 * __txn_visible_id --
 *     Can the current transaction see the given ID?
 */
static WT_INLINE bool
__txn_visible_id(WT_SESSION_IMPL *session, uint64_t id)
{
    WT_TXN *txn;

    txn = session->txn;

    /* Changes with no associated transaction are always visible. */
    if (id == WT_TXN_NONE)
        return (true);

    /* Nobody sees the results of aborted transactions. */
    if (id == WT_TXN_ABORTED)
        return (false);

    /* Transactions see their own changes. */
    if (id == txn->id)
        return (true);

    /* Read-uncommitted transactions see all other changes. */
    if (txn->isolation == WT_ISO_READ_UNCOMMITTED)
        return (true);

    /* Otherwise, we should be called with a snapshot. */
    WT_ASSERT(session, F_ISSET(txn, WT_TXN_HAS_SNAPSHOT));

    return (__wt_txn_visible_id_snapshot(id, txn->snapshot_data.snap_min,
      txn->snapshot_data.snap_max, txn->snapshot_data.snapshot, txn->snapshot_data.snapshot_count));
}

/*
 * __wt_txn_timestamp_visible --
 *     Can the current transaction see the given timestamp?
 */
static WT_INLINE bool
__wt_txn_timestamp_visible(
  WT_SESSION_IMPL *session, wt_timestamp_t timestamp, wt_timestamp_t durable_timestamp)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    /* Timestamp check. */
    if (!F_ISSET(txn, WT_TXN_SHARED_TS_READ) || timestamp == WT_TS_NONE)
        return (true);

    /*
     * For checkpoint cursors, just using the commit timestamp visibility check can go wrong when a
     * prepared transaction gets committed in parallel to a running checkpoint.
     *
     * To avoid this problem, along with the visibility check of a commit timestamp, comparing the
     * durable timestamp against the stable timestamp of a checkpoint can avoid the problems of
     * returning inconsistent data.
     */
    if (WT_READING_CHECKPOINT(session))
        return ((timestamp <= txn->checkpoint_read_timestamp) &&
          (durable_timestamp <= txn->checkpoint_stable_timestamp));

    return (timestamp <= txn_shared->read_timestamp);
}

/*
 * __wt_txn_snap_min_visible --
 *     Can the current transaction snapshot minimum/read timestamp see the given ID/timestamp? This
 *     visibility check should only be used when assessing broader visibility based on aggregated
 *     time window. It does not reflect whether a specific update is visible to a transaction.
 */
static WT_INLINE bool
__wt_txn_snap_min_visible(
  WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp, wt_timestamp_t durable_timestamp)
{
    WT_ASSERT(session, F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

    /* Transaction snapshot minimum check. */
    if (id >= session->txn->snapshot_data.snap_min)
        return (false);

    /* Transactions read their writes, regardless of timestamps. */
    if (F_ISSET(session->txn, WT_TXN_HAS_ID) && id == session->txn->id)
        return (true);

    /* Timestamp check. */
    return (__wt_txn_timestamp_visible(session, timestamp, durable_timestamp));
}

/*
 * __wt_txn_visible --
 *     Can the current transaction see the given ID/timestamp?
 */
static WT_INLINE bool
__wt_txn_visible(
  WT_SESSION_IMPL *session, uint64_t id, wt_timestamp_t timestamp, wt_timestamp_t durable_timestamp)
{
    if (!__txn_visible_id(session, id))
        return (false);

    /* Transactions read their writes, regardless of timestamps. */
    if (F_ISSET(session->txn, WT_TXN_HAS_ID) && id == session->txn->id)
        return (true);

    /* Timestamp check. */
    return (__wt_txn_timestamp_visible(session, timestamp, durable_timestamp));
}

/*
 * __wt_txn_upd_visible_type --
 *     Visible type of given update for the current transaction.
 */
static WT_INLINE WT_VISIBLE_TYPE
__wt_txn_upd_visible_type(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
    uint8_t prepare_state, new_prepare_state;
    bool upd_visible;

    for (;; __wt_yield()) {
        /* Prepare state change is on going, yield and try again. */
        WT_ACQUIRE_READ(prepare_state, upd->prepare_state);
        if (prepare_state == WT_PREPARE_LOCKED)
            continue;

        /* Entries in the history store are always visible. */
        if ((WT_IS_HS(session->dhandle) && upd->txnid != WT_TXN_ABORTED &&
              upd->type == WT_UPDATE_STANDARD))
            return (WT_VISIBLE_TRUE);

        upd_visible = __wt_txn_visible(session, upd->txnid, upd->upd_start_ts, upd->upd_durable_ts);

        /*
         * The visibility check is only valid if the update does not change state. If the state does
         * change, recheck visibility.
         *
         * We need to use an acquire read to the second read of prepare state as otherwise it could
         * overlap with the reads of the transaction id and start timestamp. Which would invalidate
         * this check.
         */
        WT_ACQUIRE_READ(new_prepare_state, upd->prepare_state);
        if (prepare_state == new_prepare_state)
            break;

        WT_STAT_CONN_INCR(session, prepared_transition_blocked_page);
    }

    if (!upd_visible)
        return (WT_VISIBLE_FALSE);

    if (prepare_state == WT_PREPARE_INPROGRESS)
        return (WT_VISIBLE_PREPARE);

    return (WT_VISIBLE_TRUE);
}

/*
 * __wt_txn_upd_visible --
 *     Can the current transaction see the given update.
 */
static WT_INLINE bool
__wt_txn_upd_visible(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
    return (__wt_txn_upd_visible_type(session, upd) == WT_VISIBLE_TRUE);
}

/*
 * __wt_upd_alloc --
 *     Allocate a WT_UPDATE structure and associated value and fill it in.
 */
static WT_INLINE int
__wt_upd_alloc(WT_SESSION_IMPL *session, const WT_ITEM *value, u_int modify_type, WT_UPDATE **updp,
  size_t *sizep)
{
    WT_UPDATE *upd;
    size_t allocsz; /* Allocation size in bytes. */

    *updp = NULL;

    /*
     * The code paths leading here are convoluted: assert we never attempt to allocate an update
     * structure if only intending to insert one we already have, or pass in a value with a type
     * that doesn't support values.
     */
    WT_ASSERT(session, modify_type != WT_UPDATE_INVALID);
    WT_ASSERT(session,
      (value == NULL && (modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE)) ||
        (value != NULL &&
          !(modify_type == WT_UPDATE_RESERVE || modify_type == WT_UPDATE_TOMBSTONE)));

    if (value == NULL || value->size == 0)
        allocsz = WT_UPDATE_SIZE_NOVALUE;
    else
        allocsz = WT_UPDATE_SIZE + value->size;

    /*
     * Allocate the WT_UPDATE structure and room for the value, then copy any value into place.
     * Memory is cleared, which is the equivalent of setting:
     *    WT_UPDATE.txnid = WT_TXN_NONE;
     *    WT_UPDATE.durable_ts = WT_TS_NONE;
     *    WT_UPDATE.start_ts = WT_TS_NONE;
     *    WT_UPDATE.prepare_state = WT_PREPARE_INIT;
     *    WT_UPDATE.flags = 0;
     */
    WT_RET(__wt_calloc(session, 1, allocsz, &upd));
    if (value != NULL && value->size != 0) {
        upd->size = WT_STORE_SIZE(value->size);
        memcpy(upd->data, value->data, value->size);
    }
    upd->type = (uint8_t)modify_type;

    *updp = upd;
    if (sizep != NULL)
        *sizep = WT_UPDATE_MEMSIZE(upd);
    return (0);
}

/*
 * __wt_upd_alloc_tombstone --
 *     Allocate a tombstone update.
 */
static WT_INLINE int
__wt_upd_alloc_tombstone(WT_SESSION_IMPL *session, WT_UPDATE **updp, size_t *sizep)
{
    return (__wt_upd_alloc(session, NULL, WT_UPDATE_TOMBSTONE, updp, sizep));
}

/*
 * __wt_txn_read_upd_list_internal --
 *     Internal helper function to get the first visible update in a list (or NULL if none are
 *     visible).
 */
static WT_INLINE int
__wt_txn_read_upd_list_internal(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key,
  uint64_t recno, WT_UPDATE *upd, WT_UPDATE **prepare_updp, WT_UPDATE **restored_updp)
{
    WT_VISIBLE_TYPE upd_visible;
    uint64_t prepare_txnid;
    uint8_t prepare_state;

    prepare_txnid = WT_TXN_NONE;

    if (prepare_updp != NULL)
        *prepare_updp = NULL;
    if (restored_updp != NULL)
        *restored_updp = NULL;
    __wt_upd_value_clear(cbt->upd_value);

    for (; upd != NULL; upd = upd->next) {
        /* Skip reserved place-holders, they're never visible. */
        if (upd->type == WT_UPDATE_RESERVE)
            continue;

        WT_ACQUIRE_READ(prepare_state, upd->prepare_state);

        /*
         * We previously found a prepared update, check if the update has the same transaction id,
         * if it does it must not be visible as it is part of the same transaction as the previous
         * prepared update.
         */
        if (prepare_txnid != WT_TXN_NONE && upd->txnid == prepare_txnid) {
            /*
             * If we see an update with prepare resolved this indicates that the read, which is
             * configured to ignore prepared updates raced with the commit of the same prepared
             * transaction. Increment a stat to track this.
             *
             * This case exists as reconciliation chooses which update to write to disk in a newest
             * to oldest fashion, and if prepared update resolution happens in the same direction
             * some artifacts of a prepared transaction could be written to disk while some remain
             * only in-memory. Instead prepared update resolution is recursively done from oldest to
             * newest. Which mean that our reader would see a prepared update followed by a
             * committed update.
             *
             * There is an alternate solution which would have reconciliation forget the chosen
             * update if it sees a prepared update after it. That would allow the update chain
             * resolution to occur from newest to oldest and this reader edge case would no longer
             * exist. That solution needs further exploration.
             */
            if (prepare_state == WT_PREPARE_RESOLVED)
                WT_STAT_CONN_DSRC_INCR(session, txn_read_race_prepare_commit);
            continue;
        }

        /*
         * If the cursor is configured to ignore tombstones, copy the timestamps from the tombstones
         * to the stop time window of the update value being returned to the caller. Caller can
         * process the stop time window to decide if there was a tombstone on the update chain. If
         * the time window already has a stop time set then we must have seen a tombstone prior to
         * ours in the update list, and therefore don't need to do this again.
         */
        if (upd->type == WT_UPDATE_TOMBSTONE && F_ISSET(&cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE) &&
          !WT_TIME_WINDOW_HAS_STOP(&cbt->upd_value->tw)) {
            WT_TIME_WINDOW_SET_STOP(&cbt->upd_value->tw, upd,
              prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);
            continue;
        }

        upd_visible = __wt_txn_upd_visible_type(session, upd);

        if (upd_visible == WT_VISIBLE_TRUE)
            break;

        /*
         * Save the prepared update to help us detect if we race with prepared commit or rollback
         * irrespective of update visibility.
         */
        if ((prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) &&
          prepare_updp != NULL && *prepare_updp == NULL &&
          F_ISSET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS))
            *prepare_updp = upd;

        /*
         * Save the restored update to use it as base value update in case if we need to reach
         * history store instead of on-disk value.
         */
        if (upd->txnid != WT_TXN_ABORTED && restored_updp != NULL &&
          F_ISSET(upd, WT_UPDATE_RESTORED_FROM_HS) && upd->type == WT_UPDATE_STANDARD) {
            WT_ASSERT(session, *restored_updp == NULL);
            *restored_updp = upd;
        }

        if (upd_visible == WT_VISIBLE_PREPARE) {
            /* Ignore the prepared update, if transaction configuration says so. */
            if (F_ISSET(session->txn, WT_TXN_IGNORE_PREPARE)) {
                prepare_txnid = upd->txnid;
                continue;
            }

            return (WT_PREPARE_CONFLICT);
        }

        if (F_ISSET(upd, WT_UPDATE_RESTORED_FROM_DELTA) && upd->type == WT_UPDATE_STANDARD) {
            WT_ASSERT(session, !F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY));
            /*
             * If we see an update that is not visible to the reader and it is restored from delta,
             * we should search the history store.
             */
            if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_HS_OPEN) &&
              !F_ISSET(session->dhandle,
                WT_DHANDLE_HS | WT_DHANDLE_IS_METADATA | WT_DHANDLE_DISAGG_META)) {
                __wt_timing_stress(session, WT_TIMING_STRESS_HS_SEARCH, NULL);
                WT_RET(__wt_hs_find_upd(session, S2BT(session)->id, key, cbt->iface.value_format,
                  recno, cbt->upd_value, &cbt->upd_value->buf));
                if (cbt->upd_value->type == WT_UPDATE_INVALID)
                    return (WT_NOTFOUND);
                return (0);
            }
        }
    }

    if (upd == NULL)
        return (0);

    /*
     * Now assign to the update value. If it's not a modify, we're free to simply point the value at
     * the update's memory without owning it. If it is a modify, we need to reconstruct the full
     * update now and make the value own the buffer.
     *
     * If the caller has specifically asked us to skip assigning the buffer, we shouldn't bother
     * reconstructing the modify.
     */
    if (upd->type != WT_UPDATE_MODIFY || cbt->upd_value->skip_buf)
        __wt_upd_value_assign(cbt->upd_value, upd);
    else
        WT_RET(__wt_modify_reconstruct_from_upd_list(
          session, cbt, upd, cbt->upd_value, WT_OPCTX_TRANSACTION));
    return (0);
}

/*
 * __wt_txn_read_upd_list --
 *     Get the first visible update in a list (or NULL if none are visible).
 */
static WT_INLINE int
__wt_txn_read_upd_list(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key, uint64_t recno, WT_UPDATE *upd)
{
    return (__wt_txn_read_upd_list_internal(session, cbt, key, recno, upd, NULL, NULL));
}

/*
 * __wt_txn_read --
 *     Get the first visible update in a chain. This function will first check the update list
 *     supplied as a function argument. If there is no visible update, it will check the onpage
 *     value for the given key. Finally, if the onpage value is not visible to the reader, the
 *     function will search the history store for a visible update.
 */
static WT_INLINE int
__wt_txn_read(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key, uint64_t recno, WT_UPDATE *upd)
{
    WT_DECL_RET;
    WT_TIME_WINDOW tw;
    WT_UPDATE *prepare_upd, *restored_upd;
    bool have_stop_tw, prepare_retry, read_onpage;

    prepare_upd = restored_upd = NULL;
    read_onpage = prepare_retry = true;

retry:
    WT_RET(
      __wt_txn_read_upd_list_internal(session, cbt, key, recno, upd, &prepare_upd, &restored_upd));
    if (WT_UPDATE_DATA_VALUE(cbt->upd_value) ||
      (cbt->upd_value->type == WT_UPDATE_MODIFY && cbt->upd_value->skip_buf))
        return (0);
    WT_ASSERT(session, cbt->upd_value->type == WT_UPDATE_INVALID);

    /* If there is no ondisk value, there can't be anything in the history store either. */
    if (cbt->ref->page->dsk == NULL) {
        cbt->upd_value->type = WT_UPDATE_TOMBSTONE;
        return (0);
    }

    /*
     * Skip retrieving the on-disk value when there exists a restored update from history store in
     * the update list. Having a restored update as part of the update list indicates that the
     * existing on-disk value is unstable.
     */
    if (restored_upd != NULL) {
        WT_ASSERT(session, !WT_IS_HS(session->dhandle));
        cbt->upd_value->buf.data = restored_upd->data;
        cbt->upd_value->buf.size = restored_upd->size;
    } else {
        /*
         * When we inspected the update list we may have seen a tombstone leaving us with a valid
         * stop time window, we don't want to overwrite this stop time window.
         */
        have_stop_tw = WT_TIME_WINDOW_HAS_STOP(&cbt->upd_value->tw);

        if (read_onpage) {
            /*
             * We may have raced with checkpoint freeing the overflow blocks. Retry from start and
             * ignore the onpage value the next time. For pages that have remained in memory after a
             * checkpoint, this will lead us to read every key with an overflow removed onpage value
             * twice. However, it simplifies the logic and doesn't depend on the assumption that the
             * cell unpacking code will always return a correct time window even it returns a
             * WT_RESTART error.
             */
            ret = __wt_value_return_buf(cbt, cbt->ref, &cbt->upd_value->buf, &tw);
            if (ret == WT_RESTART) {
                read_onpage = false;
                goto retry;
            } else
                WT_RET(ret);

            /*
             * If the stop time point is set, that means that there is a tombstone at that time. If
             * it is not prepared and it is visible to our txn it means we've just spotted a
             * tombstone and should return "not found", except scanning the history store during
             * rollback to stable and when we are told to ignore non-globally visible tombstones.
             */
            if (!have_stop_tw && __wt_txn_tw_stop_visible(session, &tw) &&
              !F_ISSET(&cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE)) {
                cbt->upd_value->buf.data = NULL;
                cbt->upd_value->buf.size = 0;
                cbt->upd_value->type = WT_UPDATE_TOMBSTONE;
                WT_TIME_WINDOW_COPY_STOP(&cbt->upd_value->tw, &tw);
                return (0);
            }

            /* Store the stop time pair of the history store record that is returning. */
            if (!have_stop_tw && WT_TIME_WINDOW_HAS_STOP(&tw) && WT_IS_HS(session->dhandle))
                WT_TIME_WINDOW_COPY_STOP(&cbt->upd_value->tw, &tw);

            /*
             * We return the onpage value in the following cases:
             * 1. The record is from the history store.
             * 2. It is visible to the reader.
             */
            if (WT_IS_HS(session->dhandle) || __wt_txn_tw_start_visible(session, &tw)) {
                if (cbt->upd_value->skip_buf) {
                    cbt->upd_value->buf.data = NULL;
                    cbt->upd_value->buf.size = 0;
                }
                cbt->upd_value->type = WT_UPDATE_STANDARD;

                WT_TIME_WINDOW_COPY_START(&cbt->upd_value->tw, &tw);
                return (0);
            }
        }
    }

    /* If there's no visible update in the update chain or ondisk, check the history store file. */
    if (!F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY) &&
      F_ISSET_ATOMIC_32(S2C(session), WT_CONN_HS_OPEN) &&
      !F_ISSET(session->dhandle, WT_DHANDLE_HS)) {
        /*
         * Stressing this code path may slow down the system too much. To minimize the impact, sleep
         * on every random 100th iteration when this is enabled.
         */
        if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_HS_SEARCH) &&
          __wt_random(&session->rnd_random) % 100 == 0)
            __wt_timing_stress(session, WT_TIMING_STRESS_HS_SEARCH, NULL);

        WT_RET(__wt_hs_find_upd(session, S2BT(session)->id, key, cbt->iface.value_format, recno,
          cbt->upd_value, &cbt->upd_value->buf));
    }

    /*
     * Retry if we race with prepared commit or rollback. If we race with prepared rollback, the
     * value the reader should read may have been removed from the history store and appended to the
     * data store. If we race with prepared commit, imagine a case we read with timestamp 50 and we
     * have a prepared update with timestamp 30 and a history store record with timestamp 20,
     * committing the prepared update will cause the record being removed by reconciliation.
     */
    if (prepare_upd != NULL) {
        WT_ASSERT(session, F_ISSET(prepare_upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS));
        if (prepare_retry &&
          (prepare_upd->txnid == WT_TXN_ABORTED ||
            prepare_upd->prepare_state == WT_PREPARE_RESOLVED)) {
            prepare_retry = false;
            /* Clean out any stale value before performing the retry. */
            __wt_upd_value_clear(cbt->upd_value);
            WT_STAT_CONN_DSRC_INCR(session, txn_read_race_prepare_update);

            /*
             * When a prepared update/insert is rollback or committed, retrying it again should fix
             * concurrent modification of a prepared update. Other than prepared insert rollback,
             * rest of the cases, the history store update is either added to the end of the update
             * chain or modified to set proper stop timestamp. In all the scenarios, retrying again
             * will work to return a proper update.
             */
            goto retry;
        }
    }

    /* Return invalid not tombstone if nothing is found in history store. */
    WT_ASSERT(session, cbt->upd_value->type != WT_UPDATE_TOMBSTONE);
    return (0);
}

/*
 * __txn_incr_bytes_dirty --
 *     Increment the number of bytes dirty in the transaction.
 *
 * The "new_update" argument indicates whether a piece of data is: (1) Newly created (not just data
 *     being moved). (2) Exclusively belongs to the current transaction.
 *
 * There are two types of "dirty" data in the system for the purpose of this function: (1) Dirty
 *     data associated with a specific transaction. (2) Dirty data that isn't tied to a single
 *     transaction (e.g., a page with updates from multiple transactions).
 *
 * Examples:
 *
 * 1. A page can be dirty with multiple updates, each belonging to different transactions. In this
 *     case: (a) The updates are tied to specific transactions. (b) The page itself isn't
 *     exclusively tied to any one transaction.
 *
 * 2. During a page split, updates move between pages. However, this movement doesn't create new
 *     dirty data, so the "new_update" flag would be set to false.
 */
static void
__txn_incr_bytes_dirty(WT_SESSION_IMPL *session, size_t size, bool new_update)
{
    /*
     * For application threads, track the transaction bytes added to cache usage. We want to capture
     * only the application's own changes to page data structures. Exclude changes to internal pages
     * or changes that are the result of the application thread being co-opted into eviction work.
     */
    if (!new_update || F_ISSET(session, WT_SESSION_INTERNAL) ||
      !F_ISSET(session->txn, WT_TXN_RUNNING | WT_TXN_HAS_ID) ||
      __wt_session_gen(session, WT_GEN_EVICT) != 0)
        return;

    WT_STAT_CONN_INCRV_ATOMIC(session, cache_updates_txn_uncommitted_bytes, (int64_t)size);
    WT_STAT_CONN_INCRV_ATOMIC(session, cache_updates_txn_uncommitted_count, 1);
    WT_STAT_SESSION_INCRV(session, txn_bytes_dirty, (int64_t)size);
    WT_STAT_SESSION_INCRV(session, txn_updates, 1);
}

/*
 * __txn_clear_bytes_dirty --
 *     Clear the number of bytes dirty in the transaction.
 */
static void
__txn_clear_bytes_dirty(WT_SESSION_IMPL *session)
{
    int64_t val;

    val = WT_STAT_SESSION_READ(&(session)->stats, txn_bytes_dirty);
    if (val != 0) {
        WT_STAT_CONN_DECRV_ATOMIC(session, cache_updates_txn_uncommitted_bytes, val);
        WT_STAT_SESSION_SET(session, txn_bytes_dirty, 0);
    }

    val = WT_STAT_SESSION_READ(&(session)->stats, txn_updates);
    if (val != 0) {
        WT_STAT_CONN_DECRV_ATOMIC(session, cache_updates_txn_uncommitted_count, val);
        WT_STAT_SESSION_SET(session, txn_updates, 0);
    }
}

/*
 * __txn_remove_from_global_table --
 *     Remove the transaction id from the global transaction table.
 */
static WT_INLINE void
__txn_remove_from_global_table(WT_SESSION_IMPL *session)
{
#ifdef HAVE_DIAGNOSTIC
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    WT_ASSERT(session, txn->id >= __wt_atomic_loadv64(&txn_global->last_running));
    WT_ASSERT(
      session, txn->id != WT_TXN_NONE && __wt_atomic_loadv64(&txn_shared->id) != WT_TXN_NONE);
#else
    WT_TXN_SHARED *txn_shared;

    txn_shared = WT_SESSION_TXN_SHARED(session);
#endif
    WT_RELEASE_WRITE_WITH_BARRIER(txn_shared->id, WT_TXN_NONE);
}

/*
 * __wt_txn_claim_prepared_txn --
 *     Claim a prepared transaction.
 */
static WT_INLINE int
__wt_txn_claim_prepared_txn(WT_SESSION_IMPL *session, uint64_t prepared_id)
{
    WT_DECL_RET;
    WT_PENDING_PREPARED_ITEM *prepared_item;
    WT_TXN *txn;
    WT_TXN_OP *tmp_mod;
    txn = session->txn;
    WT_RET(__wt_prepared_discover_find_item(session, prepared_id, &prepared_item));
    txn->prepared_id = prepared_id;
    txn->prepare_timestamp = prepared_item->prepare_timestamp;
    F_SET(txn, WT_TXN_PREPARE | WT_TXN_HAS_PREPARED_ID | WT_TXN_HAS_TS_PREPARE | WT_TXN_RUNNING);
    /*
     * Swap mod array with prepared_item to avoid double-free on cursor close and when
     * commit/rollback.
     */
    tmp_mod = txn->mod;

    txn->mod = prepared_item->mod;
    txn->mod_alloc = prepared_item->mod_alloc;
    txn->mod_count = prepared_item->mod_count;

    prepared_item->mod = tmp_mod;
    prepared_item->mod_alloc = 0;
    prepared_item->mod_count = 0;
    WT_RET(__wt_prepared_discover_remove_item(session, prepared_id));
#ifdef HAVE_DIAGNOSTIC
    txn->prepare_count = prepared_item->prepare_count;
    prepared_item->prepare_count = 0;
#endif

    /* There's no txn id since claimed prepared txn is from recovery */
    WT_ASSERT(session, !F_ISSET(session->txn, WT_TXN_HAS_ID));
    return (ret);
}

/*
 * __wt_txn_begin --
 *     Begin a transaction.
 */
static WT_INLINE int
__wt_txn_begin(WT_SESSION_IMPL *session, WT_CONF *conf)
{
    WT_CONFIG_ITEM cval;
    WT_TXN *txn;
    wt_timestamp_t prepared_id;

    txn = session->txn;
    txn->isolation = session->isolation;
    txn->txn_log.txn_logsync = S2C(session)->log_mgr.txn_logsync;
    txn->commit_timestamp = WT_TS_NONE;
    txn->durable_timestamp = WT_TS_NONE;
    txn->first_commit_timestamp = WT_TS_NONE;
    txn->modify_block_count = 0;

    WT_ASSERT(session, !F_ISSET(txn, WT_TXN_RUNNING));

    WT_RET(__wt_txn_config(session, conf));

    if (conf != NULL) {
        WT_RET(__wt_conf_gets_def(session, conf, claim_prepared_id, 0, &cval));
        if (cval.len != 0) {
            WT_RET(__wt_txn_parse_prepared_id(session, &prepared_id, &cval));
            WT_RET(__wt_txn_claim_prepared_txn(session, prepared_id));
            return (0);
        }
    }

    /*
     * Allocate a snapshot if required or update the existing snapshot. Do not update the existing
     * snapshot of autocommit transactions because they are committed at the end of the operation.
     */
    if (txn->isolation == WT_ISO_SNAPSHOT &&
      !(F_ISSET(txn, WT_TXN_AUTOCOMMIT) && F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))) {
        if (session->ncursors > 0)
            WT_RET(__wt_session_copy_values(session));

        /*
         * Stall here if the cache is completely full. Eviction check can return rollback, but the
         * WT_SESSION.begin_transaction API can't, continue on.
         */
        WT_RET_ERROR_OK(
          __wt_evict_app_assist_worker_check(session, false, true, true, NULL), WT_ROLLBACK);

        __wt_txn_get_snapshot(session);
    }

    F_SET(txn, WT_TXN_RUNNING);
    if (F_ISSET(S2C(session), WT_CONN_READONLY))
        F_SET(txn, WT_TXN_READONLY);

    WT_ASSERT_ALWAYS(
      session, txn->mod_count == 0, "The mod count should be 0 when beginning a transaction");

    __txn_clear_bytes_dirty(session);

    return (0);
}

/*
 * __wt_txn_autocommit_check --
 *     If an auto-commit transaction is required, start one.
 */
static WT_INLINE int
__wt_txn_autocommit_check(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_TXN *txn;

    txn = session->txn;
    if (F_ISSET(txn, WT_TXN_AUTOCOMMIT)) {
        ret = __wt_txn_begin(session, NULL);
        F_CLR(txn, WT_TXN_AUTOCOMMIT);
    }
    return (ret);
}

/*
 * __wt_txn_idle_cache_check --
 *     If there is no transaction active in this thread and we haven't checked if the cache is full,
 *     do it now. If we have to block for eviction, this is the best time to do it.
 */
static WT_INLINE int
__wt_txn_idle_cache_check(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    /*
     * Check the published snap_min because read-uncommitted never sets WT_TXN_HAS_SNAPSHOT. We
     * don't have any transaction information at this point, so assume the transaction will be
     * read-only. The dirty cache check will be performed when the transaction completes, if
     * necessary.
     */
    if (F_ISSET(txn, WT_TXN_RUNNING) && !F_ISSET(txn, WT_TXN_HAS_ID) &&
      __wt_atomic_loadv64(&txn_shared->pinned_id) == WT_TXN_NONE)
        WT_RET(__wt_evict_app_assist_worker_check(session, false, true, true, NULL));

    return (0);
}

/*
 * __wt_txn_id_alloc --
 *     Allocate a new transaction ID.
 */
static WT_INLINE uint64_t
__wt_txn_id_alloc(WT_SESSION_IMPL *session, bool publish)
{
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;
    uint64_t id;

    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

    /*
     * Allocating transaction IDs involves several steps.
     *
     * Firstly, publish that this transaction is allocating its ID, then publish the transaction ID
     * as the current global ID. Note that this transaction ID might not be unique among threads and
     * hence not valid at this moment. The flag will notify other transactions that are attempting
     * to get their own snapshot for this transaction ID to retry.
     *
     * Then we do an atomic increment to allocate a unique ID. This will give the valid ID to this
     * transaction that we release to the global transaction table.
     *
     * We want the global value to lead the allocated values, so that any allocated transaction ID
     * eventually becomes globally visible. When there are no transactions running, the oldest_id
     * will reach the global current ID, so we want post-increment (fetch_add) semantics.
     *
     * We rely on atomic reads of the current ID to create snapshots, so for unlocked reads to be
     * well defined, we must use an atomic increment here.
     */
    if (publish) {
        WT_RELEASE_WRITE_WITH_BARRIER(txn_shared->is_allocating, true);
        WT_RELEASE_WRITE_WITH_BARRIER(txn_shared->id, txn_global->current);
        id = __wt_atomic_fetch_addv64(&txn_global->current, 1);
        session->txn->id = id;
        WT_RELEASE_WRITE_WITH_BARRIER(txn_shared->id, id);
        WT_RELEASE_WRITE_WITH_BARRIER(txn_shared->is_allocating, false);
    } else
        id = __wt_atomic_fetch_addv64(&txn_global->current, 1);

    return (id);
}

/*
 * __wt_txn_id_check --
 *     A transaction is going to do an update, allocate a transaction ID.
 */
static WT_INLINE int
__wt_txn_id_check(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;

    txn = session->txn;

    WT_ASSERT(session, F_ISSET(txn, WT_TXN_RUNNING));

    if (F_ISSET(txn, WT_TXN_HAS_ID))
        return (0);

    /*
     * Return error when the transactions with read committed or uncommitted isolation tries to
     * perform any write operation. Don't return an error for any update on metadata because it uses
     * special transaction visibility rules, search and updates on metadata happens in
     * read-uncommitted and read-committed isolation.
     */
    if (session->dhandle != NULL && !WT_IS_METADATA(session->dhandle) &&
      (txn->isolation == WT_ISO_READ_COMMITTED || txn->isolation == WT_ISO_READ_UNCOMMITTED)) {
        WT_ASSERT(session, !F_ISSET(session, WT_SESSION_INTERNAL));
        WT_RET_MSG(session, ENOTSUP,
          "write operations are not supported in read-committed or read-uncommitted transactions.");
    }

    /* If the transaction is idle, check that the cache isn't full. */
    WT_RET(__wt_txn_idle_cache_check(session));

    WT_IGNORE_RET(__wt_txn_id_alloc(session, true));

    /*
     * If we have used 64-bits of transaction IDs, there is nothing more we can do.
     */
    if (txn->id == WT_TXN_ABORTED)
        WT_RET_MSG(session, WT_ERROR, "out of transaction IDs");
    F_SET(txn, WT_TXN_HAS_ID);

    return (0);
}

/*
 * __wt_txn_search_check --
 *     Check if a search by the current transaction violates timestamp rules.
 */
static WT_INLINE int
__wt_txn_search_check(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    uint16_t flags;
    const char *name;

    txn = session->txn;
    flags = session->dhandle->ts_flags;
    name = session->dhandle->name;

    /* Timestamps are ignored on logged files. */
    if (F_ISSET(S2BT(session), WT_BTREE_LOGGED))
        return (0);

    /* Skip checks during recovery. */
    if (F_ISSET(S2C(session), WT_CONN_RECOVERING))
        return (0);

    /* Verify if the table should always or never use a read timestamp. */
    if (LF_ISSET(WT_DHANDLE_TS_ASSERT_READ_ALWAYS) && !F_ISSET(txn, WT_TXN_SHARED_TS_READ)) {
        __wt_err(session, EINVAL,
          "%s: " WT_TS_VERBOSE_PREFIX "read timestamps required and none set", name);
#ifdef HAVE_DIAGNOSTIC
        __wt_abort(session);
#endif
        return (EINVAL);
    }

    if (LF_ISSET(WT_DHANDLE_TS_ASSERT_READ_NEVER) && F_ISSET(txn, WT_TXN_SHARED_TS_READ)) {
        __wt_err(session, EINVAL,
          "%s: " WT_TS_VERBOSE_PREFIX "read timestamps disallowed and one set", name);
#ifdef HAVE_DIAGNOSTIC
        __wt_abort(session);
#endif
        return (EINVAL);
    }
    return (0);
}

/*
 * __txn_modify_block --
 *     Check if the current transaction can modify an item.
 */
static WT_INLINE int
__txn_modify_block(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd, wt_timestamp_t *prev_tsp)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_TIME_WINDOW tw;
    WT_TXN *txn;
    uint32_t snap_count;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool ignore_prepare_set, rollback, tw_found;

    rollback = tw_found = false;
    txn = session->txn;

    /*
     * Always include prepared transactions in this check: they are not supposed to affect
     * visibility for update operations.
     */
    ignore_prepare_set = F_ISSET(txn, WT_TXN_IGNORE_PREPARE);
    F_CLR(txn, WT_TXN_IGNORE_PREPARE);
    for (; upd != NULL && !__wt_txn_upd_visible(session, upd); upd = upd->next) {
        if (upd->txnid != WT_TXN_ABORTED) {
            ++txn->modify_block_count;
            __wt_verbose_level(session, WT_VERB_TRANSACTION,
              txn->modify_block_count >= WT_HUNDRED ? WT_VERBOSE_INFO : WT_VERBOSE_DEBUG_1,
              "Conflict with update with txn id %" PRIu64
              " at start timestamp: %s, prepare timestamp: %s",
              upd->txnid, __wt_timestamp_to_string(upd->upd_start_ts, ts_string[0]),
              __wt_timestamp_to_string(upd->prepare_ts, ts_string[1]));
            rollback = true;
            break;
        }
    }

    WT_ASSERT(session, upd != NULL || !rollback);

    /*
     * Check conflict against any on-page value if there is no update on the update chain except
     * aborted updates. Otherwise, we would have either already detected a conflict if we saw an
     * uncommitted update or determined that it would be safe to write if we saw a committed update.
     *
     * In the case of row-store we also need to check that the insert list is empty as the existence
     * of it implies there is no on disk value for the given key. However we can still get a
     * time-window from an unrelated on-disk value if we are not careful as the slot can still be
     * set on the cursor b-tree.
     */
    if (!rollback && upd == NULL && (CUR2BT(cbt)->type != BTREE_ROW || cbt->ins == NULL)) {
        tw_found = __wt_read_cell_time_window(cbt, &tw);
        if (tw_found) {
            if (WT_TIME_WINDOW_HAS_STOP(&tw)) {
                rollback = !__wt_txn_tw_stop_visible(session, &tw);
                if (rollback) {
                    ++txn->modify_block_count;
                    __wt_verbose_level(session, WT_VERB_TRANSACTION,
                      txn->modify_block_count >= WT_HUNDRED ? WT_VERBOSE_INFO : WT_VERBOSE_DEBUG_1,
                      "Conflict with update %" PRIu64
                      " at stop timestamp: %s, prepare timestamp: %s",
                      tw.stop_txn, __wt_timestamp_to_string(tw.stop_ts, ts_string[0]),
                      __wt_timestamp_to_string(tw.stop_prepare_ts, ts_string[1]));
                }
            } else {
                rollback = !__wt_txn_tw_start_visible(session, &tw);
                if (rollback) {
                    ++txn->modify_block_count;
                    __wt_verbose_level(session, WT_VERB_TRANSACTION,
                      txn->modify_block_count >= WT_HUNDRED ? WT_VERBOSE_INFO : WT_VERBOSE_DEBUG_1,
                      "Conflict with update %" PRIu64
                      " at start timestamp: %s, prepare timestamp: %s",
                      tw.start_txn, __wt_timestamp_to_string(tw.start_ts, ts_string[0]),
                      __wt_timestamp_to_string(tw.start_prepare_ts, ts_string[1]));
                }
            }
        }
    }

    if (rollback) {
        /* Dump information about the txn snapshot. */
        WT_VERBOSE_LEVEL level =
          txn->modify_block_count >= WT_HUNDRED ? WT_VERBOSE_INFO : WT_VERBOSE_DEBUG_1;

        if (WT_VERBOSE_LEVEL_ISSET(session, WT_VERB_TRANSACTION, level)) {
            WT_ERR(__wt_scr_alloc(session, 1024, &buf));
            WT_ERR(__wt_buf_fmt(session, buf,
              "snapshot_min=%" PRIu64 ", snapshot_max=%" PRIu64 ", snapshot_count=%" PRIu32,
              txn->snapshot_data.snap_min, txn->snapshot_data.snap_max,
              txn->snapshot_data.snapshot_count));
            if (txn->snapshot_data.snapshot_count > 0) {
                WT_ERR(__wt_buf_catfmt(session, buf, ", snapshots=["));
                for (snap_count = 0; snap_count < txn->snapshot_data.snapshot_count - 1;
                     ++snap_count)
                    WT_ERR(__wt_buf_catfmt(
                      session, buf, "%" PRIu64 ",", txn->snapshot_data.snapshot[snap_count]));
                WT_ERR(__wt_buf_catfmt(
                  session, buf, "%" PRIu64 "]", txn->snapshot_data.snapshot[snap_count]));
            }
            __wt_verbose_level(session, WT_VERB_TRANSACTION, level, "%s", (const char *)buf->data);
        }

        WT_STAT_CONN_DSRC_INCR(session, txn_update_conflict);
        ret = WT_ROLLBACK;
        __wt_session_set_last_error(
          session, ret, WT_WRITE_CONFLICT, WT_TXN_ROLLBACK_REASON_CONFLICT);
    }

    /*
     * Don't access the update from an uncommitted transaction as it can produce wrong timestamp
     * results.
     */
    if (!rollback && prev_tsp != NULL) {
        if (upd != NULL) {
            /* The durable timestamp must be greater than or equal to the commit timestamp. */
            WT_ASSERT(session, upd->upd_durable_ts >= upd->upd_start_ts);
            *prev_tsp = upd->upd_durable_ts;
        } else if (tw_found)
            *prev_tsp = WT_TIME_WINDOW_HAS_STOP(&tw) ? tw.durable_stop_ts : tw.durable_start_ts;
    }

    if (ignore_prepare_set)
        F_SET(txn, WT_TXN_IGNORE_PREPARE);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_txn_modify_check --
 *     Check if the current transaction can modify an item.
 */
static WT_INLINE int
__wt_txn_modify_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd,
  wt_timestamp_t *prev_tsp, u_int modify_type)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;

    txn = session->txn;

    /*
     * Check if this operation is permitted, skipping if transaction isolation is not snapshot or
     * operating on the metadata table.
     */
    if (txn->isolation == WT_ISO_SNAPSHOT && !WT_IS_METADATA(cbt->dhandle))
        WT_RET(__txn_modify_block(session, cbt, upd, prev_tsp));

    /*
     * Prepending a tombstone to another tombstone indicates remove of a non-existent key and that
     * isn't permitted, return a WT_NOTFOUND error.
     */
    if (modify_type == WT_UPDATE_TOMBSTONE) {
        /* Loop until a valid update is found. */
        while (upd != NULL && upd->txnid == WT_TXN_ABORTED)
            upd = upd->next;

        if (upd != NULL && upd->type == WT_UPDATE_TOMBSTONE)
            return (WT_NOTFOUND);
    }

    /* Everything is OK, optionally rollback for testing (skipping metadata operations). */
    if (!WT_IS_METADATA(cbt->dhandle)) {
        txn_global = &S2C(session)->txn_global;
        if (txn_global->debug_rollback != 0 &&
          ++txn_global->debug_ops % txn_global->debug_rollback == 0)
            WT_RET_SUB(session, WT_ROLLBACK, WT_NONE, "debug mode simulated conflict");
    }
    return (0);
}

/*
 * __wt_txn_read_last --
 *     Called when the last page for a session is released.
 */
static WT_INLINE void
__wt_txn_read_last(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;

    txn = session->txn;

    /*
     * Release the snap_min ID we put in the global table.
     *
     * If the isolation has been temporarily forced, don't touch the snapshot here: it will be
     * restored by WT_WITH_TXN_ISOLATION.
     */
    if ((!F_ISSET(txn, WT_TXN_RUNNING) || txn->isolation != WT_ISO_SNAPSHOT) &&
      txn->forced_iso == 0)
        __wt_txn_release_snapshot(session);
}

/*
 * __wt_txn_read_committed_should_release_snapshot --
 *     Called to check whether we want to release our snapshot through calling WT_CURSOR::reset().
 */
static WT_INLINE bool
__wt_txn_read_committed_should_release_snapshot(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;

    txn = session->txn;

    /* Check if we can release the snap_min ID we put in the global table. */
    return (
      (!F_ISSET(txn, WT_TXN_RUNNING) || txn->isolation != WT_ISO_SNAPSHOT) && txn->forced_iso == 0);
}

/*
 * __wt_txn_cursor_op --
 *     Called for each cursor operation.
 */
static WT_INLINE void
__wt_txn_cursor_op(WT_SESSION_IMPL *session)
{
    WT_TXN *txn;
    WT_TXN_GLOBAL *txn_global;
    WT_TXN_SHARED *txn_shared;

    txn = session->txn;
    txn_global = &S2C(session)->txn_global;
    txn_shared = WT_SESSION_TXN_SHARED(session);

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
        if (__wt_atomic_loadv64(&txn_shared->pinned_id) == WT_TXN_NONE)
            __wt_atomic_storev64(
              &txn_shared->pinned_id, __wt_atomic_loadv64(&txn_global->last_running));
        if (__wt_atomic_loadv64(&txn_shared->metadata_pinned) == WT_TXN_NONE)
            __wt_atomic_storev64(
              &txn_shared->metadata_pinned, __wt_atomic_loadv64(&txn_shared->pinned_id));
    } else if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        __wt_txn_get_snapshot(session);
}

/*
 * __wt_txn_activity_check --
 *     Check whether there are any running transactions.
 */
static WT_INLINE int
__wt_txn_activity_check(WT_SESSION_IMPL *session, bool *txn_active)
{
    WT_TXN_GLOBAL *txn_global;

    txn_global = &S2C(session)->txn_global;

    /*
     * Default to true - callers shouldn't rely on this if an error is returned, but let's give them
     * deterministic behavior if they do.
     */
    *txn_active = true;

    /*
     * Ensure the oldest ID is as up to date as possible so we can use a simple check to find if
     * there are any running transactions.
     */
    WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    *txn_active =
      (__wt_atomic_loadv64(&txn_global->oldest_id) != __wt_atomic_loadv64(&txn_global->current) ||
        __wt_atomic_loadv64(&txn_global->metadata_pinned) !=
          __wt_atomic_loadv64(&txn_global->current));

    return (0);
}

/*
 * __wt_upd_value_assign --
 *     Point an update value at a given update. We're specifically not getting the value to own the
 *     memory since this exists in an update list somewhere.
 */
static WT_INLINE void
__wt_upd_value_assign(WT_UPDATE_VALUE *upd_value, WT_UPDATE *upd)
{
    uint8_t prepare_state;

    WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);

    if (!upd_value->skip_buf) {
        upd_value->buf.data = upd->data;
        upd_value->buf.size = upd->size;
    }
    if (upd->type == WT_UPDATE_TOMBSTONE)
        WT_TIME_WINDOW_SET_STOP(&upd_value->tw, upd,
          prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);
    else
        WT_TIME_WINDOW_SET_START(&upd_value->tw, upd,
          prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);

    upd_value->type = upd->type;
}

/*
 * __wt_upd_value_clear --
 *     Clear an update value to its defaults.
 */
static WT_INLINE void
__wt_upd_value_clear(WT_UPDATE_VALUE *upd_value)
{
    /*
     * Make sure we don't touch the memory pointers here. If we have some allocated memory, that
     * could come in handy next time we need to write to the buffer.
     */
    upd_value->buf.data = NULL;
    upd_value->buf.size = 0;
    WT_TIME_WINDOW_INIT(&upd_value->tw);
    upd_value->type = WT_UPDATE_INVALID;
}

#define WT_SKIP_ABORTED_AND_SET_CHECK_PREPARED(temp_txnid, txnid_prepared, check_prepared, upd) \
    WT_ACQUIRE_READ((temp_txnid), (upd)->txnid);                                                \
    if ((temp_txnid) == WT_TXN_ABORTED) {                                                       \
        if (!(check_prepared))                                                                  \
            continue;                                                                           \
                                                                                                \
        /* We may see aborted reserve updates in between the prepared updates. */               \
        if ((upd)->type == WT_UPDATE_RESERVE)                                                   \
            continue;                                                                           \
                                                                                                \
        /*                                                                                      \
         * If we have multiple prepared updates from the same transaction, there is no other    \
         * updates in between them.                                                             \
         */                                                                                     \
        uint8_t tmp_prepare_state;                                                              \
        WT_ACQUIRE_READ(tmp_prepare_state, (upd)->prepare_state);                               \
        if (tmp_prepare_state != WT_PREPARE_INPROGRESS &&                                       \
          tmp_prepare_state != WT_PREPARE_LOCKED) {                                             \
            (check_prepared) = false;                                                           \
            continue;                                                                           \
        }                                                                                       \
                                                                                                \
        if ((upd)->upd_saved_txnid != txnid_prepared) {                                         \
            (check_prepared) = false;                                                           \
            continue;                                                                           \
        }                                                                                       \
    }
