/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Wrapper for substituting checkpoint state when doing checkpoint cursor operations.
 *
 * A checkpoint cursor has two extra things in it: a dummy transaction (always), and a dhandle for
 * the corresponding history store checkpoint (mostly but not always).
 *
 * If there's a checkpoint transaction, it means we're a checkpoint cursor. In that case we
 * substitute the transaction into the session, and also stick the checkpoint name of the history
 * store dhandle in the session for when the history store is opened. And store the base write
 * generation we got from the global checkpoint metadata in the session for use in the unpacking
 * cleanup code. After the operation completes we then undo it all.
 *
 * If the current transaction is _already_ a checkpoint cursor dummy transaction, however, do
 * nothing. This happens when the history store logic opens history store cursors inside checkpoint
 * cursor operations on the data store. In that case we want to keep the existing state. Note that
 * in this case we know that the checkpoint write generation is the same -- we are reading some
 * specific checkpoint and we got that checkpoint's write generation from the global checkpoint
 * metadata, not from per-tree information.
 */
#define WT_WITH_CHECKPOINT(session, cbt, op)                                                  \
    do {                                                                                      \
        WT_TXN *__saved_txn;                                                                  \
        uint64_t __saved_write_gen = (session)->ckpt.write_gen;                               \
        bool no_reconcile_set;                                                                \
                                                                                              \
        no_reconcile_set = F_ISSET((session), WT_SESSION_NO_RECONCILE);                       \
        if ((cbt)->checkpoint_txn != NULL) {                                                  \
            __saved_txn = (session)->txn;                                                     \
            if (F_ISSET(__saved_txn, WT_TXN_IS_CHECKPOINT)) {                                 \
                WT_ASSERT(session, (cbt)->checkpoint_write_gen == (session)->ckpt.write_gen); \
                __saved_txn = NULL;                                                           \
            } else {                                                                          \
                (session)->txn = (cbt)->checkpoint_txn;                                       \
                /* Reconciliation is disabled when reading a checkpoint. */                   \
                F_SET((session), WT_SESSION_NO_RECONCILE);                                    \
                if ((cbt)->checkpoint_hs_dhandle != NULL) {                                   \
                    WT_ASSERT(session, (session)->hs_checkpoint == NULL);                     \
                    (session)->hs_checkpoint = (cbt)->checkpoint_hs_dhandle->checkpoint;      \
                }                                                                             \
                __saved_write_gen = (session)->ckpt.write_gen;                                \
                (session)->ckpt.write_gen = (cbt)->checkpoint_write_gen;                      \
            }                                                                                 \
        } else                                                                                \
            __saved_txn = NULL;                                                               \
        op;                                                                                   \
        if (__saved_txn != NULL) {                                                            \
            (session)->txn = __saved_txn;                                                     \
            if (!no_reconcile_set)                                                            \
                F_CLR((session), WT_SESSION_NO_RECONCILE);                                    \
            (session)->hs_checkpoint = NULL;                                                  \
            (session)->ckpt.write_gen = __saved_write_gen;                                    \
        }                                                                                     \
    } while (0)

/*
 * __curfile_check_cbt_txn --
 *     Enforce restrictions on nesting checkpoint cursors. The only nested cursors we should get to
 *     from a checkpoint cursor are cursors for the corresponding history store checkpoint.
 */
static WT_INLINE int
__curfile_check_cbt_txn(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
    WT_TXN *txn;

    txn = session->txn;

    /* If not reading a checkpoint, everything's fine. */
    if (cbt->checkpoint_txn == NULL)
        return (0);

    /*
     * It is ok if the current transaction is already a checkpoint transaction. Assert that we are
     * the history store.
     */
    if (F_ISSET(txn, WT_TXN_IS_CHECKPOINT)) {
        WT_ASSERT(session, WT_IS_HS(cbt->dhandle));
        WT_ASSERT(session, WT_DHANDLE_IS_CHECKPOINT(cbt->dhandle));
    }

    return (0);
}

/*
 * __wt_cursor_checkpoint_id --
 *     Return the checkpoint ID for checkpoint cursors, otherwise 0.
 */
uint64_t
__wt_cursor_checkpoint_id(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;

    cbt = (WT_CURSOR_BTREE *)cursor;

    return (cbt->checkpoint_id);
}

/*
 * __curfile_compare --
 *     WT_CURSOR->compare method for the btree cursor type.
 */
static int
__curfile_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)a;
    CURSOR_API_CALL(a, session, ret, compare, cbt->dhandle);

    /*
     * Check both cursors are a btree type then call the underlying function, it can handle cursors
     * pointing to different objects.
     */
    if (!WT_BTREE_PREFIX(a->internal_uri) || !WT_BTREE_PREFIX(b->internal_uri))
        WT_ERR_MSG(session, EINVAL, "Cursors must reference the same object");

    WT_ERR(__cursor_checkkey(a));
    WT_ERR(__cursor_checkkey(b));

    ret = __wt_btcur_compare((WT_CURSOR_BTREE *)a, (WT_CURSOR_BTREE *)b, cmpp);

err:
    API_END_RET_STAT(session, ret, cursor_compare);
}

/*
 * __curfile_equals --
 *     WT_CURSOR->equals method for the btree cursor type.
 */
static int
__curfile_equals(WT_CURSOR *a, WT_CURSOR *b, int *equalp)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)a;
    CURSOR_API_CALL(a, session, ret, equals, cbt->dhandle);

    /*
     * Check both cursors are a btree type then call the underlying function, it can handle cursors
     * pointing to different objects.
     */
    if (!WT_BTREE_PREFIX(a->internal_uri) || !WT_BTREE_PREFIX(b->internal_uri))
        WT_ERR_MSG(session, EINVAL, "Cursors must reference the same object");

    WT_ERR(__cursor_checkkey(a));
    WT_ERR(__cursor_checkkey(b));

    ret = __wt_btcur_equals((WT_CURSOR_BTREE *)a, (WT_CURSOR_BTREE *)b, equalp);

err:
    API_END_RET_STAT(session, ret, cursor_equals);
}

/*
 * __curfile_next --
 *     WT_CURSOR->next method for the btree cursor type.
 */
static int
__curfile_next(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, cbt->dhandle);
    CURSOR_REPOSITION_ENTER(cursor, session);
    WT_ERR(__cursor_copy_release(cursor));

    WT_ERR(__curfile_check_cbt_txn(session, cbt));

    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_next(cbt, false));
    WT_ERR(ret);

    /* Next maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    CURSOR_REPOSITION_END(cursor, session);
    API_END_RET_STAT(session, ret, cursor_next);
}

/*
 * __wti_curfile_next_random --
 *     WT_CURSOR->next method for the btree cursor type when configured with next_random.
 */
int
__wti_curfile_next_random(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, cbt->dhandle);
    WT_ERR(__cursor_copy_release(cursor));

    WT_ERR(__curfile_check_cbt_txn(session, cbt));

    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_next_random(cbt));
    WT_ERR(ret);

    /* Next-random maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    API_END_RET_STAT(session, ret, cursor_next_random);
}

/*
 * __curfile_prev --
 *     WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curfile_prev(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, prev, cbt->dhandle);
    CURSOR_REPOSITION_ENTER(cursor, session);
    WT_ERR(__cursor_copy_release(cursor));

    WT_ERR(__curfile_check_cbt_txn(session, cbt));

    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_prev(cbt, false));
    WT_ERR(ret);

    /* Prev maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    CURSOR_REPOSITION_END(cursor, session);
    API_END_RET_STAT(session, ret, cursor_prev);
}

/*
 * __curfile_reset --
 *     WT_CURSOR->reset method for the btree cursor type.
 */
static int
__curfile_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, cbt->dhandle);
    WT_ERR(__cursor_copy_release(cursor));

    ret = __wt_btcur_reset(cbt);

    /*
     * The bounded cursor API clears bounds on external calls to cursor->reset. We determine this by
     * guarding the call to cursor bound reset with the API_USER_ENTRY macro. Doing so prevents
     * internal API calls from resetting cursor bounds unintentionally, e.g. cursor->remove.
     */
    if (API_USER_ENTRY(session))
        __wt_cursor_bound_reset(cursor);

    /* Reset maintains no position, key or value. */
    WT_ASSERT(session,
      !F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == 0 &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:
    API_END_RET_STAT(session, ret, cursor_reset);
}

/*
 * __curfile_search --
 *     WT_CURSOR->search method for the btree cursor type.
 */
static int
__curfile_search(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, search, cbt->dhandle);
    API_RETRYABLE(session);
    CURSOR_REPOSITION_ENTER(cursor, session);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    WT_ERR(__curfile_check_cbt_txn(session, cbt));

    time_start = __wt_clock(session);
    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_search(cbt));
    WT_ERR(ret);
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opread(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /* Search maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    CURSOR_REPOSITION_END(cursor, session);
    API_RETRYABLE_END(session, ret);
    API_END_RET_STAT(session, ret, cursor_search);
}

/*
 * __wti_curfile_search_near --
 *     WT_CURSOR->search_near method for the btree cursor type.
 */
int
__wti_curfile_search_near(WT_CURSOR *cursor, int *exact)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL(cursor, session, ret, search_near, cbt->dhandle);
    API_RETRYABLE(session);
    CURSOR_REPOSITION_ENTER(cursor, session);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    WT_ERR(__curfile_check_cbt_txn(session, cbt));

    time_start = __wt_clock(session);
    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_search_near(cbt, exact));
    WT_ERR(ret);
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opread(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /* Search-near maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    CURSOR_REPOSITION_END(cursor, session);
    API_RETRYABLE_END(session, ret);
    API_END_RET_STAT(session, ret, cursor_search_near);
}

/*
 * __curfile_insert --
 *     WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curfile_insert(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_UPDATE_API_CALL_BTREE(cursor, session, ret, insert);
    WT_ERR(__cursor_copy_release(cursor));

    if (!F_ISSET(cursor, WT_CURSTD_APPEND))
        WT_ERR(__cursor_checkkey(cursor));
    WT_ERR(__cursor_checkvalue(cursor));

    time_start = __wt_clock(session);
    WT_ERR(__wt_btcur_insert(cbt));
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opwrite(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /*
     * Insert maintains no position, key or value (except for column-store appends, where we are
     * returning a key).
     */
    WT_ASSERT(session,
      !F_ISSET(cbt, WT_CBT_ACTIVE) &&
        ((F_ISSET(cursor, WT_CURSTD_APPEND) &&
           F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_EXT) ||
          (!F_ISSET(cursor, WT_CURSTD_APPEND) && F_MASK(cursor, WT_CURSTD_KEY_SET) == 0)));
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_insert);
    return (ret);
}

/*
 * __wt_curfile_insert_check --
 *     WT_CURSOR->insert_check method for the btree cursor type.
 */
int
__wt_curfile_insert_check(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int tret;

    cbt = (WT_CURSOR_BTREE *)cursor;
    tret = 0;
    CURSOR_UPDATE_API_CALL_BTREE(cursor, session, ret, insert_check);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    tret = __wt_btcur_insert_check(cbt);

/*
 * Detecting a conflict should not cause transaction error.
 */
err:
    CURSOR_UPDATE_API_END(session, ret);
    WT_TRET(tret);
    API_RET_STAT(session, ret, cursor_insert_check);
}

/*
 * __curfile_modify --
 *     WT_CURSOR->modify method for the btree cursor type.
 */
static int
__curfile_modify(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_UPDATE_API_CALL_BTREE(cursor, session, ret, modify);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    /* Check for a rational modify vector count. */
    if (nentries <= 0)
        WT_ERR_MSG(session, EINVAL, "Illegal modify vector with %d entries", nentries);

    time_start = __wt_clock(session);
    WT_ERR(__wt_btcur_modify(cbt, entries, nentries));
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opwrite(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /*
     * Modify maintains a position, key and value. Unlike update, it's not always an internal value.
     */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) != 0);

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_modify);
    return (ret);
}

/*
 * __curfile_update --
 *     WT_CURSOR->update method for the btree cursor type.
 */
static int
__curfile_update(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_UPDATE_API_CALL_BTREE(cursor, session, ret, update);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));
    WT_ERR(__cursor_checkvalue(cursor));

    time_start = __wt_clock(session);
    WT_ERR(__wt_btcur_update(cbt));
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opwrite(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /* Update maintains a position, key and value. */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
        F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_update);
    return (ret);
}

/*
 * __curfile_remove --
 *     WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curfile_remove(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t time_start, time_stop;
    bool positioned;

    /*
     * WT_CURSOR.remove has a unique semantic, the cursor stays positioned if it starts positioned,
     * otherwise clear the cursor on completion. Track if starting with a positioned cursor and pass
     * that information into the underlying Btree remove function so it tries to maintain a position
     * in the tree. This is complicated by the loop in this code that restarts operations if they
     * return prepare-conflict or restart.
     */
    positioned = F_ISSET(cursor, WT_CURSTD_KEY_INT);

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_REMOVE_API_CALL(cursor, session, ret, cbt->dhandle);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    time_start = __wt_clock(session);
    WT_ERR(__wt_btcur_remove(cbt, positioned));
    time_stop = __wt_clock(session);
    __wt_stat_usecs_hist_incr_opwrite(session, WT_CLOCKDIFF_US(time_stop, time_start));

    /* If we've lost an initial position, we must fail. */
    if (positioned && !F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
        __wt_verbose_notice(session, WT_VERB_ERROR_RETURNS, "%s",
          "WT_ROLLBACK: rolling back cursor remove as initial position was lost");
        WT_ERR(WT_ROLLBACK);
    }

    /*
     * Remove with a search-key is fire-and-forget, no position and no key. Remove starting from a
     * position maintains the position and a key, but the key can end up being internal, external,
     * or not set, there's nothing to assert. There's never a value.
     */
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:
    /* If we've lost an initial position, we must fail. */
    CURSOR_UPDATE_API_END_RETRY_STAT(
      session, ret, !positioned || F_ISSET(cursor, WT_CURSTD_KEY_INT), cursor_remove);
    return (ret);
}

/*
 * __curfile_reserve --
 *     WT_CURSOR->reserve method for the btree cursor type.
 */
static int
__curfile_reserve(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_UPDATE_API_CALL_BTREE(cursor, session, ret, reserve);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_checkkey(cursor));

    WT_ERR(__wt_txn_context_check(session, true));

    WT_ERR(__wt_btcur_reserve(cbt));

    /*
     * Reserve maintains a position and key, which doesn't match the library API, where reserve
     * maintains a value. Fix the API by searching after each successful reserve operation.
     */
    WT_ASSERT(session,
      F_ISSET(cbt, WT_CBT_ACTIVE) && F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_reserve);

    /*
     * The application might do a WT_CURSOR.get_value call when we return, so we need a value and
     * the underlying functions didn't set one up. For various reasons, those functions may not have
     * done a search and any previous value in the cursor might race with WT_CURSOR.reserve. For
     * simplicity, repeat the search here.
     */
    return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __curfile_close --
 *     WT_CURSOR->close method for the btree cursor type.
 */
static int
__curfile_close(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool dead, released;

    cbt = (WT_CURSOR_BTREE *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, cbt->dhandle);
    WT_ERR(__cursor_copy_release(cursor));
err:

    /* Only try to cache the cursor if there's no error. */
    if (ret == 0) {
        /*
         * If releasing the cursor fails in any way, it will be left in a state that allows it to be
         * normally closed.
         */
        ret = __wti_cursor_cache_release(session, cursor, &released);
        if (released)
            goto done;
    }

    dead = F_ISSET(cursor, WT_CURSTD_DEAD);

    /* For cached cursors, free any extra buffers retained now. */
    __wt_cursor_free_cached_memory(cursor);

    /* Free the bulk-specific resources. */
    if (F_ISSET(cursor, WT_CURSTD_BULK))
        WT_TRET(__wti_curbulk_close(session, (WT_CURSOR_BULK *)cursor));

    WT_TRET(__wt_btcur_close(cbt, false));
    /* The URI is owned by the btree handle. */
    cursor->internal_uri = NULL;

    WT_ASSERT(session,
      session->dhandle == NULL || __wt_atomic_loadi32(&session->dhandle->session_inuse) > 0);

    /* Free any private transaction set up for a checkpoint cursor. */
    if (cbt->checkpoint_txn != NULL)
        __wt_txn_close_checkpoint_cursor(session, &cbt->checkpoint_txn);

    /* Close any history store handle set up for a checkpoint cursor. */
    if (cbt->checkpoint_hs_dhandle != NULL) {
        WT_WITH_DHANDLE(
          session, cbt->checkpoint_hs_dhandle, WT_TRET(__wt_session_release_dhandle(session)));
        cbt->checkpoint_hs_dhandle = NULL;
    }

    __wt_cursor_close(cursor);

    /*
     * Note: release the data handle last so that cursor statistics are updated correctly.
     */
    if (session->dhandle != NULL) {
        /* Decrement the data-source's in-use counter. */
        __wt_cursor_dhandle_decr_use(session);

        /*
         * If the cursor was marked dead, we got here from reopening a cached cursor, which had a
         * handle that was dead at that time, so it did not obtain a lock on the handle.
         */
        if (!dead)
            WT_TRET(__wt_session_release_dhandle(session));
    }

done:
    API_END_RET_STAT(session, ret, cursor_close);
}

/*
 * __curfile_cache --
 *     WT_CURSOR->cache method for the btree cursor type.
 */
static int
__curfile_cache(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;
    session = CUR2S(cursor);

    WT_TRET(__wti_cursor_cache(cursor, cbt->dhandle));
    WT_TRET(__wt_session_release_dhandle(session));

    API_RET_STAT(session, ret, cursor_cache);
}

/*
 * __curfile_reopen_int --
 *     Helper for __curfile_reopen, called with the session data handle set.
 */
static int
__curfile_reopen_int(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool is_dead;

    session = CUR2S(cursor);
    dhandle = session->dhandle;

    /*
     * Lock the handle: we're only interested in open handles, any other state disqualifies the
     * cache.
     */
    ret = __wt_session_lock_dhandle(session, 0, &is_dead);
    if (!is_dead && ret == 0 && !WT_DHANDLE_CAN_REOPEN(dhandle)) {
        WT_RET(__wt_session_release_dhandle(session));
        ret = __wt_set_return(session, EBUSY);
    }

    /*
     * The data handle may not be available, in which case handle it like a dead handle: fail the
     * reopen, and flag the cursor so that the handle won't be unlocked when subsequently closed.
     */
    if (is_dead || ret == EBUSY) {
        F_SET(cursor, WT_CURSTD_DEAD);
        ret = WT_NOTFOUND;
    }
    __wti_cursor_reopen(cursor, dhandle);

    /*
     * The btree handle may have been reopened since we last accessed it. Reset fields in the cursor
     * that point to memory owned by the btree handle.
     */
    if (ret == 0) {
        /* Assert a valid tree (we didn't race with eviction). */
        WT_ASSERT(session, WT_DHANDLE_BTREE(dhandle));
        WT_ASSERT(session, ((WT_BTREE *)dhandle->handle)->root.page != NULL);

        btree = CUR2BT(cursor);
        cursor->internal_uri = btree->dhandle->name;
        cursor->key_format = btree->key_format;
        cursor->value_format = btree->value_format;

        WT_STAT_CONN_DSRC_INCR(session, cursor_reopen);
    }
    return (ret);
}

/*
 * __curfile_reopen --
 *     WT_CURSOR->reopen method for the btree cursor type.
 */
static int
__curfile_reopen(WT_CURSOR *cursor, bool sweep_check_only)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool can_sweep;

    session = CUR2S(cursor);
    dhandle = ((WT_CURSOR_BTREE *)cursor)->dhandle;

    if (sweep_check_only) {
        /*
         * The sweep check returns WT_NOTFOUND if the cursor should be swept. Generally if the
         * associated data handle cannot be reopened it should be swept. But a handle being operated
         * on by this thread should not be swept. The situation where a handle cannot be reopened
         * but also cannot be swept can occur if this thread is in the middle of closing a cursor
         * for a handle that is marked as dropped. During the close, a few iterations of the session
         * cursor sweep are run. The sweep calls this function to see if a cursor should be swept,
         * and it may thus be asking about the very cursor being closed.
         */
        can_sweep = !WT_DHANDLE_CAN_REOPEN(dhandle) && dhandle != session->dhandle;
        return (can_sweep ? WT_NOTFOUND : 0);
    }

    /*
     * Temporarily set the session's data handle to the data handle in the cursor. Reopen may be
     * called either as part of an open API call, or during cursor sweep as part of a different API
     * call, so we need to restore the original data handle that was in our session after the reopen
     * completes.
     */
    WT_WITH_DHANDLE(session, dhandle, ret = __curfile_reopen_int(cursor));
    API_RET_STAT(session, ret, cursor_reopen);
}

/*
 * __curfile_setup_checkpoint --
 *     Open helper code for checkpoint cursors.
 */
static int
__curfile_setup_checkpoint(WT_CURSOR_BTREE *cbt, const char *cfg[], WT_DATA_HANDLE *hs_dhandle,
  WT_CKPT_SNAPSHOT *ckpt_snapshot)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);

    /*
     * It is important that reading from a checkpoint also reads from the corresponding
     * checkpoint of the history store and also uses matching snapshot and timestamp data;
     * otherwise all kinds of things go wrong. The logic for getting a matching set is complex (what
     * it means to be "matching" is also complex) and is explained in session_dhandle.c. This
     * comment explains what happens once we get a matching set so that subsequent reads work
     * correctly.
     *
     * 1. When we get here, if we are opening a data store checkpoint, our "current" dhandle in the
     * session is the data store checkpoint, hs_dhandle is the matching history store checkpoint,
     * and ckpt_snapshot contains the snapshot and timestamp data. It is at least theoretically
     * possible for hs_dhandle to be null; this means there is no corresponding history store
     * checkpoint. In this case we will avoid trying to open it later.
     *
     * We keep the history store checkpoint dhandle in the checkpoint cursor, and hold it open as
     * long as the checkpoint cursor remains open. It is never directly used, but it ensures that
     * the history store checkpoint will not be removed under us and any history store lookups done
     * via the checkpoint cursor (which open the history store separately themselves) will be able
     * to open the right version of the history store. This is essential for unnamed checkpoints as
     * they turn over frequently and asynchronously. It is, strictly speaking, not necessary for
     * named checkpoints, because as long as a named checkpoint data store cursor is open that named
     * checkpoint cannot be changed. However, making the behavior conditional would introduce
     * substantial interface complexity to little benefit.
     *
     * 2. When we get here, if we are opening a history store checkpoint, our "current" dhandle in
     * the session is the history store checkpoint, hs_dhandle is null, and ckpt_snapshot contains
     * the checkpoint's snapshot and timestamp information.
     *
     * If we are opening a history store checkpoint directly from the application (normally the
     * application should never do this, but one or two tests do) we will get snapshot information
     * matching the checkpoint. If we are opening a history store checkpoint internally, as part of
     * an operation on a data store checkpoint cursor, we will have explicitly opened the right
     * history store checkpoint. The snapshot information may be from a newer checkpoint, but will
     * not be used.
     *
     * 3. To make visibility checks work correctly relative to the checkpoint snapshot, we concoct a
     * dummy transaction and load the snapshot data into it. This transaction lives in the
     * checkpoint cursor. It is substituted into session->txn during checkpoint cursor operations.
     * Note that we do _not_ substitute into txn_shared, so using a checkpoint cursor does not cause
     * interactions with other threads and in particular does not affect the pinned timestamp
     * computation. The read timestamp associated with the checkpoint is kept in the dummy
     * transaction, and there's a (single) special case in the visibility code to check it instead
     * of the normal read timestamp in txn_shared.
     *
     * Global visibility checks that can occur during checkpoint cursor operations need to be
     * special-cased, because global visibility checks against the current world and not the
     * checkpoint. There are only a few of these and it seemed more effective to conditionalize them
     * directly rather than tinkering with the visibility code itself.
     *
     * 4. We do not substitute into session->txn if we are already in a checkpoint cursor (that is,
     * if session->txn is a checkpoint cursor dummy transaction) -- this happens when doing history
     * store accesses within a data store operation, and means that the history store accesses use
     * the same snapshot and timestamp information as the data store accesses, which is important
     * for consistency.
     *
     * 5. Because the checkpoint cursor in use is not itself visible in various parts of the
     * system (most notably the history store code) anything else we need to know about
     * elsewhere also gets substituted into the session at this point. Currently the only such item
     * is the name for the matching history store checkpoint.
     *
     * 6. When accessing the history store, we will use the history store checkpoint name stashed in
     * the session if there is one.
     */

    /* We may have gotten a history store handle, but not if we're the history store. */
    WT_ASSERT(session, !WT_IS_HS(session->dhandle) || hs_dhandle == NULL);

    /* We should always have snapshot data, though it might be degenerate. */
    WT_ASSERT(session, ckpt_snapshot != NULL);

    /*
     * Remember the write generation so we can use it in preference to the btree's own write
     * generation. This comes into play when the btree-level checkpoint is from an earlier run than
     * the global checkpoint metadata: the unpack code hides old transaction ids, and we need to
     * have it show us exactly the transaction ids that correspond to the snapshot we're using. The
     * write generation we get might be 0 if the global checkpoint is old and didn't contain the
     * information; in that case we'll ignore it.
     */
    cbt->checkpoint_write_gen = ckpt_snapshot->snapshot_write_gen;

    /* Remember the checkpoint ID so it can be returned to the application. */
    cbt->checkpoint_id = ckpt_snapshot->ckpt_id;

    /*
     * Override the read timestamp if explicitly provided. Otherwise it's the stable timestamp from
     * the checkpoint. Replace it in the snapshot info if necessary.
     */
    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.checkpoint_read_timestamp", 0, &cval), true);
    if (ret == 0) {
        if (cval.len > 0 && cval.val == 0)
            /*
             * Allow setting "0" explicitly to mean "none". Otherwise 0 is rejected by the timestamp
             * parser. Note that the default is not "none", it is the checkpoint's stable timestamp.
             */
            ckpt_snapshot->stable_ts = WT_TXN_NONE;
        else if (cval.val != 0) {
            WT_ERR(__wt_txn_parse_timestamp(
              session, "checkpoint read timestamp", &ckpt_snapshot->stable_ts, &cval));
            /*
             * Fail if the read timestamp is less than checkpoint's oldest timestamp. Since this is
             * a debug setting it's not super critical to make it a usable interface, and for
             * testing it's usually more illuminating to fail if something unexpected happens. If we
             * end up exposing the checkpoint read timestamp, it might be better to have this always
             * round up instead, since there's no useful way for the application to get the
             * checkpoint's oldest timestamp itself.
             */
            if (ckpt_snapshot->stable_ts < ckpt_snapshot->oldest_ts)
                WT_ERR_MSG(session, EINVAL,
                  "checkpoint_read_timestamp must not be before the checkpoint oldest timestamp");
        }
    }

    /*
     * Always create the dummy transaction. If we're opening the history store from inside a data
     * store checkpoint cursor, we'll end up not using it, but we can't easily tell from here
     * whether that's the case. Pass in the snapshot info.
     */
    WT_ERR(__wt_txn_init_checkpoint_cursor(session, ckpt_snapshot, &cbt->checkpoint_txn));

    /*
     * Stow the history store handle on success. (It will be released further up the call chain if
     * we fail.)
     */
    WT_ASSERT(session, ret == 0);
    cbt->checkpoint_hs_dhandle = hs_dhandle;

err:
    return (ret);
}

/*
 * __curfile_bound --
 *     WT_CURSOR->bound implementation for file cursors.
 */
static int
__curfile_bound(WT_CURSOR *cursor, const char *config)
{
    WT_COLLATOR *btree_collator;
    WT_CONFIG_ITEM cval;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_CONF(WT_CURSOR, bound, conf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = (WT_CURSOR_BTREE *)cursor;

    CURSOR_API_CALL(cursor, session, ret, bound, NULL);

    WT_ERR(__wt_conf_compile_api_call(session, WT_CONFIG_REF(session, WT_CURSOR_bound),
      WT_CONFIG_ENTRY_WT_CURSOR_bound, config, &_conf, sizeof(_conf), &conf));

    if (CUR2BT(cursor)->type == BTREE_COL_FIX)
        WT_ERR_MSG(session, EINVAL, "setting bounds is not compatible with fixed column store");

    /* It is illegal to set a bound on a positioned cursor (it's fine to clear one) */
    WT_ERR(__wt_conf_gets(session, conf, action, &cval));
    if (WT_CONF_STRING_MATCH(set, cval) && WT_CURSOR_IS_POSITIONED(cbt))
        WT_ERR_MSG(session, EINVAL, "setting bounds on a positioned cursor is not allowed");
    btree_collator = CUR2BT(cursor)->collator;

    WT_ERR(__wti_cursor_bound(cursor, conf, btree_collator));
err:
    API_END_RET_STAT(session, ret, cursor_bound);
}

/*
 * __curfile_largest_key --
 *     WT_CURSOR->largest_key default implementation..
 */
static int
__curfile_largest_key(WT_CURSOR *cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool key_only;

    cbt = (WT_CURSOR_BTREE *)cursor;
    key_only = F_ISSET(cursor, WT_CURSTD_KEY_ONLY);
    CURSOR_API_CALL(cursor, session, ret, largest_key, cbt->dhandle);

    if (WT_CURSOR_BOUNDS_SET(cursor))
        WT_ERR_MSG(session, EINVAL, "setting bounds is not compatible with cursor largest key");

    WT_ERR(__wt_scr_alloc(session, 0, &key));

    /* Reset the cursor to give up the cursor position. */
    WT_ERR(cursor->reset(cursor));

    /* Set the flag to bypass value read. */
    F_SET(cursor, WT_CURSTD_KEY_ONLY);

    /* Call btree cursor prev to get the largest key. */
    WT_WITH_CHECKPOINT(session, cbt, ret = __wt_btcur_prev(cbt, false));
    WT_ERR(ret);

    /* Copy the key as we will reset the cursor after that. */
    WT_ERR(__wt_buf_set(session, key, cursor->key.data, cursor->key.size));
    WT_ERR(cursor->reset(cursor));
    WT_ERR(__wt_buf_set(session, &cursor->key, key->data, key->size));
    /* Set the key as external. */
    F_SET(cursor, WT_CURSTD_KEY_EXT);

err:
    if (!key_only)
        F_CLR(cursor, WT_CURSTD_KEY_ONLY);
    __wt_scr_free(session, &key);
    if (ret != 0)
        WT_TRET(cursor->reset(cursor));
    API_END_RET_STAT(session, ret, cursor_largest_key);
}

/*
 * __curfile_create --
 *     Open a cursor for a given btree handle.
 */
static int
__curfile_create(WT_SESSION_IMPL *session, WT_CURSOR *owner, const char *cfg[], bool bulk,
  bool bitmap, WT_DATA_HANDLE *hs_dhandle, WT_CKPT_SNAPSHOT *ckpt_snapshot, WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __curfile_compare,                              /* compare */
      __curfile_equals,                               /* equals */
      __curfile_next,                                 /* next */
      __curfile_prev,                                 /* prev */
      __curfile_reset,                                /* reset */
      __curfile_search,                               /* search */
      __wti_curfile_search_near,                      /* search-near */
      __curfile_insert,                               /* insert */
      __wti_cursor_modify_value_format_notsup,        /* modify */
      __curfile_update,                               /* update */
      __curfile_remove,                               /* remove */
      __curfile_reserve,                              /* reserve */
      __wti_cursor_reconfigure,                       /* reconfigure */
      __curfile_largest_key,                          /* largest_key */
      __curfile_bound,                                /* bound */
      __curfile_cache,                                /* cache */
      __curfile_reopen,                               /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curfile_close);                               /* close */
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_BULK *cbulk;
    WT_DECL_RET;
    size_t csize;
    bool cacheable;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_BTREE);

    btree = S2BT(session);
    WT_ASSERT(session, btree != NULL);

    csize = bulk ? sizeof(WT_CURSOR_BULK) : sizeof(WT_CURSOR_BTREE);
    cacheable = F_ISSET(session, WT_SESSION_CACHE_CURSORS) && !bulk;

    WT_RET(__wt_calloc(session, 1, csize, &cbt));
    cursor = &cbt->iface;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->internal_uri = btree->dhandle->name;
    cursor->key_format = btree->key_format;
    cursor->value_format = btree->value_format;
    cbt->dhandle = session->dhandle;

    /*
     * Increment the data-source's in-use counter; done now because closing the cursor will
     * decrement it, and all failure paths from here close the cursor.
     */
    __wt_cursor_dhandle_incr_use(session);

    /*
     * We should have already set up the checkpoint cursor snapshot to read the history store unless
     * we are reading the history store checkpoint cursor directly. Check whether we are already in
     * a checkpoint cursor transaction.
     */
    if (!F_ISSET(session->txn, WT_TXN_IS_CHECKPOINT) && WT_READING_CHECKPOINT(session)) {
        /* Checkpoint cursor. */
        if (bulk)
            /* Fail now; otherwise we fail further down and then segfault trying to recover. */
            WT_RET_MSG(session, EINVAL, "checkpoints are read-only and cannot be bulk-loaded");
        WT_RET(__curfile_setup_checkpoint(cbt, cfg, hs_dhandle, ckpt_snapshot));
    } else {
        /* We should not have been given the bits used by checkpoint cursors. */
        WT_ASSERT(session, hs_dhandle == NULL);
        WT_ASSERT(session, ckpt_snapshot->snapshot_txns == NULL);
    }

    if (bulk) {
        F_SET(cursor, WT_CURSTD_BULK);

        cbulk = (WT_CURSOR_BULK *)cbt;

        /* Optionally skip the validation of each bulk-loaded key. */
        WT_ERR(__wt_config_gets_def(session, cfg, "skip_sort_check", 0, &cval));
        WT_ERR(__wti_curbulk_init(session, cbulk, bitmap, cval.val == 0 ? 0 : 1));
    }

    /*
     * Random retrieval, row-store only. Random retrieval cursors support a limited set of methods.
     */
    WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0) {
        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_seed", 0, &cval));
        if (cval.val != 0)
            __wt_random_init_seed(&cbt->rnd, (uint64_t)cval.val);
        else
            __wt_random_init(session, &cbt->rnd);

        if (WT_CURSOR_RECNO(cursor))
            WT_ERR_MSG(
              session, ENOTSUP, "next_random configuration not supported for column-store objects");

        __wti_cursor_set_notsup(cursor);
        cursor->next = __wti_curfile_next_random;
        cursor->reset = __curfile_reset;

        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_sample_size", 0, &cval));
        if (cval.val != 0)
            cbt->next_random_sample_size = (u_int)cval.val;
        cacheable = false;
    }

    WT_ERR(__wt_config_gets_def(session, cfg, "read_once", 0, &cval));
    if (cval.val != 0)
        F_SET(cbt, WT_CBT_READ_ONCE);

    /* Underlying btree initialization. */
    __wt_btcur_open(cbt);

    /*
     * WT_CURSOR.modify supported on 'S' and 'u' value formats, but the fast-path through the btree
     * code requires log file format changes, it's not available in all versions.
     */
    if ((WT_STREQ(cursor->value_format, "S") || WT_STREQ(cursor->value_format, "u")) &&
      __wt_version_gte(S2C(session)->compat_version, WT_LOG_V2_VERSION))
        cursor->modify = __curfile_modify;

    /* Cursors on metadata should not be cached, doing so interferes with named checkpoints. */
    if (cacheable && strcmp(WT_METAFILE_URI, cursor->internal_uri) != 0)
        F_SET(cursor, WT_CURSTD_CACHEABLE);

    WT_ERR(__wt_cursor_init(cursor, cursor->internal_uri, owner, cfg, cursorp));

    WT_STAT_CONN_DSRC_INCR(session, cursor_create);

    if (0) {
err:
        __wt_cursor_dhandle_decr_use(session);

        /*
         * Our caller expects to release the data handles if we fail. Disconnect both the main and
         * any history store handle from the cursor before closing.
         */
        cbt->dhandle = NULL;
        cbt->checkpoint_hs_dhandle = NULL;

        WT_TRET(__curfile_close(cursor));
        *cursorp = NULL;
    }

    if (ret == 0 && bulk)
        WT_STAT_CONN_INCR_ATOMIC(session, cursor_bulk_count);

    return (ret);
}

/*
 * __wt_curfile_open --
 *     WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_curfile_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CKPT_SNAPSHOT ckpt_snapshot;
    WT_CONFIG_ITEM cval;
    WT_DATA_HANDLE *hs_dhandle;
    WT_DECL_RET;
    uint32_t flags;
    bool bitmap, bulk, checkpoint_use_history, checkpoint_wait;

    hs_dhandle = NULL;
    bitmap = bulk = false;
    checkpoint_wait = true;
    flags = 0;

    WT_ASSERT(session, WT_BTREE_PREFIX(uri));

    /*
     * Decode the bulk configuration settings. In memory databases ignore bulk load.
     */
    if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY)) {
        WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
        if (cval.type == WT_CONFIG_ITEM_BOOL ||
          (cval.type == WT_CONFIG_ITEM_NUM && (cval.val == 0 || cval.val == 1))) {
            bitmap = false;
            bulk = cval.val != 0;
        } else if (WT_CONFIG_LIT_MATCH("bitmap", cval))
            bitmap = bulk = true;
        /*
         * Unordered bulk insert is a special case used internally by index creation on existing
         * tables. It doesn't enforce any special semantics at the file level. It primarily exists
         * to avoid some locking problems between LSM and index creation.
         */
        else if (!WT_CONFIG_LIT_MATCH("unordered", cval))
            WT_RET_MSG(session, EINVAL, "Value for 'bulk' must be a boolean or 'bitmap'");

        if (bulk) {
            if (F_ISSET(session->txn, WT_TXN_RUNNING))
                WT_RET_MSG(session, EINVAL, "Bulk cursors can't be opened inside a transaction");

            WT_RET(__wt_config_gets(session, cfg, "checkpoint_wait", &cval));
            checkpoint_wait = cval.val != 0;
        }
    }

    /* Bulk handles require exclusive access. */
    if (bulk)
        LF_SET(WT_BTREE_BULK | WT_DHANDLE_EXCLUSIVE);

    /* Find out if we're supposed to avoid opening the history store. */
    WT_RET(__wt_config_gets_def(session, cfg, "checkpoint_use_history", 1, &cval));
    checkpoint_use_history = cval.val != 0;

    /*
     * This open path is used for checkpoint cursors and bulk cursors as well as ordinary cursors.
     * Several considerations apply as a result.
     *
     * 1. For bulk cursors we need to do an exclusive open. In this case, a running database-wide
     * checkpoint can result in EBUSY. To avoid this, we can take the checkpoint lock while opening
     * the dhandle, which causes us to block until any running checkpoint finishes. This is
     * controlled by the "checkpoint_wait" config. Nothing else does an exclusive open, so the path
     * with the checkpoint lock is not otherwise reachable.
     *
     * 2. Historically, for checkpoint cursors, it was not safe to take the checkpoint lock here,
     * because previously the LSM code would open a checkpoint cursor while holding the schema lock.
     * The checkpoint lock is supposed to come before the schema lock which meant the ordering would
     * be backwards. It is now possible although unimplemented to do an exclusive open of a
     * checkpoint cursor if there is a good reason for it.
     *
     * 3. If we are opening a checkpoint cursor, we need two dhandles, one for the tree we're
     * actually trying to open and (unless that's itself the history store) one for the history
     * store, and also a copy of the snapshot and timestamp metadata for the checkpoint. It's
     * necessary for data correctness for all three of these to match. There's a complicated scheme
     * for getting a matching set while avoiding races with a running checkpoint inside the open
     * logic (see session_dhandle.c) that we fortunately don't need to think about here.
     *
     * 4. To avoid a proliferation of cases, and to avoid repeatedly parsing config strings, we
     * always pass down the return arguments for the history store dhandle and checkpoint snapshot
     * information (except for the bulk-only case and the LSM case) and pass the results on to
     * __curfile_create. We will not get anything back unless we are actually opening a checkpoint
     * cursor. The open code takes care of the special case of opening a checkpoint cursor on the
     * history store. (This is not normally done by applications; but it is done by a couple tests,
     * and furthermore any internally opened history store cursors come through here, so this case
     * does matter.)
     *
     * This initialization is repeated when opening the underlying data handle, which is ugly, but
     * cleanup requires the initialization have happened even if not opening a checkpoint handle.
     */
    __wt_checkpoint_snapshot_clear(&ckpt_snapshot);

    /* Get the handle and lock it while the cursor is using it. */
    if (LF_ISSET(WT_DHANDLE_EXCLUSIVE) && checkpoint_wait)
        WT_WITH_CHECKPOINT_LOCK(
          session, ret = __wt_session_get_btree_ckpt(session, uri, cfg, flags, NULL, NULL));
    else if (checkpoint_use_history)
        ret = __wt_session_get_btree_ckpt(session, uri, cfg, flags, &hs_dhandle, &ckpt_snapshot);
    else
        ret = __wt_session_get_btree_ckpt(session, uri, cfg, flags, NULL, NULL);
    WT_RET(ret);

    WT_ERR(
      __curfile_create(session, owner, cfg, bulk, bitmap, hs_dhandle, &ckpt_snapshot, cursorp));

    return (0);

err:
    if (hs_dhandle != NULL)
        WT_WITH_DHANDLE(session, hs_dhandle, WT_TRET(__wt_session_release_dhandle(session)));

    /* If a snapshot array was returned and hasn't been moved elsewhere, discard it now. */
    __wt_free(session, ckpt_snapshot.snapshot_txns);

    /* If the cursor could not be opened, release the handle. */
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}
