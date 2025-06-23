/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_prepared_discover_find_or_create_transaction --
 *     We have learned that a prepared transaction with a particular ID exists. If this is the first
 *     time it's been noticed, create a transaction corresponding to it. Otherwise return the
 *     matching transaction.
 */
int
__wt_prepared_discover_find_or_create_transaction(
  WT_SESSION_IMPL *session, wt_timestamp_t prepare_transaction_id, WT_SESSION_IMPL **prep_sessionp)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *new_session, *next_session;
    WT_TXN_GLOBAL *txn_global;
    uint32_t prepared_session_cnt;

    new_session = next_session = NULL;
    conn = S2C(session);
    txn_global = &conn->txn_global;

    if (txn_global->pending_prepared_sessions != NULL) {
        for (prepared_session_cnt = 0;
             (next_session = txn_global->pending_prepared_sessions[prepared_session_cnt]) != NULL;
             prepared_session_cnt++) {
            if (next_session->txn->prepare_timestamp == prepare_transaction_id) {
                *prep_sessionp = next_session;
                return (0);
            }
        }
    }

    /* No existing session/transaction matched, create a new one */
    WT_RET(__wt_realloc_def(session, &txn_global->pending_prepared_sessions_allocated,
      txn_global->pending_prepared_sessions_count + 1, &txn_global->pending_prepared_sessions));

    /* Allocate a new session and setup the transaction ready for populating */
    WT_RET(__wt_open_internal_session(conn, "prepared_discover", true, 0, 0, &new_session));
    WT_RET(__wt_txn_begin(new_session, NULL));
    new_session->txn->prepare_timestamp = prepare_transaction_id;
    new_session->txn->prepared_id = prepare_transaction_id;
    /* Add it to the discovered set of sessions. */
    txn_global->pending_prepared_sessions[txn_global->pending_prepared_sessions_count++] =
      new_session;
    F_SET(new_session->txn, WT_TXN_PREPARE);
    *prep_sessionp = new_session;
    return (0);
}

/*
 * __wti_prepared_discover_add_artifact_upd --
 *     Add an artifact to a pending prepared transaction.
 */
int
__wti_prepared_discover_add_artifact_upd(
  WT_SESSION_IMPL *session, wt_timestamp_t prepare_transaction_id, WT_ITEM *key, WT_UPDATE *upd)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *prep_session;

    WT_RET(__wt_prepared_discover_find_or_create_transaction(
      session, prepare_transaction_id, &prep_session));

    /* Add the update to the prepared transaction context. */
    WT_WITH_DHANDLE(prep_session, session->dhandle, ret = __wt_txn_modify(prep_session, upd));
    WT_RET(ret);
    /* Copy the key into the transaction operation, so it can be used to find the update later. */
    WT_WITH_DHANDLE(prep_session, session->dhandle, ret = __wt_txn_op_set_key(prep_session, key));
    WT_RET(ret);

#ifdef HAVE_DIAGNOSTIC
    ++prep_session->txn->prepare_count;
#endif

    return (0);
}

/*
 * __wti_prepared_discover_add_artifact_ondisk_row --
 *     Add an artifact to a pending prepared transaction.
 */
int
__wti_prepared_discover_add_artifact_ondisk_row(
  WT_SESSION_IMPL *session, wt_timestamp_t prepare_transaction_id, WT_TIME_WINDOW *tw, WT_ITEM *key)
{
    WT_DECL_RET;
    WT_UPDATE *upd;

    /*
     * Create an update structure with the time information and state populated - that allows this
     * code to reuse existing machinery for installing transaction operations.
     */
    WT_RET(__wt_upd_alloc(session, NULL, WT_UPDATE_STANDARD, &upd, NULL));
    upd->txnid = session->txn->id;
    upd->durable_ts = tw->durable_start_ts;
    upd->start_ts = tw->start_ts;
    upd->prepare_state = WT_PREPARE_INPROGRESS;

    WT_ERR(__wti_prepared_discover_add_artifact_upd(session, prepare_transaction_id, key, upd));
err:
    /*
     * It's OK to free the update now, the transaction structure will lookup using the key since
     * this is for a prepared transaction.
     */
    __wt_free_update_list(session, &upd);
    return (ret);
}
