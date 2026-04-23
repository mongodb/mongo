/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __clayered_copy_bounds(WT_CURSOR_LAYERED *);
static int __clayered_lookup(WT_SESSION_IMPL *, WT_CURSOR_LAYERED *, WT_ITEM *);
static int __clayered_open_cursors(WT_SESSION_IMPL *, WT_CURSOR_LAYERED *);
static int __clayered_reset_cursors(WT_CURSOR_LAYERED *, bool);
static int __clayered_search_near(WT_CURSOR *, int *);
static int __clayered_adjust_state(WT_CURSOR_LAYERED *, bool, bool *);

/* Operations passed to __clayered_put. */
typedef enum {
    WT_CLAYERED_PUT_INSERT,
    WT_CLAYERED_PUT_UPDATE,
    WT_CLAYERED_PUT_RESERVE,
} WT_CLAYERED_PUT_OP;

/*
 * __clayered_is_deleted_encoded --
 *     Check if the value starts with the tombstone.
 */
static WT_INLINE bool
__clayered_is_deleted_encoded(const WT_ITEM *value)
{
    return (value->size > __wt_tombstone.size &&
      memcmp(value->data, __wt_tombstone.data, __wt_tombstone.size) == 0);
}

/*
 * __clayered_deleted_encode --
 *     Encode values that are in the encoded name space.
 */
static WT_INLINE int
__clayered_deleted_encode(
  WT_SESSION_IMPL *session, const WT_ITEM *value, WT_ITEM *final_value, WT_ITEM **tmpp)
{
    WT_ITEM *tmp;

    /*
     * If value requires encoding, get a scratch buffer of the right size and create a copy of the
     * data with the first byte of the tombstone appended.
     */
    if (__clayered_is_deleted_encoded(value)) {
        WT_RET(__wt_scr_alloc(session, value->size + 1, tmpp));
        tmp = *tmpp;

        memcpy(tmp->mem, value->data, value->size);
        memcpy((uint8_t *)tmp->mem + value->size, __wt_tombstone.data, 1);
        final_value->data = tmp->mem;
        final_value->size = value->size + 1;
    } else {
        final_value->data = value->data;
        final_value->size = value->size;
    }

    return (0);
}

/*
 * __clayered_deleted_decode --
 *     Decode values that start with the tombstone.
 */
static WT_INLINE void
__clayered_deleted_decode(WT_ITEM *value)
{
    if (__clayered_is_deleted_encoded(value))
        --value->size;
}

/*
 * __clayered_get_collator --
 *     Retrieve the collator for a layered cursor. Wrapped in a function, since in the future the
 *     collator might live in a constituent cursor instead of the handle.
 */
static void
__clayered_get_collator(WT_CURSOR_LAYERED *clayered, WT_COLLATOR **collatorp)
{
    *collatorp = ((WT_LAYERED_TABLE *)clayered->dhandle)->collator;
}

/*
 * __clayered_cursor_compare --
 *     Compare two constituent cursors in a layered tree
 */
static int
__clayered_cursor_compare(WT_CURSOR_LAYERED *clayered, WT_CURSOR *c1, WT_CURSOR *c2, int *cmpp)
{
    WT_COLLATOR *collator;
    WT_SESSION_IMPL *session;

    session = CUR2S(clayered);

    WT_ASSERT_ALWAYS(session, F_ISSET(c1, WT_CURSTD_KEY_SET) && F_ISSET(c2, WT_CURSTD_KEY_SET),
      "Can only compare cursors with keys available in layered tree");

    __clayered_get_collator(clayered, &collator);
    return (__wt_compare(session, collator, &c1->key, &c2->key, cmpp));
}

/*
 * __clayered_enter --
 *     Start an operation on a layered cursor.
 */
static WT_INLINE int
__clayered_enter(WT_CURSOR_LAYERED *clayered, bool reset, bool need_read_stable, bool iteration)
{
    WT_SESSION_IMPL *session;
    bool external_state_change;

    session = CUR2S(clayered);

    /* Query operations need a full set of cursors. */
    if (need_read_stable)
        F_SET(clayered, WT_CLAYERED_READ_STABLE);

    /*
     * FIXME-WT-15058: When inside a read committed isolation, the file cursor code expects to
     * release the snapshot when the count of active cursors is zero. Reset the constituent cursors
     * to adhere to that behavior. Ideally we should not be changing the active cursors counter
     * outside of the file cursor code.
     */
    if (reset && __wt_txn_read_committed_should_release_snapshot(session)) {
        WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT));
        WT_RET(__clayered_reset_cursors(clayered, false));
    }

    WT_RET(__clayered_adjust_state(clayered, iteration, &external_state_change));

    if (external_state_change || clayered->ingest_cursor == NULL ||
      (need_read_stable && clayered->stable_cursor == NULL &&
        clayered->checkpoint_meta_lsn != WT_DISAGG_LSN_NONE))
        WT_RET(__clayered_open_cursors(session, clayered));

    if (!F_ISSET(clayered, WT_CLAYERED_ACTIVE)) {
        /*
         * Opening this layered cursor has opened a number of btree cursors, ensure other code
         * doesn't think this is the first cursor in a session.
         */
        ++session->ncursors;
        WT_RET(__cursor_enter(session));
        F_SET(clayered, WT_CLAYERED_ACTIVE);
    }

    return (0);
}

/*
 * __clayered_leave --
 *     Finish an operation on a layered cursor.
 */
static void
__clayered_leave(WT_CURSOR_LAYERED *clayered)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(clayered);

    F_CLR(clayered, WT_CLAYERED_READ_STABLE);

    if (F_ISSET(clayered, WT_CLAYERED_ACTIVE)) {
        --session->ncursors;
        __cursor_leave(session);
        F_CLR(clayered, WT_CLAYERED_ACTIVE);
    }
}

/*
 * __clayered_close_cursors --
 *     Close any btree cursors that are not needed.
 */
static int
__clayered_close_cursors(WT_CURSOR_LAYERED *clayered)
{
    WT_CURSOR *c;

    clayered->current_cursor = NULL;
    if ((c = clayered->ingest_cursor) != NULL) {
        WT_RET(c->close(c));
        clayered->ingest_cursor = NULL;
    }
    if ((c = clayered->stable_cursor) != NULL) {
        WT_RET(c->close(c));
        clayered->stable_cursor = NULL;
    }

    /* Some flags persist across closes of constituents. */
    F_CLR(clayered, ~(WT_CLAYERED_ACTIVE | WT_CLAYERED_RANDOM));
    return (0);
}

/*
 * __clayered_configure_random --
 *     Make a configuration string that either empty or includes any random configuration as
 *     appropriate.
 */
static int
__clayered_configure_random(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, WT_ITEM *random_config)
{
    /*
     * If the layered cursor is configured with next_random, we'll need to open any constituent
     * cursors with the same configuration that is relevant for random cursors.
     */
    if (F_ISSET(clayered, WT_CLAYERED_RANDOM))
        WT_RET(__wt_buf_fmt(session, random_config,
          "next_random=true,next_random_seed=%" PRId64 ",next_random_sample_size=%" PRIu64,
          clayered->next_random_seed, (uint64_t)clayered->next_random_sample_size));

    return (0);
}

/*
 * __clayered_open_stable --
 *     Open the stable cursor using the given role.
 */
static int
__clayered_open_stable(WT_CURSOR_LAYERED *clayered, bool leader)
{
    WT_CURSOR *c;
    WT_DECL_ITEM(random_config);
    WT_DECL_ITEM(stable_uri_buf);
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered;
    WT_SESSION_IMPL *session;
    const char *cfg[4] = {WT_CONFIG_BASE(CUR2S(clayered), WT_SESSION_open_cursor), "", NULL, NULL};
    const char *checkpoint_name, *stable_uri;

    session = CUR2S(clayered);
    c = &clayered->iface;
    layered = (WT_LAYERED_TABLE *)clayered->dhandle;
    checkpoint_name = NULL;

    WT_RET(__wt_scr_alloc(session, 0, &random_config));
    /* Get the configuration for random cursors, if any. */
    WT_ERR(__clayered_configure_random(session, clayered, random_config));

    if (random_config->size > 0)
        cfg[1] = random_config->data;

retry:
    stable_uri = layered->stable_uri;
    if (!leader) {
        /*
         * We may have a stable chunk with no checkpoint yet. If that's the case then open a cursor
         * on stable without a checkpoint. It will never return an invalid result (it's content is
         * by definition trailing the ingest cursor). It is just slightly less efficient, and also
         * not an accurate reflection of what we want in terms of sharing checkpoints across
         * different WiredTiger instances eventually.
         */

        /* Look up the most recent data store checkpoint. This fetches the exact name to use. */
        WT_ERR_NOTFOUND_OK(
          __wt_meta_checkpoint_last_name(session, stable_uri, &checkpoint_name, NULL, NULL), true);

        if (ret == WT_NOTFOUND) {
            /*
             * We've never picked up a checkpoint, open a regular btree on the stable URI. If we're
             * a follower and we never picked up a checkpoint, then no checkpoint has ever occurred
             * on this Btree. Everything we need will be satisfied by the ingest table until the
             * next checkpoint is picked up. So technically, opening this (empty) stable table is
             * wasteful, but it's a corner case, it will be resolved at the next checkpoint, and it
             * keeps the code easy.
             *
             * FIXME-WT-16476: how to close this dhandle later as it is a live btree handle? We may
             * get this dhandle when the node steps up.
             */
            F_SET(clayered, WT_CLAYERED_STABLE_NO_CKPT);
        } else {
            if (stable_uri_buf == NULL)
                WT_ERR(__wt_scr_alloc(session, 0, &stable_uri_buf));
            /*
             * Use a URI with a "/<checkpoint name> suffix. This is interpreted as reading from the
             * stable checkpoint, but without it being a traditional checkpoint cursor.
             */
            WT_ERR(
              __wt_buf_fmt(session, stable_uri_buf, "%s/%s", layered->stable_uri, checkpoint_name));
            stable_uri = stable_uri_buf->data;
        }
        cfg[2] = "read_only=true";
    }

    ret = __wt_open_cursor(session, stable_uri, c, cfg, &clayered->stable_cursor);

    if (ret == EBUSY && !leader) {
        __wt_free(session, checkpoint_name);
        /* FIXME-WT-16476: no need to yield if we no longer take the checkpoint lock. */
        __wt_yield();
        goto retry;
    }

    /* Opening a cursor can return both of these, unfortunately. FIXME-WT-15816. */
    if ((ret == ENOENT || ret == WT_NOTFOUND) && !leader)
        /*
         * This is fine on followers, we simply may not have seen a checkpoint with this table yet.
         * Defer the open.
         */
        ret = 0;
    WT_ERR(ret);

    if (clayered->stable_cursor != NULL) {
        F_SET(clayered->stable_cursor, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);

        if (F_ISSET(c, WT_CURSTD_DEBUG_RESET_EVICT))
            F_SET(clayered->stable_cursor, WT_CURSTD_DEBUG_RESET_EVICT);
    }

err:
    __wt_scr_free(session, &random_config);
    __wt_scr_free(session, &stable_uri_buf);
    __wt_free(session, checkpoint_name);

    return (ret);
}

/*
 * __clayered_ingest_check_close --
 *     Return true if the ingest cursor need to be reopened.
 */
static bool
__clayered_ingest_check_close(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered)
{
    /* See if there's nothing to do for the ingest cursor. */
    if (clayered->ingest_cursor == NULL)
        return (false);

    bool leader = S2C(session)->layered_table_manager.leader;
    /*
     * Layered cursor is positioned on the ingest cursor. Changing it may lose the layered cursor
     * position.
     */
    if (F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) &&
      clayered->current_cursor == clayered->ingest_cursor) {
        /* This should not happen on the leader at the moment. */
        WT_ASSERT(session, !leader);
        return (false);
    }

    /* For the ingest table, we'll need to close it or open it. Either way it's a change. */
    if (leader == clayered->leader)
        return (false);

    return (true);
}

/*
 * __clayered_can_advance_stable --
 *     Return true if the stable cursor can be advanced to a newer checkpoint at this time.
 */
static bool
__clayered_can_advance_stable(WT_CURSOR_LAYERED *clayered, bool iteration)
{
    WT_SESSION_IMPL *session;
    WT_TXN_SHARED *txn_shared;

    session = CUR2S(clayered);

    /* A random stable cursor shouldn't be reopened, it may have additional state. */
    if (clayered->stable_cursor == NULL || F_ISSET(clayered, WT_CLAYERED_RANDOM))
        return (false);

    /*
     * Layered cursor is positioned on the stable cursor. Changing it may lose the layered cursor
     * position. FIXME-WT-16467: If we are reading with a timestamp and can ensure that we never
     * select a checkpoint with an oldest timestamp greater than the pinned timestamp, we should
     * safely advance to a newer checkpoint. This is because the version we intend to read would
     * still be present in the newer checkpoint.
     */
    if (F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) &&
      clayered->current_cursor == clayered->stable_cursor)
        return (false);

    /*
     * First, layered cursors are sometimes paired with read timestamps. When using read
     timestamps,
     * it's always safe to update cursors, even during iterations. That's because the view at a
     * timestamp is always consistent, the history store covers that.
     */
    txn_shared = WT_SESSION_TXN_SHARED(session);
    if (txn_shared != NULL && txn_shared->read_timestamp != WT_TS_NONE)
        return (true);
    else {
        /* if this is an iteration, we won't reopen the cursor, we're done. */
        if (iteration)
            return (false);

        /*
         * There are other points when it is appropriate to update cursors. If we don't currently
         * have a transactional snapshot, or if the snapshot has changed, we can update.
         *
         * Why shouldn't we update when in a transaction? We may have read some values, and we'd
         * expect to see the same values if we read them again. Reading from a newer checkpoint
         can
         * violate that.
         */
        if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT) ||
          (__wt_session_gen(session, WT_GEN_HAS_SNAPSHOT) != clayered->snapshot_gen))
            return (true);
    }

    return (false);
}

/*
 * __clayered_advance_stable --
 *     Advance the stable cursor to a newer checkpoint.
 */
static int
__clayered_advance_stable(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, bool current_leader)
{
    WT_CURSOR *old_stable;
    WT_DECL_RET;

    /*
     * We can't just close the stable cursor here, as we need to retain any position that the
     * current stable cursor has. It's easier to keep the old cursor open briefly while we copy the
     * position.
     */
    old_stable = clayered->stable_cursor;
    clayered->stable_cursor = NULL;

    WT_ERR(__clayered_open_stable(clayered, current_leader));
    WT_ASSERT(session, clayered->stable_cursor != NULL);

    /*
     * If the old cursor has a position, copy it to the newly opened cursor. Prepared updates are
     * always ignored on the stable cursor, making it safe to check the WT_CURSTD_KEY_INT flag.
     */
    if (F_ISSET(old_stable, WT_CURSTD_KEY_INT)) {
        WT_ERR_NOTFOUND_OK(__wt_cursor_dup_position(old_stable, clayered->stable_cursor), true);
        /*
         * If the key is removed from the new checkpoint, the layered cursor must be positioned on
         * the ingest table.
         */
        WT_ASSERT_ALWAYS(session,
          ret == 0 || !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) ||
            clayered->current_cursor == clayered->ingest_cursor,
          "upgrading a positioned stable cursor");
        /*
         * If the key is removed in the new checkpoint, clear the iteration flag to reposition it to
         * the correct location.
         */
        if (ret == WT_NOTFOUND)
            F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    } else if (F_ISSET(old_stable, WT_CURSTD_KEY_EXT)) {
        WT_ITEM_SET(clayered->stable_cursor->key, old_stable->key);
        if (F_ISSET(old_stable, WT_CURSTD_VALUE_EXT))
            WT_ITEM_SET(clayered->stable_cursor->value, old_stable->value);
    }

    /* Add any bounds for the new cursor. */
    WT_ERR(__clayered_copy_bounds(clayered));

    if (clayered->current_cursor == old_stable) {
        WT_CURSOR *cursor = (WT_CURSOR *)clayered;
        WT_CURSOR *new_stable = clayered->stable_cursor;
        if (F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
            /* Reset the cursor key to point to the new stable cursor. */
            WT_ITEM_SET(cursor->key, new_stable->key);
            /* Clear the value as the new stable cursor may point to a different one. */
            F_CLR(cursor, WT_CURSTD_VALUE_INT);
        }
        clayered->current_cursor = new_stable;
    }

err:
    if (ret == 0) {
        /* Close the old cursor. */
        WT_TRET(old_stable->close(old_stable));
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_advance_stable);
    } else {
        /* Give up the advancement if we fail. */
        if (clayered->stable_cursor != NULL)
            WT_TRET(clayered->stable_cursor->close(clayered->stable_cursor));
        clayered->stable_cursor = old_stable;
    }

    return (ret);
}

/*
 * __clayered_adjust_state --
 *     Update the state of the cursor to match the state of the disaggregated system. In particular,
 *     if the system has changed in a way that makes constituent cursors out of date, either reopen
 *     them or close them, and let them be opened later as needed.
 */
static int
__clayered_adjust_state(WT_CURSOR_LAYERED *clayered, bool iteration, bool *state_updated)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    uint64_t last_checkpoint_meta_lsn, snapshot_gen;
    bool change_ingest, change_stable, current_leader;

    *state_updated = false;
    session = CUR2S(clayered);
    conn = S2C(session);
    current_leader = conn->layered_table_manager.leader;

    /* Get the current checkpoint LSN. This only matters if we are a follower. */
    if (!current_leader)
        last_checkpoint_meta_lsn =
          __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);
    else
        last_checkpoint_meta_lsn = WT_DISAGG_LSN_NONE;

    /*
     * Has any state changed? What is not checked here is the possibility that a step down and
     step
     * up have both occurred since the last check. We don't have a way to detect that (or its
     * opposite) at the moment. If we did, we'd want to issue a rollback if the stable cursor has
     * any changes. FIXME-WT-14545.
     */
    if (current_leader == clayered->leader &&
      last_checkpoint_meta_lsn == clayered->checkpoint_meta_lsn)
        return (0);

    snapshot_gen = clayered->snapshot_gen;

    /* Is this a step up or step down? */
    if (current_leader != clayered->leader) {
        /*
         * If we're stepping down, then we currently have a R/W stable cursor and all writes
         would
         * go to it. Any writes we were about to make or have made to this table could never be
         * committed at this point.
         */
        if (!current_leader && session->txn->mod_count != 0) {
            __wt_txn_err_set(session, WT_ROLLBACK);
            /* Write operations are not allowed after stepping down from leader role. */
            WT_RET(WT_ROLLBACK);
        }

        /*
         * It turns out that the right choice for step up and step down is always to reopen the
         * stable cursor whenever we can.
         *
         * For step up, we're currently using a readonly stable cursor at a checkpoint. We can
         * reopen the stable cursor, we'd get a R/W cursor. We don't need the ability to write,
         as
         * this request was kicked off on the follower, so it must be all reads. But we want to
         * discard the stable cursor when we can, as long as we're not breaking transactional
         * semantics for cursors.
         *
         * For step down, we're currently using a R/W stable cursor. After the check above, we
         know
         * we've done read operations to this point. So again, we should reopen if we can.
         */
    }

    if ((change_ingest = __clayered_ingest_check_close(session, clayered))) {
        /*
         * To reopen the ingest table, all we need to do here is close it. It will be reopened
         when
         * needed. There's never a situation where we need to save its position.
         */
        WT_RET(clayered->ingest_cursor->close(clayered->ingest_cursor));
        if (clayered->current_cursor == clayered->ingest_cursor)
            clayered->current_cursor = NULL;
        clayered->ingest_cursor = NULL;
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_reopen_ingest);
    }

    /*
     * Even if the leader hasn't changed, we can get here if we have a new checkpoint on the
     * follower. And again, we'd like to reopen the stable cursor if we can.
     */
    if ((change_stable = __clayered_can_advance_stable(clayered, iteration))) {
        snapshot_gen = __wt_session_gen(session, WT_GEN_HAS_SNAPSHOT);
        WT_RET(__clayered_advance_stable(session, clayered, current_leader));
    }

    /* Update the state of the layered cursor. */
    clayered->leader = current_leader;
    clayered->checkpoint_meta_lsn = last_checkpoint_meta_lsn;
    clayered->snapshot_gen = snapshot_gen;
    *state_updated = (change_ingest || change_stable);

    return (0);
}

/*
 * __clayered_open_ingest --
 *     Open an ingest cursor.
 */
static int
__clayered_open_ingest(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, WT_CURSOR **cursorp)
{
    WT_CURSOR *c, *cursor;
    WT_DECL_ITEM(random_config);
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered;
    const char *ckpt_cfg[3] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "", NULL};

    c = &clayered->iface;
    layered = (WT_LAYERED_TABLE *)clayered->dhandle;

    WT_RET(__wt_scr_alloc(session, 0, &random_config));
    /* Get the configuration for random cursors, if any. */
    WT_ERR(__clayered_configure_random(session, clayered, random_config));
    if (random_config->size > 0)
        ckpt_cfg[1] = random_config->data;

    WT_ERR(__wt_open_cursor(session, layered->ingest_uri, c, ckpt_cfg, &cursor));
    F_SET(cursor, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);

    if (F_ISSET(c, WT_CURSTD_DEBUG_RESET_EVICT))
        F_SET(cursor, WT_CURSTD_DEBUG_RESET_EVICT);

    *cursorp = cursor;

err:
    __wt_scr_free(session, &random_config);
    return (ret);
}

/*
 * __clayered_open_cursors --
 *     Open cursors for the current set of files.
 */
static int
__clayered_open_cursors(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered)
{
    WT_CONNECTION_IMPL *conn;
    bool leader;

    conn = S2C(session);

    if (clayered->ingest_cursor != NULL && clayered->stable_cursor != NULL)
        return (0);

    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);

    /* Always open both the ingest and stable cursors */
    if (clayered->ingest_cursor == NULL)
        WT_RET(__clayered_open_ingest(session, clayered, &clayered->ingest_cursor));

    if (F_ISSET(clayered, WT_CLAYERED_READ_STABLE) && clayered->stable_cursor == NULL) {
        leader = conn->layered_table_manager.leader;
        WT_RET(__clayered_open_stable(clayered, leader));
    }

    if (F_ISSET(clayered, WT_CLAYERED_RANDOM)) {
        /*
         * Cursors configured with next_random only allow the next method to be called. But our
         * implementation of random requires search_near to be called on the two constituent
         * cursors, so explicitly allow that here.
         */
        WT_ASSERT(session, WT_PREFIX_MATCH(clayered->ingest_cursor->uri, "file:"));
        clayered->ingest_cursor->search_near = __wti_curfile_search_near;

        /*
         * If the stable cursor is not set, and we've succeeded to this point, that means we've
         * deferred opening the stable cursor.
         */
        if (clayered->stable_cursor != NULL) {
            WT_ASSERT(session, WT_PREFIX_MATCH(clayered->stable_cursor->uri, "file:"));
            clayered->stable_cursor->search_near = __wti_curfile_search_near;
        }
    }

    /*
     * Set any boundaries for any newly opened cursors.
     */
    WT_RET(__clayered_copy_bounds(clayered));

    return (0);
}

/*
 * __clayered_get_current --
 *     Find the smallest / largest of the cursors and copy its key/value.
 */
static int
__clayered_get_current(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, bool smallest)
{
    WT_COLLATOR *collator;
    WT_CURSOR *current;
    int cmp;
    bool ingest_positioned, stable_positioned;

    current = NULL;
    ingest_positioned = stable_positioned = false;

    /*
     * There are a couple of cases to deal with here: Some cursors don't have both ingest and stable
     * cursors. Some cursor positioning operations will only have one positioned cursor (e.g a walk
     * has exhausted one cursor but not the other).
     */
    if (clayered->ingest_cursor != NULL && F_ISSET(clayered->ingest_cursor, WT_CURSTD_KEY_INT))
        ingest_positioned = true;

    /*
     * FIXME-WT-16810: In leader mode, skip searching ingest as it should be empty. This will need
     * revisiting when asynchronous step-up is supported, because ingest may legitimately contain
     * data for some time after promotion.
     */
    if (clayered->leader)
        ingest_positioned = false;

    if (clayered->stable_cursor != NULL && F_ISSET(clayered->stable_cursor, WT_CURSTD_KEY_INT))
        stable_positioned = true;

    if (!ingest_positioned && !stable_positioned) {
        clayered->current_cursor = NULL;
        return (WT_NOTFOUND);
    }

    __clayered_get_collator(clayered, &collator);

    if (ingest_positioned && stable_positioned) {
        WT_RET(__wt_compare(
          session, collator, &clayered->ingest_cursor->key, &clayered->stable_cursor->key, &cmp));
        if (smallest ? cmp < 0 : cmp > 0)
            current = clayered->ingest_cursor;
        else if (cmp == 0)
            current = clayered->ingest_cursor;
        else
            current = clayered->stable_cursor;
    } else if (ingest_positioned)
        current = clayered->ingest_cursor;
    else if (stable_positioned)
        current = clayered->stable_cursor;

    clayered->current_cursor = current;

    return (0);
}

/*
 * __clayered_compare --
 *     WT_CURSOR->compare implementation for the layered cursor type.
 */
static int
__clayered_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_COLLATOR *collator;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /* There's no need to sync with the layered tree, avoid layered enter. */
    clayered = (WT_CURSOR_LAYERED *)a;
    CURSOR_API_CALL(a, session, ret, compare, clayered->dhandle);

    /*
     * Confirm both cursors refer to the same source and have keys, then compare the keys.
     */
    if (strcmp(a->internal_uri, b->internal_uri) != 0)
        WT_ERR_MSG(session, EINVAL, "comparison method cursors must reference the same object");

    /* Both cursors are from the same tree - they share the same collator */
    __clayered_get_collator(clayered, &collator);

    WT_ERR(__wt_compare(session, collator, &a->key, &b->key, cmpp));

err:
    API_END_RET(session, ret);
}

/*
 * __clayered_leader_reposition_iterate --
 *     Detect if the stable cursor position has been truncated. If so, position the cursor to
 *     next/prev visible position.
 */
static int
__clayered_leader_reposition_iterate(WT_CURSOR_LAYERED *clayered, WT_CURSOR *stable, bool forward)
{
    WT_COLLATOR *collator;
    WT_DECL_RET;
    int cmp;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_TRUNCATE *t;

    if (!__wt_process.disagg_fast_truncate_2026)
        return (0);

    /* Fast truncate only works under snapshot isolation. */
    WT_ASSERT(session, session->txn->isolation == WT_ISO_SNAPSHOT);
    __clayered_get_collator(clayered, &collator);

    /*
     * There could be overlapping truncates in the layered table truncate list. So we need to loop
     * until we find a non-truncated key or reach the end of the range.
     */
    for (;;) {
        ret = __wt_truncate_delete_visible_check(
          session, (WT_LAYERED_TABLE *)clayered->dhandle, &stable->key, &t);
        if (ret == WT_NOTFOUND)
            break;

        stable->set_key(stable, forward ? &t->stop_key : &t->start_key);
        WT_RET(stable->search_near(stable, &cmp));

        while (forward ? cmp < 0 : cmp > 0) {
            /*
             * The cursor next()/prev() could return back WT_NOTFOUND, meaning we have reached the
             * end of the table.
             */
            WT_RET(forward ? stable->next(stable) : stable->prev(stable));

            WT_RET(__wt_compare(
              session, collator, &stable->key, forward ? &t->stop_key : &t->start_key, &cmp));
        }
    }
    return (0);
}

/*
 * __wt_layered_truncate --
 *     Discard a cursor range from the layered table.
 */
int
__wt_layered_truncate(WT_TRUNCATE_INFO *trunc_info)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    const char *uri;

    session = trunc_info->session;
    uri = trunc_info->uri;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    WT_CURSOR_LAYERED *clayered_start = (WT_CURSOR_LAYERED *)trunc_info->start;
    WT_CURSOR_LAYERED *clayered_stop = (WT_CURSOR_LAYERED *)trunc_info->stop;

    /*
     * On leader mode, we can directly perform truncate operation on the stable table. On follower
     * mode, we need to perform truncate on the ingest table and add an entry inside the truncate
     * list.
     */
    if (S2C(session)->layered_table_manager.leader) {
        trunc_info->start = clayered_start->stable_cursor;
        if (F_ISSET(trunc_info, WT_TRUNC_EXPLICIT_STOP))
            trunc_info->stop = clayered_stop->stable_cursor;
        WT_WITH_BTREE(
          session, CUR2BT(trunc_info->start), ret = __wt_btcur_range_truncate(trunc_info));
        WT_RET(ret);
    } else {
        /*
         * Set the original keys on the ingest cursors. The ingest cursor may not have its key set
         * if the layered cursor was positioned via next/prev, or if search_near on an empty ingest
         * table reset the cursor position.
         */
        clayered_start->ingest_cursor->set_key(
          clayered_start->ingest_cursor, trunc_info->orig_start_key);
        trunc_info->start = clayered_start->ingest_cursor;

        trunc_info->stop = NULL;
        if (clayered_stop != NULL) {
            clayered_stop->ingest_cursor->set_key(
              clayered_stop->ingest_cursor, trunc_info->orig_stop_key);
            trunc_info->stop = clayered_stop->ingest_cursor;
        }

        /* Perform truncate on ingest table. */
        WT_RET_NOTFOUND_OK(__wt_range_truncate(trunc_info->start, trunc_info->stop));

        /* Add a truncate entry inside layered table truncate list. */
        WT_RET(__wt_insert_truncate_entry(
          session, uri, trunc_info->orig_start_key, trunc_info->orig_stop_key));
    }
    return (0);
}

/*
 * __clayered_position_alternate --
 *     Position an alternate cursor to the right position according to the current one.
 */
static int
__clayered_position_alternate(
  WT_CURSOR_LAYERED *clayered, WT_CURSOR *current, WT_CURSOR *alternate, bool forward)
{
    int cmp;

    WT_SESSION_IMPL *session = CUR2S(clayered);

    WT_ASSERT(session, F_ISSET(current, WT_CURSTD_KEY_SET));
    alternate->set_key(alternate, &current->key);
    WT_RET(alternate->search_near(alternate, &cmp));

    while (forward ? cmp < 0 : cmp > 0) {
        WT_RET(forward ? alternate->next(alternate) : alternate->prev(alternate));

        /*
         * With higher isolation levels, where we have stable reads, we're done: the cursor is now
         * positioned as expected.
         *
         * With read-uncommitted isolation, a new record could have appeared in between the search
         * and stepping forward / back. In that case, keep going until we see a key in the expected
         * range.
         */
        if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED)
            return (0);

        WT_RET(__clayered_cursor_compare(clayered, alternate, current, &cmp));
    }

    return (0);
}

/*
 * __clayered_constituent_iter --
 *     Move the cursor forward or backward.
 */
static int
__clayered_constituent_iter(WT_CURSOR *constituent, bool forward)
{
    return (forward ? constituent->next(constituent) : constituent->prev(constituent));
}

/*
 * __clayered_iterate_constituents --
 *     Move the constituents to the next (or prev) position. If the cursor is unpositioned, position
 *     the constituents.
 *
 * If only one constituent is available, this logic can be simplified to calling "next" on that
 *     constituent.
 *
 * If the cursor is unpositioned, both constituents are positioned on the first (or last) element.
 *
 * A layered cursor is considered positioned when the customer-visible `iface` cursor has the
 *     WT_CURSTD_KEY_INT flag set. In that case, `current_cursor` is expected to be set to the
 *     constituent used to produce the key/value pair for `iface`, and WT_CURSTD_KEY_INT should be
 *     set for it as well. In this case the `iface` key could be used to position the other
 *     (alternate) constituent correctly.
 *
 * For the `alternate_cursor`, the correct position is either the same key as `current_cursor` or
 *     the next available key if the same key does not exist.
 *
 * Subsequent calls to __clayered_prev() or __clayered_next() may skip the positioning step
 *     entirely, since they guarantee that both constituents are properly positioned on exit. To
 *     detect these cases, WT_CLAYERED_ITERATE_NEXT/PREV are used.
 *
 * If both `current_cursor` and `alternate_cursor` are positioned on the same key, both should be
 *     advanced to the next position. Otherwise, only `current_cursor` should be advanced.
 *     `__clayered_get_current` will determine which one to select.
 */
static int
__clayered_iterate_constituents(WT_CURSOR_LAYERED *clayered, uint32_t iter_flag)
{
    WT_CURSOR *c_alternate, *c_current;
    WT_DECL_RET;
    int cmp;
    bool current_moved, forward;

    WT_SESSION_IMPL *session = CUR2S(clayered);
    current_moved = false;
    forward = (iter_flag == WT_CLAYERED_ITERATE_NEXT);
    WT_CURSOR *c_ingest = clayered->ingest_cursor;
    WT_CURSOR *c_stable = clayered->stable_cursor;

    /*
     * FIXME-WT-16810: In leader mode, skip iterating through ingest as it should be empty. This
     * will need revisiting when asynchronous step-up is supported, because ingest may legitimately
     * contain data for some time after promotion.
     */
    if (clayered->leader)
        c_ingest = NULL;

    /*
     * FIXME-WT-15058: Both cursors are expected to be initialized, but we currently have an issue
     * where a cursor open operation can return WT_NOTFOUND for the stable table. Until this is
     * resolved, it is simpler to handle all the different cases here.
     */
    WT_ASSERT(session, c_stable != NULL || c_ingest != NULL);
    if (c_ingest == NULL || c_stable == NULL) {
        c_current = (c_ingest == NULL) ? c_stable : c_ingest;
        /* Return without setting iter_flag because the alternate cursor does not exist. */
        return (__clayered_constituent_iter(c_current, forward));
    }

    WT_ASSERT(session, c_stable != NULL && c_ingest != NULL);

    /*
     * When both constitute cursors are not positioned, it is the start of the cursor walk. Check if
     * the ingest btree cursor has a reference, as the WT_CURSTD_KEY_INT flag is cleared when a
     * prepared conflict occurs. Prepared updates are always ignored on the stable cursor, making it
     * safe to check the WT_CURSTD_KEY_INT flag.
     */
    if (((WT_CURSOR_BTREE *)c_ingest)->ref == NULL && !F_ISSET(c_stable, WT_CURSTD_KEY_INT)) {
        /*
         * Move the stable cursor first to ensure it is advanced, even if a prepared conflict occurs
         * on the ingest cursor.
         */
        WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(c_stable, forward), false);
        /* FIXME-WT-16811: Refactor into a common cursor iteration function. */
        WT_ERR_NOTFOUND_OK(
          __clayered_leader_reposition_iterate(clayered, c_stable, forward), false);
        WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(c_ingest, forward), false);
        goto done;
    }

    /*
     * If WT_CURSTD_KEY_INT is set, the current cursor is expected to be positioned as well. If
     * there is no current cursor, the cursor walk must be blocked by a prepared conflict on the
     * ingest cursor. Select the ingest cursor as the current cursor.
     */
    if (clayered->current_cursor == NULL) {
        WT_ASSERT(session, ((WT_CURSOR_BTREE *)c_ingest)->ref != NULL);
        c_current = c_ingest;
    } else {
        c_current = clayered->current_cursor;
        WT_ASSERT(session,
          F_ISSET(c_current, WT_CURSTD_KEY_INT) ||
            (c_current == c_ingest && ((WT_CURSOR_BTREE *)c_current)->ref != NULL));
    }
    WT_ASSERT(session, c_current == c_stable || c_current == c_ingest);

    /* Identify alternate cursor. */
    c_alternate = (c_current == c_stable) ? c_ingest : c_stable;
    WT_ASSERT(session, c_alternate != c_current);

    /*
     * Move the current cursor ahead of the alternate cursor if its motion was previously blocked by
     * a prepared conflict. This ensures the current cursor has a valid key, which is necessary for
     * properly positioning the alternate cursor. Failing to do so would result in a situation where
     * the alternate cursor cannot be placed due to the absence of a key from the current cursor.
     */
    if (!F_ISSET(c_current, WT_CURSTD_KEY_INT)) {
        WT_ASSERT(session,
          c_current == c_ingest &&
            F_ISSET(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV));
        WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(c_current, forward), false);
        current_moved = true;
    } else if (!F_ISSET(clayered, iter_flag))
        /*
         * The cursor is positioned, but `iter_flag` is not set so we cannot rely on alternate
         * cursor and need to position it.
         */
        WT_ERR_NOTFOUND_OK(
          __clayered_position_alternate(clayered, c_current, c_alternate, forward), false);

    /*
     * If the alternate cursor's key is equal to the current one, we should move it as well. In that
     * case, the alternate must be the stable cursor.
     */
    if (F_ISSET(c_alternate, WT_CURSTD_KEY_INT) && c_current == c_ingest) {
        WT_ERR(__clayered_cursor_compare(clayered, c_alternate, c_current, &cmp));
        if (cmp == 0) {
            WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(c_alternate, forward), false);
            /* FIXME-WT-16811: Refactor into a common cursor iteration function. */
            if (c_alternate == c_stable)
                WT_ERR_NOTFOUND_OK(
                  __clayered_leader_reposition_iterate(clayered, c_stable, forward), false);
        }
    }

    /* Move the current cursor if we haven't done so. */
    if (!current_moved) {
        WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(c_current, forward), false);
        /* FIXME-WT-16811: Refactor into a common cursor iteration function. */
        if (c_current == c_stable)
            WT_ERR_NOTFOUND_OK(
              __clayered_leader_reposition_iterate(clayered, c_stable, forward), false);
    }

done:
err:
    if (ret == 0 || ret == WT_PREPARE_CONFLICT) {
        if (!F_ISSET(clayered, iter_flag)) {
            F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
            F_SET(clayered, iter_flag);
        }
    } else {
        WT_ASSERT(session, ret != WT_NOTFOUND);
        F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    }
    return (ret);
}

/*
 * __clayered_iterate --
 *     Common function for moving a layered cursor to the next or previous position.
 */
static int
__clayered_iterate(WT_CURSOR_LAYERED *clayered, uint32_t iter_flag)
{
    WT_DECL_RET;
    bool deleted;

    WT_SESSION_IMPL *session = CUR2S(clayered);

    do {
        WT_ERR(__clayered_iterate_constituents(clayered, iter_flag));
        WT_ERR(__clayered_get_current(session, clayered, iter_flag == WT_CLAYERED_ITERATE_NEXT));
        if (clayered->current_cursor == clayered->ingest_cursor)
            deleted = __wt_clayered_deleted(&clayered->current_cursor->value);
        else
            deleted = false;
    } while (deleted);

err:
    if (ret != 0 && ret != WT_PREPARE_CONFLICT)
        WT_TRET(__clayered_reset_cursors(clayered, false));

    return (ret);
}

/*
 * __clayered_next --
 *     WT_CURSOR->next method for the layered cursor type.
 */
static int
__clayered_next(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, next, clayered->dhandle);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, false, true, true));

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_next);

    WT_ERR(__clayered_iterate(clayered, WT_CLAYERED_ITERATE_NEXT));

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

    if (clayered->current_cursor == clayered->ingest_cursor)
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_next_ingest);
    else {
        WT_ASSERT(session, clayered->current_cursor == clayered->stable_cursor);
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_next_stable);
    }

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_deleted_decode(&cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    API_END_RET(session, ret);
}

/*
 * __clayered_prev --
 *     WT_CURSOR->prev method for the layered cursor type.
 */
static int
__clayered_prev(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, prev, clayered->dhandle);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, false, true, true));

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_prev);

    WT_ERR(__clayered_iterate(clayered, WT_CLAYERED_ITERATE_PREV));

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

    if (clayered->current_cursor == clayered->ingest_cursor)
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_prev_ingest);
    else {
        WT_ASSERT(session, clayered->current_cursor == clayered->stable_cursor);
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_prev_stable);
    }

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_deleted_decode(&cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    API_END_RET(session, ret);
}

/*
 * __clayered_reset_cursors --
 *     Reset any positioned constituent cursors.
 */
static int
__clayered_reset_cursors(WT_CURSOR_LAYERED *clayered, bool skip_ingest)
{
    WT_CURSOR *c;
    WT_DECL_RET;

    /*
     * Reset constituents that are positioned. Check both KEY_SET and the btree ref, because a
     * prepare conflict clears KEY_SET while leaving the btree cursor positioned (ref != NULL).
     */
    c = clayered->stable_cursor;
    if (c != NULL && F_ISSET(c, WT_CURSTD_KEY_SET))
        WT_TRET(c->reset(c));

    c = clayered->ingest_cursor;
    if (!skip_ingest && c != NULL && ((WT_CURSOR_BTREE *)c)->ref != NULL)
        WT_TRET(c->reset(c));

    clayered->current_cursor = NULL;
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);

    return (ret);
}

/*
 * __clayered_reset --
 *     WT_CURSOR->reset method for the layered cursor type.
 */
static int
__clayered_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clayered_enter path: that is wasted work when all we want to do is
     * give up our position.
     */
    clayered = (WT_CURSOR_LAYERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, clayered->dhandle);
    /* The flag should have been cleared after each cursor operation. */
    WT_ASSERT(session, !F_ISSET(clayered, WT_CLAYERED_READ_STABLE));
    WT_ERR(__cursor_copy_release(cursor));

    WT_TRET(__clayered_reset_cursors(clayered, false));

    /* Reset any bounds on the top level cursor, and propagate that to constituents */
    if (API_USER_ENTRY(session)) {
        __wt_cursor_bound_reset(cursor);
        WT_TRET(__clayered_copy_bounds(clayered));
    }

err:
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    API_END_RET(session, ret);
}

/*
 * __clayered_copy_constituent_bound --
 *     Copy the top level bound into a single constituent cursor
 */
static int
__clayered_copy_constituent_bound(WT_CURSOR_LAYERED *clayered, WT_CURSOR *constituent)
{
    WT_CURSOR *base_cursor;
    WT_SESSION_IMPL *session;

    session = CUR2S(clayered);
    base_cursor = (WT_CURSOR *)clayered;

    if (constituent == NULL)
        return (0);

    /*
     * It doesn't matter if the bound in question is already set on the constituent. It is legal to
     * reset it. Note that the inclusive flag is additive to upper/lower, so no need to check it as
     * well.
     */
    if (F_ISSET(base_cursor, WT_CURSTD_BOUND_UPPER))
        WT_RET(__wt_buf_set(session, &constituent->upper_bound, base_cursor->upper_bound.data,
          base_cursor->upper_bound.size));
    else {
        __wt_buf_free(session, &constituent->upper_bound);
        WT_CLEAR(constituent->upper_bound);
    }
    if (F_ISSET(base_cursor, WT_CURSTD_BOUND_LOWER))
        WT_RET(__wt_buf_set(session, &constituent->lower_bound, base_cursor->lower_bound.data,
          base_cursor->lower_bound.size));
    else {
        __wt_buf_free(session, &constituent->lower_bound);
        WT_CLEAR(constituent->lower_bound);
    }
    /* Copy across all the bound configurations. */
    F_CLR(constituent, WT_CURSTD_BOUND_ALL);
    F_SET(constituent, F_MASK(base_cursor, WT_CURSTD_BOUND_ALL));
    return (0);
}

/*
 * __clayered_copy_bounds --
 *     A method for copying (or clearing) bounds on constituent cursors within a layered cursor
 */
static int
__clayered_copy_bounds(WT_CURSOR_LAYERED *clayered)
{
    WT_RET(__clayered_copy_constituent_bound(clayered, clayered->ingest_cursor));
    WT_RET(__clayered_copy_constituent_bound(clayered, clayered->stable_cursor));
    return (0);
}

/*
 * __clayered_bound --
 *     WT_CURSOR->bound method for the layered cursor type.
 */
static int
__clayered_bound(WT_CURSOR *cursor, const char *config)
{
    WT_COLLATOR *collator;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_CONF(WT_CURSOR, bound, conf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    /*
     * The bound interface operates on an unpositioned cursor, so skip entering the layered cursor
     * for this API.
     */
    CURSOR_API_CALL(cursor, session, ret, bound, clayered->dhandle);

    WT_ERR(__wt_conf_compile_api_call(session, WT_CONFIG_REF(session, WT_CURSOR_bound),
      WT_CONFIG_ENTRY_WT_CURSOR_bound, config, &_conf, sizeof(_conf), &conf));

    __clayered_get_collator(clayered, &collator);
    /* Setup bounds on this top level cursor */
    WT_ERR(__wti_cursor_bound(cursor, conf, collator));

    /*
     * Copy those bounds into the constituents. Note that the constituent cursors may not be open
     * yet, and that would be fine, the layered cursor open interface handles setting up configured
     * bounds as well.
     */
    WT_ERR(__clayered_copy_bounds(clayered));

err:
    if (ret != 0) {
        /* Free any bounds we set on the top level cursor before the error */
        if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER)) {
            __wt_buf_free(session, &cursor->upper_bound);
            WT_CLEAR(cursor->upper_bound);
        }
        if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER)) {
            __wt_buf_free(session, &cursor->lower_bound);
            WT_CLEAR(cursor->lower_bound);
        }
        F_CLR(cursor, WT_CURSTD_BOUND_ALL);
        /* Ensure the bounds are cleaned up on any constituents */
        WT_TRET(__clayered_copy_bounds(clayered));
    }
    API_END_RET(session, ret);
}

/*
 * __clayered_cache --
 *     WT_CURSOR->cache method for the layered cursor type.
 */
static int
__clayered_cache(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;
    session = CUR2S(cursor);

    WT_TRET(__wti_cursor_cache(cursor, clayered->dhandle));
    WT_TRET(__wt_session_release_dhandle(session));

    API_RET_STAT(session, ret, cursor_cache);
}

/*
 * __clayered_reopen_int --
 *     Helper for __clayered_reopen, called with the session data handle set.
 */
static int
__clayered_reopen_int(WT_CURSOR *cursor)
{
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
     * The data handle may not be available, fail the reopen, and flag the cursor so that the handle
     * won't be unlocked when subsequently closed.
     */
    if (is_dead || ret == EBUSY) {
        F_SET(cursor, WT_CURSTD_DEAD);
        ret = WT_NOTFOUND;
    }
    __wti_cursor_reopen(cursor, dhandle);

    /*
     * The layered handle may have been reopened since we last accessed it. Reset fields in the
     * cursor that point to memory owned by the handle.
     */
    if (ret == 0) {
        WT_LAYERED_TABLE *layered = (WT_LAYERED_TABLE *)session->dhandle;
        cursor->internal_uri = session->dhandle->name;
        cursor->key_format = layered->key_format;
        cursor->value_format = layered->value_format;

        WT_STAT_CONN_DSRC_INCR(session, cursor_reopen);
    }
    return (ret);
}

/*
 * __clayered_reopen --
 *     WT_CURSOR->reopen method for the layered cursor type.
 */
static int
__clayered_reopen(WT_CURSOR *cursor, bool sweep_check_only)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool can_sweep;

    session = CUR2S(cursor);
    dhandle = ((WT_CURSOR_LAYERED *)cursor)->dhandle;

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
    WT_WITH_DHANDLE(session, dhandle, ret = __clayered_reopen_int(cursor));
    API_RET_STAT(session, ret, cursor_reopen);
}

/*
 * __clayered_lookup_constituent --
 *     The cursor-agnostic parts of layered table lookups.
 */
static int
__clayered_lookup_constituent(WT_CURSOR *c, WT_CURSOR_LAYERED *clayered, WT_ITEM *value)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = &clayered->iface;

    c->set_key(c, &cursor->key);
    if ((ret = c->search(c)) == 0) {
        WT_RET(c->get_value(c, value));
        clayered->current_cursor = c;
    }

    return (ret);
}

/*
 * __clayered_lookup --
 *     Position a layered cursor.
 */
static int
__clayered_lookup(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, WT_ITEM *value)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    bool found;

    cursor = &clayered->iface;
    conn = S2C(session);
    found = false;

    if (!conn->layered_table_manager.leader) {
        WT_ERR_NOTFOUND_OK(
          __clayered_lookup_constituent(clayered->ingest_cursor, clayered, value), true);
        if (ret == 0) {
            found = true;
            if (__wt_clayered_deleted(value))
                ret = WT_NOTFOUND;
        }

        if (!found) {
            WT_ERR_NOTFOUND_OK(__wt_truncate_delete_visible_check(session,
                                 (WT_LAYERED_TABLE *)clayered->dhandle, &cursor->key, NULL),
              true);
            if (ret == 0) {
                found = true;
                ret = WT_NOTFOUND;
            }
        }
    } else
        /* Be sure we'll make a search attempt further down.  */
        WT_ASSERT(
          session, F_ISSET(clayered, WT_CLAYERED_READ_STABLE) && clayered->stable_cursor != NULL);

    /*
     * If the key didn't exist in the ingest constituent and the cursor is setup for reading, check
     * the stable constituent.
     */
    if (!found && F_ISSET(clayered, WT_CLAYERED_READ_STABLE) && clayered->stable_cursor != NULL)
        WT_ERR_NOTFOUND_OK(
          __clayered_lookup_constituent(clayered->stable_cursor, clayered, value), true);

err:
    if (ret != 0 && ret != WT_PREPARE_CONFLICT)
        WT_TRET(__clayered_reset_cursors(clayered, false));

    return (ret);
}

/*
 * __clayered_search --
 *     WT_CURSOR->search method for the layered cursor type.
 */
static int
__clayered_search(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, search, clayered->dhandle);
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clayered_enter(clayered, true, true, false));

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_search);
    WT_ERR(__clayered_lookup(session, clayered, &cursor->value));
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    if (clayered->current_cursor == clayered->ingest_cursor)
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_ingest);
    else {
        WT_ASSERT(session, clayered->current_cursor == clayered->stable_cursor);
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_stable);
    }

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_deleted_decode(&cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }
    API_END_RET(session, ret);
}

/*
 * __clayered_search_near_move_ingest_to_opposite_side --
 *     search near helper function to move the ingest btree to the opposite side of the search key.
 */
static int
__clayered_search_near_move_ingest_to_opposite_side(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, int stable_cmp, int *ingest_cmp)
{
    WT_COLLATOR *collator;
    WT_CURSOR *cursor, *ingest_cursor;
    WT_DECL_RET;

    cursor = &clayered->iface;
    ingest_cursor = clayered->ingest_cursor;

    __clayered_get_collator(clayered, &collator);
    /*
     * When reading with read-uncommitted isolation, concurrent key insertions may occur. Continue
     * the walk until the search key is reached or passed.
     */
    if (stable_cmp > 0) {
        /* Stable is larger. Move ingest forward to find a larger key in ingest. */
        do {
            WT_ERR_NOTFOUND_OK(ingest_cursor->next(ingest_cursor), true);

            if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED) {
                *ingest_cmp = stable_cmp;
                break;
            }

            if (ret == 0)
                WT_ERR(
                  __wt_compare(session, collator, &ingest_cursor->key, &cursor->key, ingest_cmp));
        } while (ret == 0 && *ingest_cmp < 0);
    } else {
        /* Stable is smaller. Move ingest backward to find a smaller key in ingest. */
        do {
            WT_ERR_NOTFOUND_OK(ingest_cursor->prev(ingest_cursor), true);

            if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED) {
                *ingest_cmp = stable_cmp;
                break;
            }

            if (ret == 0)
                WT_ERR(
                  __wt_compare(session, collator, &ingest_cursor->key, &cursor->key, ingest_cmp));
        } while (ret == 0 && *ingest_cmp > 0);
    }
err:
    return (ret);
}

/*
 * __clayered_search_near_int --
 *     search near method for the layered cursor type.
 */
static int
__clayered_search_near_int(WT_SESSION_IMPL *session, WT_CURSOR *cursor, int *exactp)
{
    WT_CURSOR *closest;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    int cmp, ingest_cmp, stable_cmp;
    bool deleted, ingest_found, match_deleted, stable_found;

    closest = NULL;
    clayered = (WT_CURSOR_LAYERED *)cursor;
    ingest_cmp = stable_cmp = 0;
    ingest_found = match_deleted = stable_found = false;

    /*
     * search_near is somewhat fiddly: we can't just use a nearby key from the current constituent
     * because there could be a closer key in the other table.
     *
     * The semantics are:
     * * An exact match always wins.
     * * Otherwise a larger key is preferred if one exists.
     * * Otherwise a smaller key should be returned.
     * If both constituents have a larger key available, return the one closes to the search term.
     *
     * FIXME-WT-16810: In leader mode, skip searching ingest as it should be empty.
     * This will need revisiting when asynchronous step-up is supported, because ingest may
     * legitimately contain data for some time after promotion.
     */
    if (!clayered->leader) {
        clayered->ingest_cursor->set_key(clayered->ingest_cursor, &cursor->key);
        WT_ERR_NOTFOUND_OK(
          clayered->ingest_cursor->search_near(clayered->ingest_cursor, &ingest_cmp), true);
        if (ret == 0) {
            ingest_found = true;
            match_deleted = __wt_clayered_deleted(&clayered->ingest_cursor->value);
        }
    }

    /* If there wasn't an exact match or the value is deleted, check the stable table as well */
    if ((!ingest_found || ingest_cmp != 0 || match_deleted) && clayered->stable_cursor != NULL) {
        clayered->stable_cursor->set_key(clayered->stable_cursor, &cursor->key);
        WT_ERR_NOTFOUND_OK(
          clayered->stable_cursor->search_near(clayered->stable_cursor, &stable_cmp), true);
        stable_found = ret != WT_NOTFOUND;
    }

    if (!ingest_found && !stable_found) {
        ret = WT_NOTFOUND;
        goto err;
    } else if (!stable_found)
        closest = clayered->ingest_cursor;
    else if (!ingest_found)
        closest = clayered->stable_cursor;

    /* Now that we know there are two positioned cursors - choose the one with the best match */
    if (closest == NULL) {
        if (ingest_cmp == 0)
            closest = clayered->ingest_cursor;
        else if (stable_cmp == 0)
            closest = clayered->stable_cursor;
        else {
            /*
             * If the ingest cursor and stable cursor are positioned on opposite sides of the search
             * key, adjust the ingest cursor to align with the stable cursor on the same side.
             * Otherwise, there is a risk of overlooking a closer key on the ingest table that lies
             * on the opposite side of the search key relative to the stable cursor's position.
             */
            if ((ingest_cmp ^ stable_cmp) < 0) {
                /*
                 * The cursors are on opposite sides of the search key. Move the ingest cursor to
                 * the other side.
                 */
                WT_ERR_NOTFOUND_OK(__clayered_search_near_move_ingest_to_opposite_side(
                                     session, clayered, stable_cmp, &ingest_cmp),
                  true);

                if (ret == WT_NOTFOUND) {
                    ret = 0;
                    closest = clayered->stable_cursor;
                }
            }

            if (closest == NULL) {
                if (ingest_cmp == 0) {
                    WT_ASSERT(session, session->txn->isolation == WT_ISO_READ_UNCOMMITTED);
                    closest = clayered->ingest_cursor;
                } else if (ingest_cmp > 0) {
                    WT_ASSERT(session, stable_cmp > 0);
                    /* Both cursors were larger than the search key - choose the smaller one */
                    WT_ERR(__clayered_cursor_compare(
                      clayered, clayered->ingest_cursor, clayered->stable_cursor, &cmp));
                    if (cmp <= 0)
                        /* If the cursors were identical, choose ingest. */
                        closest = clayered->ingest_cursor;
                    else
                        closest = clayered->stable_cursor;
                } else {
                    WT_ASSERT(session, stable_cmp < 0);
                    /* Both cursors were smaller than the search key - choose the bigger one */
                    WT_ERR(__clayered_cursor_compare(
                      clayered, clayered->ingest_cursor, clayered->stable_cursor, &cmp));
                    if (cmp >= 0)
                        /* If the cursors were identical, choose ingest. */
                        closest = clayered->ingest_cursor;
                    else
                        closest = clayered->stable_cursor;
                }
            }
        }
    }

    WT_ASSERT_ALWAYS(session, closest != NULL, "Layered search near should have found something");

    clayered->current_cursor = closest;

    /* Get prepared for finalizing the result before fixing up for tombstones. */
    if (closest == clayered->stable_cursor) {
        /* Short cut for stable cursor as it doesn't have any deleted value. */
        cmp = stable_cmp;
        goto done;
    }

    /* Closest is the ingest cursor. */
    cmp = ingest_cmp;
    deleted = __wt_clayered_deleted(&closest->value);
    if (deleted) {
        /* Advance past the deleted record using normal cursor traversal interface */
        WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT));
        if ((ret = __clayered_iterate(clayered, WT_CLAYERED_ITERATE_NEXT)) == 0) {
            cmp = 1;
            deleted = false;
        }

        WT_ERR_NOTFOUND_OK(ret, false);
    }

    if (deleted) {
        WT_ASSERT(session, clayered->current_cursor == NULL);
        WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT));
        WT_ERR(__clayered_iterate(clayered, WT_CLAYERED_ITERATE_PREV));
        cmp = -1;
    }

done:
    if (exactp != NULL)
        *exactp = cmp;

    /*
     * If the search-near operation did not internally invoke cursor iteration, reset the
     * constituent cursor that is not the current cursor to prevent unnecessarily pinning the page
     * in memory.
     */
    if (!F_ISSET(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV)) {
        if (clayered->stable_cursor != NULL && clayered->current_cursor == clayered->ingest_cursor)
            WT_ERR(clayered->stable_cursor->reset(clayered->stable_cursor));
        else if (clayered->current_cursor == clayered->stable_cursor)
            WT_ERR(clayered->ingest_cursor->reset(clayered->ingest_cursor));
    }

err:
    if (ret != 0 && ret != WT_PREPARE_CONFLICT)
        WT_TRET(__clayered_reset_cursors(clayered, false));

    return (ret);
}

/*
 * __clayered_search_near --
 *     WT_CURSOR->search_near method for the layered cursor type.
 */
static int
__clayered_search_near(WT_CURSOR *cursor, int *exactp)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, search_near, clayered->dhandle);
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clayered_enter(clayered, true, true, false));

    WT_ERR(__clayered_search_near_int(session, cursor, exactp));

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_near);
    /* FIXME-WT-15545: Handle the case of current_cursor being NULL */
    if (clayered->current_cursor == clayered->ingest_cursor)
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_near_ingest);
    else
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_near_stable);

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_deleted_decode(&cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }

    API_END_RET(session, ret);
}

/*
 * __clayered_reserve_constituent --
 *     Reserve a key in the constituent cursor.
 */
static int
__clayered_reserve_constituent(WT_SESSION_IMPL *session, WT_CURSOR *constituent)
{
    WT_DECL_RET;
    CURSOR_UPDATE_API_CALL_BTREE(constituent, session, ret, reserve);

    /*
     * Pass overwrite=true for followers: a follower's ingest table may not contain the key yet (it
     * lives only in the stable table), so we need overwrite mode to allow the reserve to succeed
     * without the key being present in the update tree.
     */
    WT_ERR(__wt_btcur_reserve(
      (WT_CURSOR_BTREE *)constituent, !S2C(session)->layered_table_manager.leader));

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_reserve);

    return (ret);
}

/*
 * __clayered_put --
 *     Put an entry into the desired tree.
 */
static WT_INLINE int
__clayered_put(WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, const WT_ITEM *key,
  const WT_ITEM *value, WT_CLAYERED_PUT_OP op)
{

    bool leader = S2C(session)->layered_table_manager.leader;

    if (!leader) {
        /*
         * FIXME-WT-16812: Investigate whether this function can be called below the cursor layer.
         * Doing so would remove the cursor write operation dependency on the truncate list.
         */
        WT_RET(__wt_layered_table_truncate_detect_write_conflict(
          session, (WT_LAYERED_TABLE *)clayered->dhandle, key));

        /*
         * Clear the stable cursor position. Don't clear the ingest cursor: we're about to use it
         * anyway. Keep the cursor position if we are in the middle of a cursor traversal.
         */
        if (!F_ISSET(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV))
            WT_RET(__clayered_reset_cursors(clayered, true));
    }

    WT_CURSOR *c = leader ? clayered->stable_cursor : clayered->ingest_cursor;
    c->set_key(c, key);
    /* Reserve does not require a value. */
    if (op != WT_CLAYERED_PUT_RESERVE)
        c->set_value(c, value);

    switch (op) {
    case WT_CLAYERED_PUT_INSERT:
        WT_RET(c->insert(c));
        break;
    case WT_CLAYERED_PUT_UPDATE:
        WT_RET(c->update(c));
        break;
    case WT_CLAYERED_PUT_RESERVE:
        WT_RET(__clayered_reserve_constituent(session, c));
        break;
    }

    /* If necessary, set the position for future scans. */
    if (op != WT_CLAYERED_PUT_INSERT)
        clayered->current_cursor = c;

    return (0);
}

/*
 * __clayered_remove_follower --
 *     Remove an entry from the ingest table.
 */
static WT_INLINE int
__clayered_remove_follower(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, const WT_ITEM *key, bool positioned)
{
    WT_CURSOR *const c = clayered->ingest_cursor;
    WT_DECL_RET;
    WT_ITEM value;

    WT_CLEAR(value);

    if (positioned) {
        if (clayered->current_cursor == c) {
            WT_ASSERT(session, F_ISSET(c, WT_CURSTD_KEY_INT));
            WT_ASSERT(session, F_ISSET(c, WT_CURSTD_VALUE_INT));
            /*
             * If we are erasing a record that is already a tombstone, don't write another one: we
             * don't ever want consecutive tombstones on an update chain.
             */
            WT_ITEM_SET(value, c->value);
            if (__wt_clayered_deleted(&value))
                return (WT_NOTFOUND);
        }
    } else {
        WT_ASSERT(session, F_ISSET(&clayered->iface, WT_CURSTD_KEY_EXT));
        WT_RET(__clayered_lookup(session, clayered, &value));
    }

    /* If we are positioned on the stable table, we need to set the key. */
    if (clayered->current_cursor != c)
        c->set_key(c, key);

    /*
     * Clear the stable cursor position. Don't clear the ingest cursor: we're about to use it
     * anyway. Keep the cursor position if we are in the middle of a cursor traversal.
     */
    if (!F_ISSET(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV))
        WT_RET(__clayered_reset_cursors(clayered, true));

    /*
     * FIXME-WT-16812: Investigate whether this function can be called below the cursor layer. Doing
     * so would remove the write cursor operations dependency on the truncate list.
     */
    WT_RET(__wt_layered_table_truncate_detect_write_conflict(
      session, (WT_LAYERED_TABLE *)clayered->dhandle, key));
    c->set_value(c, &__wt_tombstone);
    WT_ERR(c->update(c));
    clayered->current_cursor = c;

err:
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    return (ret);
}

/*
 * __clayered_remove_leader --
 *     Remove an entry from the stable table.
 */
static WT_INLINE int
__clayered_remove_leader(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, const WT_ITEM *key, bool positioned)
{
    WT_CURSOR *const c = clayered->stable_cursor;

    /* There is no content on the ingest table. We must be positioned on the stable table. */
    if (!positioned)
        c->set_key(c, key);
    else
        WT_ASSERT(session, F_ISSET(c, WT_CURSTD_KEY_INT));

    WT_RET(c->remove(c));
    clayered->current_cursor = c;

    return (0);
}

/*
 * __clayered_remove_int --
 *     Remove an entry from the desired tree.
 */
static WT_INLINE int
__clayered_remove_int(
  WT_SESSION_IMPL *session, WT_CURSOR_LAYERED *clayered, const WT_ITEM *key, bool positioned)
{
    return (S2C(session)->layered_table_manager.leader ?
        __clayered_remove_leader(session, clayered, key, positioned) :
        __clayered_remove_follower(session, clayered, key, positioned));
}

/*
 * __clayered_copy_duplicate_kv --
 *     Copy the duplicate key value from the constitute cursor.
 */
static int
__clayered_copy_duplicate_kv(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_SESSION_IMPL *session;

    clayered = (WT_CURSOR_LAYERED *)cursor;
    session = CUR2S(cursor);

    WT_ASSERT(session,
      F_ISSET(clayered->current_cursor, WT_CURSTD_KEY_INT) &&
        F_ISSET(clayered->current_cursor, WT_CURSTD_VALUE_INT));
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    WT_RET(__wt_cursor_localkey(cursor));
    WT_RET(__cursor_localvalue(cursor));
    WT_RET(__clayered_reset_cursors(clayered, false));

    return (0);
}

/*
 * __clayered_insert --
 *     WT_CURSOR->insert method for the layered cursor type.
 */
static int
__clayered_insert(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, insert, clayered->dhandle);
    /* Insert doesn't keep the cursor positioned. Always clear the iteration flags. */
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clayered_enter(clayered, false,
      S2C(session)->layered_table_manager.leader || !F_ISSET(clayered, WT_CURSTD_OVERWRITE),
      false));

    /*
     * It isn't necessary to copy the key out after the lookup in this case because any non-failed
     * lookup results in an error, and a failed lookup leaves the original key intact.
     */
    if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) &&
      (ret = __clayered_lookup(session, clayered, &value)) != WT_NOTFOUND) {
        if (ret == 0) {
            WT_ERR(__clayered_copy_duplicate_kv(cursor));
            WT_ERR(__clayered_reset_cursors(clayered, false));
            WT_ERR(WT_DUPLICATE_KEY);
        }

        goto err;
    }

    WT_ERR(__clayered_deleted_encode(session, &cursor->value, &value, &buf));
    WT_ERR(__clayered_put(session, clayered, &cursor->key, &value, WT_CLAYERED_PUT_INSERT));

    /*
     * WT_CURSOR.insert doesn't leave the cursor positioned, and the application may want to free
     * the memory used to configure the insert; don't read that memory again (matching the
     * underlying file object cursor insert semantics).
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_insert);
err:
    __wt_scr_free(session, &buf);
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_update --
 *     WT_CURSOR->update method for the layered cursor type.
 */
static int
__clayered_update(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, update, clayered->dhandle);
    /*
     * Update keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT))
        F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clayered_enter(clayered, false,
      S2C(session)->layered_table_manager.leader || !F_ISSET(clayered, WT_CURSTD_OVERWRITE),
      false));

    if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE)) {
        WT_ERR(__clayered_lookup(session, clayered, &value));
        /*
         * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may
         * have landed on.
         */
        WT_ERR(__cursor_needkey(cursor));
    }
    WT_ERR(__clayered_deleted_encode(session, &cursor->value, &value, &buf));
    WT_ERR(__clayered_put(session, clayered, &cursor->key, &value, WT_CLAYERED_PUT_UPDATE));

    /*
     * Set the cursor to reference the internal key/value of the positioned cursor.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    WT_ASSERT(session, F_MASK(clayered->current_cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(
      session, F_MASK(clayered->current_cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_update);

err:
    __wt_scr_free(session, &buf);
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_remove --
 *     WT_CURSOR->remove method for the layered cursor type.
 */
static int
__clayered_remove(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool positioned;

    clayered = (WT_CURSOR_LAYERED *)cursor;

    /* Remember if the cursor is currently positioned. */
    positioned = F_ISSET(cursor, WT_CURSTD_KEY_INT);

    CURSOR_REMOVE_API_CALL(cursor, session, ret, clayered->dhandle);
    /*
     * Remove keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!positioned)
        F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);

    WT_ERR(__clayered_enter(clayered, false, true, false));
    /*
     * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may have
     * landed on.
     */
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__clayered_remove_int(session, clayered, &cursor->key, positioned));

    /*
     * If the cursor was positioned, it stays positioned with a key but no value, otherwise, there's
     * no position, key or value. This isn't just cosmetic, without a reset, iteration on this
     * cursor won't start at the beginning/end of the table.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (positioned)
        F_SET(cursor, WT_CURSTD_KEY_INT);
    else
        WT_TRET(__clayered_reset_cursors(clayered, false));
    WT_STAT_CONN_DSRC_INCR(session, layered_curs_remove);

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_reserve --
 *     WT_CURSOR->reserve method for the layered cursor type.
 */
static int
__clayered_reserve(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, reserve, clayered->dhandle);

    /*
     * Since a search will be performed afterward that clears the iteration flags, no point to
     * retain the flags.
     */
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__wt_txn_context_check(session, true));

    WT_ERR(__clayered_enter(clayered, false, true, false));

    /* WT_CURSOR.reserve is update-without-overwrite so we should check whether the key exists. */
    WT_ERR(__clayered_lookup(session, clayered, &value));

    WT_ERR(__clayered_put(session, clayered, &cursor->key, NULL, WT_CLAYERED_PUT_RESERVE));

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);

    /*
     * The application might do a WT_CURSOR.get_value call when we return, so we need a value and
     * the underlying functions didn't set one up. For various reasons, those functions may not have
     * done a search and any previous value in the cursor might race with WT_CURSOR.reserve (and in
     * cases like layered tables, the reserve never encountered the original key). For simplicity,
     * repeat the search here.
     */
    return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __clayered_largest_key --
 *     WT_CURSOR->largest_key implementation for layered tables.
 */
static int
__clayered_largest_key(WT_CURSOR *cursor)
{
    WT_COLLATOR *collator;
    WT_CURSOR *larger_cursor, *ingest_cursor, *stable_cursor;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int cmp;
    bool ingest_found, stable_found;

    clayered = (WT_CURSOR_LAYERED *)cursor;
    ingest_found = stable_found = false;

    CURSOR_API_CALL(cursor, session, ret, largest_key, clayered->dhandle);
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, false, true, false));

    ingest_cursor = clayered->ingest_cursor;
    stable_cursor = clayered->stable_cursor;

    WT_ERR(__wt_scr_alloc(session, 0, &key));

    WT_ERR_NOTFOUND_OK(ingest_cursor->largest_key(ingest_cursor), true);
    if (ret == 0)
        ingest_found = true;

    if (stable_cursor != NULL) {
        WT_ERR_NOTFOUND_OK(stable_cursor->largest_key(stable_cursor), true);
        if (ret == 0)
            stable_found = true;
    }

    if (!ingest_found && !stable_found) {
        ret = WT_NOTFOUND;
        goto err;
    }

    if (ingest_found && !stable_found)
        larger_cursor = ingest_cursor;
    else if (!ingest_found && stable_found) {
        larger_cursor = stable_cursor;
    } else {
        __clayered_get_collator(clayered, &collator);
        if (stable_cursor == NULL)
            larger_cursor = ingest_cursor;
        else {
            WT_ERR(__wt_compare(session, collator, &ingest_cursor->key, &stable_cursor->key, &cmp));
            if (cmp <= 0)
                larger_cursor = stable_cursor;
            else
                larger_cursor = ingest_cursor;
        }
    }

    /* Copy the key as we will reset the cursor after that. */
    WT_ERR(__wt_buf_set(session, key, larger_cursor->key.data, larger_cursor->key.size));
    WT_TRET(__clayered_reset_cursors(clayered, false));
    F_CLR(cursor, WT_CURSTD_KEY_INT);
    WT_ERR(__wt_buf_set(session, &cursor->key, key->data, key->size));
    /* Set the key as external. */
    F_SET(cursor, WT_CURSTD_KEY_EXT);

err:
    __clayered_leave(clayered);
    __wt_scr_free(session, &key);
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    API_END_RET_STAT(session, ret, cursor_largest_key);
}

/*
 * __clayered_close_int --
 *     Close a layered cursor
 */
static int
__clayered_close_int(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool dead;

    dead = F_ISSET(cursor, WT_CURSTD_DEAD);
    session = CUR2S(cursor);
    WT_ASSERT_ALWAYS(session, session->dhandle->type == WT_DHANDLE_TYPE_LAYERED,
      "Valid layered dhandle is required to close a cursor");
    clayered = (WT_CURSOR_LAYERED *)cursor;

    /*
     * No need to close the constituent cursors if it has been already done during connection->close
     * performing a close of all cursors in the session.
     */
    if (!F_ISSET(cursor, WT_CURSTD_CONSTITUENT_DEAD))
        WT_TRET(__clayered_close_cursors(clayered));

    __wt_cursor_close(cursor);

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
    return (ret);
}

/*
 * __clayered_close --
 *     WT_CURSOR->close method for the layered cursor type.
 */
static int
__clayered_close(WT_CURSOR *cursor)
{
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clayered_enter path: that is wasted work when closing, and the cursor
     * may never have been used.
     */
    clayered = (WT_CURSOR_LAYERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, clayered->dhandle);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ASSERT(session, !F_ISSET(clayered, WT_CLAYERED_READ_STABLE));
err:
    if (ret == 0) {
        /*
         * If releasing the cursor fails in any way, it will be left in a state that allows it to be
         * normally closed.
         */
        bool released = false;
        ret = __wti_cursor_cache_release(session, cursor, &released);

        if (released) {
            /*
             * If the cursor has been cached, try to cache the constituent cursors by evoking a
             * cursor close.
             *
             * Note: There no need to close the constituent cursors if it has been already done
             * during connection->close performing a close of all cursors in the session.
             */
            if (!F_ISSET(cursor, WT_CURSTD_CONSTITUENT_DEAD))
                WT_TRET(__clayered_close_cursors(clayered));

            goto done;
        }
    }
    /* For cached cursors, free any extra buffers retained now. */
    __wt_cursor_free_cached_memory(cursor);
    cursor->internal_uri = NULL;

    WT_TRET(__clayered_close_int(cursor));
done:
    API_END_RET(session, ret);
}

/*
 * __clayered_next_random --
 *     WT_CURSOR->next_random method for the layered cursor type.
 */
static int
__clayered_next_random(WT_CURSOR *cursor)
{
    WT_CURSOR *c;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int exact;

    c = NULL; /* Workaround for compilers reporting it as used uninitialized. */
    clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, next, clayered->dhandle);
    F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, false, true, true));

    for (;;) {
        /* FIXME-WT-14736: consider the size of ingest table in the future. */
        if (clayered->stable_cursor != NULL) {
            c = clayered->stable_cursor;
            /*
             * This call to next_random on the layered table can potentially end in WT_NOTFOUND if
             * the layered table is empty. When that happens, use the ingest table.
             */
            WT_ERR_NOTFOUND_OK(__wti_curfile_next_random(c), true);
        } else
            ret = WT_NOTFOUND;

        /* The stable table was either empty or missing. */
        if (ret == WT_NOTFOUND) {
            c = clayered->ingest_cursor;
            WT_ERR(__wti_curfile_next_random(c));
        }

        WT_ITEM_SET(cursor->key, c->key);
        F_SET(cursor, WT_CURSTD_KEY_INT);
        WT_ERR(__wt_cursor_localkey(cursor));

        /*
         * Search near the current key to resolve any tombstones and position to a valid document.
         * If we see a WT_NOTFOUND here that is valid, as the tree has no documents visible to us.
         */
        WT_ERR(__clayered_search_near_int(session, cursor, &exact));
        break;
    }

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_deleted_decode(&cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else {
        WT_TRET(__clayered_reset_cursors(clayered, false));
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    }

    API_END_RET(session, ret);
}

/*
 * __clayered_modify_leader --
 *     Apply a set of modifications on a leader node.
 */
static int
__clayered_modify_leader(
  WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_CURSOR_LAYERED *clayered = (WT_CURSOR_LAYERED *)cursor;
    WT_CURSOR *stable = clayered->stable_cursor;
    WT_DECL_RET;
    WT_DECL_ITEM(buf);

    stable->set_key(stable, &cursor->key);
    /* It's valid to build the modify on an empty value. */
    WT_ERR_NOTFOUND_OK(stable->search(stable), true);

    /*
     * Similarly, a delete-encoded value alters the original value and also cannot serve as the base
     * value for a modify. In these cases, perform a full update instead.
     */
    if (ret == 0 && __clayered_is_deleted_encoded(&stable->value)) {
        __clayered_deleted_decode(&stable->value);
        WT_ERR(__wt_modify_apply_api(stable, entries, nentries));
        WT_ERR(__clayered_deleted_encode(session, &stable->value, &stable->value, &buf));
        F_SET(stable, WT_CURSTD_VALUE_EXT);
        WT_ERR(stable->update(stable));
    } else
        WT_ERR(stable->modify(stable, entries, nentries));

    clayered->current_cursor = stable;

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __clayered_modify_follower --
 *     Apply a set of modifications on a leader node.
 */
static int
__clayered_modify_follower(
  WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_CURSOR_LAYERED *clayered = (WT_CURSOR_LAYERED *)cursor;
    WT_CURSOR *ingest = clayered->ingest_cursor;
    WT_DECL_RET;
    WT_DECL_ITEM(buf);
    WT_ITEM value;

    WT_CLEAR(value);

    /* Do a search if we're not positioned. */
    if (!F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT))
        WT_ERR_NOTFOUND_OK(__clayered_lookup(session, clayered, &value), true);
    else
        WT_ITEM_SET(value, cursor->value);

    if (clayered->current_cursor != ingest) {
        /*
         * Cursor is positioned on the stable table. Compute a full value and write it to the ingest
         * table.
         */
        ingest->set_key(ingest, &cursor->key);
        __clayered_deleted_decode(&value);
        WT_ITEM_SET(ingest->value, value);
        WT_ERR(__wt_modify_apply_api(ingest, entries, nentries));
        WT_ERR(__clayered_deleted_encode(session, &ingest->value, &ingest->value, &buf));
        F_SET(ingest, WT_CURSTD_VALUE_EXT);
        WT_ERR(ingest->update(ingest));
    } else {
        /*
         * A tombstone is a special value in the ingest table, so it cannot be used as a base value
         * for a modify operation. Similarly, a delete-encoded value alters the original value and
         * also cannot serve as the base value for a modify. In these cases, perform a full update
         * instead.
         */
        if (ret == WT_NOTFOUND || __wt_clayered_deleted(&ingest->value) ||
          __clayered_is_deleted_encoded(&ingest->value)) {
            __clayered_deleted_decode(&ingest->value);
            WT_ERR(__wt_modify_apply_api(ingest, entries, nentries));
            WT_ERR(__clayered_deleted_encode(session, &ingest->value, &ingest->value, &buf));
            F_SET(ingest, WT_CURSTD_VALUE_EXT);
            WT_ERR(ingest->update(ingest));
        } else
            WT_ERR(ingest->modify(ingest, entries, nentries));
    }

    /*
     * Clear the stable cursor position. Keep the cursor position if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV))
        WT_ERR(__clayered_reset_cursors(clayered, true));
    clayered->current_cursor = ingest;

err:
    __wt_scr_free(session, &buf);
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    return (ret);
}

/*
 * __clayered_modify_int --
 *     Dispatch a modify call based on leader/follower status.
 */
static int
__clayered_modify_int(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    if (S2C(session)->layered_table_manager.leader)
        WT_RET(__clayered_modify_leader(session, cursor, entries, nentries));
    else
        WT_RET(__clayered_modify_follower(session, cursor, entries, nentries));

    return (0);
}

/*
 * __clayered_modify --
 *     WT_CURSOR->modify method for layered cursors.
 */
static int
__clayered_modify(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WT_CURSOR *current;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WT_CURSOR_LAYERED *clayered = (WT_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, modify, clayered->dhandle);
    /*
     * Modify keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT))
        F_CLR(clayered, WT_CLAYERED_ITERATE_NEXT | WT_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__clayered_enter(clayered, false, true, false));

    /* Check for a rational modify vector count. */
    if (nentries <= 0)
        WT_ERR_MSG(session, EINVAL, "Illegal modify vector with %d entries", nentries);

    WT_ERR(__clayered_modify_int(session, cursor, entries, nentries));

    /*
     * Set the cursor to reference the internal key/value of the positioned cursor.
     */
    current = clayered->current_cursor;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    /*
     * Assign the new key/value to the top-level cursor.
     */
    WT_ITEM_SET(cursor->key, current->key);
    WT_ITEM_SET(cursor->value, current->value);
    WT_ASSERT(session, F_MASK(current, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    F_SET(cursor, WT_CURSTD_KEY_INT);

    WT_ASSERT(session, F_ISSET(current, WT_CURSTD_VALUE_SET));
    F_SET(cursor, F_MASK(current, WT_CURSTD_VALUE_SET));

    /*
     * Modify maintains a position, key and value. Unlike update, it's not always an internal value.
     */
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) != 0);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_modify);

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_modify);
    return (ret);
}

/*
 * __wt_clayered_open --
 *     WT_SESSION->open_cursor method for layered cursors.
 */
int
__wt_clayered_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __clayered_compare,                             /* compare */
      __wt_cursor_equals,                             /* equals */
      __clayered_next,                                /* next */
      __clayered_prev,                                /* prev */
      __clayered_reset,                               /* reset */
      __clayered_search,                              /* search */
      __clayered_search_near,                         /* search-near */
      __clayered_insert,                              /* insert */
      __clayered_modify,                              /* modify */
      __clayered_update,                              /* update */
      __clayered_remove,                              /* remove */
      __clayered_reserve,                             /* reserve */
      __wti_cursor_reconfigure,                       /* reconfigure */
      __clayered_largest_key,                         /* largest_key */
      __clayered_bound,                               /* bound */
      __clayered_cache,                               /* cache */
      __clayered_reopen,                              /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __clayered_close);                              /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered;
    bool cacheable;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_LAYERED);

    clayered = NULL;
    cursor = NULL;
    cacheable = F_ISSET(session, WT_SESSION_CACHE_CURSORS);

    if (!WT_PREFIX_MATCH(uri, "layered:"))
        return (__wt_unexpected_object_type(session, uri, "layered:"));

    WT_RET(__wt_inmem_unsupported_op(session, "Layered trees"));

    WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
    if (cval.len != 0)
        WT_RET_MSG(session, EINVAL, "Layered trees do not support opening by checkpoint");

    WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
    if (cval.val != 0)
        WT_RET_MSG(session, EINVAL, "Layered trees do not support bulk loading");

    /* Get the layered tree, and hold a reference to it until the cursor is closed. */
    WT_RET(__wt_session_get_dhandle(session, uri, NULL, cfg, 0));

    /*
     * Increment the data-source's in-use counter; done now because closing the cursor will
     * decrement it, and all failure paths from here close the cursor.
     */
    __wt_cursor_dhandle_incr_use(session);

    layered = (WT_LAYERED_TABLE *)session->dhandle;
    WT_ASSERT_ALWAYS(session, layered->ingest_uri != NULL && layered->key_format != NULL,
      "Layered handle not setup");

    WT_ERR(__wt_calloc_one(session, &clayered));
    clayered->dhandle = session->dhandle;

    cursor = (WT_CURSOR *)clayered;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->internal_uri = session->dhandle->name;
    cursor->key_format = layered->key_format;
    cursor->value_format = layered->value_format;

    WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0) {
        F_SET(clayered, WT_CLAYERED_RANDOM);
        __wti_cursor_set_notsup(cursor);
        cursor->next = __clayered_next_random;

        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_seed", 0, &cval));
        clayered->next_random_seed = cval.val;

        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_sample_size", 0, &cval));
        clayered->next_random_sample_size = (u_int)cval.val;
        cacheable = false;
    }

    /* Set the cache flag before finding a cursor handle. */
    if (cacheable)
        F_SET(cursor, WT_CURSTD_CACHEABLE);

    /* Try to find the cursor in the cache. */
    WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

    /* Layered cursor is not compatible with cursor_copy config. */
    F_CLR(cursor, WT_CURSTD_DEBUG_COPY_KEY | WT_CURSTD_DEBUG_COPY_VALUE);

    if (0) {
err:
        /* Our caller expects to release the data handles if we fail. */
        clayered->dhandle = NULL;
        __wt_cursor_dhandle_decr_use(session);
        if (clayered != NULL)
            WT_TRET(__clayered_close(cursor));
        WT_TRET(__wt_session_release_dhandle(session));

        *cursorp = NULL;
    }

    return (ret);
}

/*
 * __wt_debug_layered_cursor_page --
 *     Dump the in-memory information for a cursor-referenced page.
 */
int
__wt_debug_layered_cursor_page(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    const WT_CURSOR *cursor = (const WT_CURSOR *)cursor_arg;
    WT_UNUSED(ofile);

    __wt_verbose_debug1(
      CUR2S(cursor), WT_VERB_DEFAULT, "%s: unsupported cursor type for debug dump", cursor->uri);

    return (0);
}

/*
 * __wt_debug_layered_cursor_tree_hs --
 *     Dump the in-memory information for a cursor-referenced tree's history store page.
 */
int
__wt_debug_layered_cursor_tree_hs(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    const WT_CURSOR *cursor = (const WT_CURSOR *)cursor_arg;
    WT_UNUSED(ofile);

    __wt_verbose_debug1(
      CUR2S(cursor), WT_VERB_DEFAULT, "%s: unsupported cursor type for debug dump", cursor->uri);

    return (0);
}
