/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __tiered_flush_state --
 *     Account for flush work units so threads can know when shared storage flushing is complete.
 */
static void
__tiered_flush_state(WT_SESSION_IMPL *session, uint32_t type, bool incr)
{
    WT_CONNECTION_IMPL *conn;

    if (type != WT_TIERED_WORK_FLUSH)
        return;
    conn = S2C(session);
    if (incr)
        (void)__wt_atomic_addv32(&conn->flush_state, 1);
    else
        (void)__wt_atomic_subv32(&conn->flush_state, 1);
}

/*
 * __wt_tiered_work_free --
 *     Free a work unit and account for it in the flush state.
 */
void
__wt_tiered_work_free(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    __tiered_flush_state(session, entry->type, false);
    /* If all work is done signal any waiting thread waiting for sync. */
    if (WT_FLUSH_STATE_DONE(conn->flush_state))
        __wt_cond_signal(session, conn->flush_cond);
    __wt_free(session, entry);
}

/*
 * __wt_tiered_push_work --
 *     Push a work unit to the queue. Assumes it is passed an already filled out structure.
 */
void
__wt_tiered_push_work(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT *entry)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    __wt_spin_lock(session, &conn->tiered_lock);
    TAILQ_INSERT_TAIL(&conn->tieredqh, entry, q);
    WT_STAT_CONN_INCR(session, tiered_work_units_created);
    __wt_spin_unlock(session, &conn->tiered_lock);
    __tiered_flush_state(session, entry->type, true);
    __wt_cond_signal(session, conn->tiered_cond);
    return;
}

/*
 * __wt_tiered_pop_work --
 *     Pop a work unit of the given type from the queue. If a maximum value is given, only return a
 *     work unit that is less than the maximum value. The caller is responsible for freeing the
 *     returned work unit structure.
 */
void
__wt_tiered_pop_work(
  WT_SESSION_IMPL *session, uint32_t type, uint64_t maxval, WT_TIERED_WORK_UNIT **entryp)
{
    WT_CONNECTION_IMPL *conn;
    WT_TIERED_WORK_UNIT *entry;

    *entryp = entry = NULL;

    conn = S2C(session);
    if (TAILQ_EMPTY(&conn->tieredqh))
        return;
    __wt_spin_lock(session, &conn->tiered_lock);

    TAILQ_FOREACH (entry, &conn->tieredqh, q) {
        if (FLD_ISSET(type, entry->type) && (maxval == 0 || entry->op_val < maxval)) {
            TAILQ_REMOVE(&conn->tieredqh, entry, q);
            WT_STAT_CONN_INCR(session, tiered_work_units_dequeued);
            *entryp = entry;
            break;
        }
    }
    __wt_spin_unlock(session, &conn->tiered_lock);
    return;
}

/*
 * __wt_tiered_get_flush --
 *     Get the first flush work unit from the queue. The id information cannot change between our
 *     caller and here. The caller is responsible for freeing the work unit.
 */
void
__wt_tiered_get_flush(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp)
{
    __wt_tiered_pop_work(session, WT_TIERED_WORK_FLUSH, 0, entryp);
    return;
}

/*
 * __wt_tiered_get_drop_local --
 *     Get a drop local work unit if it is less than the time given. The caller is responsible for
 *     freeing the work unit.
 */
void
__wt_tiered_get_drop_local(WT_SESSION_IMPL *session, uint64_t now, WT_TIERED_WORK_UNIT **entryp)
{
    __wt_tiered_pop_work(session, WT_TIERED_WORK_DROP_LOCAL, now, entryp);
    return;
}

/*
 * __wt_tiered_get_drop_shared --
 *     Get a drop shared work unit. The caller is responsible for freeing the work unit.
 */
void
__wt_tiered_get_drop_shared(WT_SESSION_IMPL *session, WT_TIERED_WORK_UNIT **entryp)
{
    __wt_tiered_pop_work(session, WT_TIERED_WORK_DROP_SHARED, 0, entryp);
    return;
}

/*
 * __wt_tiered_put_drop_local --
 *     Add a drop local work unit for the given ID to the queue.
 */
int
__wt_tiered_put_drop_local(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
{
    WT_TIERED_WORK_UNIT *entry;
    uint64_t now;

    WT_RET(__wt_calloc_one(session, &entry));
    entry->type = WT_TIERED_WORK_DROP_LOCAL;
    entry->id = id;
    WT_ASSERT(session, tiered->bstorage != NULL);
    __wt_seconds(session, &now);
    /* Put a work unit in the queue with the time this object expires. */
    entry->op_val = now + tiered->bstorage->retain_secs;
    entry->tiered = tiered;
    __wt_tiered_push_work(session, entry);
    return (0);
}

/*
 * __wt_tiered_put_drop_shared --
 *     Add a drop shared work unit for the given ID to the queue.
 */
int
__wt_tiered_put_drop_shared(WT_SESSION_IMPL *session, WT_TIERED *tiered, uint32_t id)
{
    WT_TIERED_WORK_UNIT *entry;

    WT_RET(__wt_calloc_one(session, &entry));
    entry->type = WT_TIERED_WORK_DROP_SHARED;
    entry->id = id;
    entry->tiered = tiered;
    __wt_tiered_push_work(session, entry);
    return (0);
}

/*
 * __wt_tiered_put_flush --
 *     Add a flush work unit to the queue. We're single threaded so the tiered structure's id
 *     information cannot change between our caller and here.
 */
int
__wt_tiered_put_flush(WT_SESSION_IMPL *session, WT_TIERED *tiered)
{
    WT_TIERED_WORK_UNIT *entry;

    WT_RET(__wt_calloc_one(session, &entry));
    entry->type = WT_TIERED_WORK_FLUSH;
    entry->id = tiered->current_id;
    entry->tiered = tiered;
    __wt_tiered_push_work(session, entry);
    return (0);
}
