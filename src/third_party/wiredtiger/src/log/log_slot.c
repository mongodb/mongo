/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
/*
 * __log_slot_dump --
 *     Dump the entire slot state.
 */
static void
__log_slot_dump(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    int earliest, i;

    conn = S2C(session);
    log = conn->log;
    ret = __wt_verbose_dump_log(session);
    WT_ASSERT(session, ret == 0);
    earliest = 0;
    for (i = 0; i < WT_SLOT_POOL; i++) {
        slot = &log->slot_pool[i];
        if (__wt_log_cmp(&slot->slot_release_lsn, &log->slot_pool[earliest].slot_release_lsn) < 0)
            earliest = i;
        __wt_errx(session, "Slot %d (0x%p):", i, (void *)slot);
        __wt_errx(session, "    State: %" PRIx64 " Flags: %" PRIx32, (uint64_t)slot->slot_state,
          slot->flags);
        __wt_errx(session, "    Start LSN: %" PRIu32 "/%" PRIu32, slot->slot_start_lsn.l.file,
          slot->slot_start_lsn.l.offset);
        __wt_errx(session, "    End  LSN: %" PRIu32 "/%" PRIu32, slot->slot_end_lsn.l.file,
          slot->slot_end_lsn.l.offset);
        __wt_errx(session, "    Release LSN: %" PRIu32 "/%" PRIu32, slot->slot_release_lsn.l.file,
          slot->slot_release_lsn.l.offset);
        __wt_errx(session, "    Offset: start: %" PRIuMAX " last:%" PRIuMAX,
          (uintmax_t)slot->slot_start_offset, (uintmax_t)slot->slot_last_offset);
        __wt_errx(session, "    Unbuffered: %" PRId64 " error: %" PRId32, slot->slot_unbuffered,
          slot->slot_error);
    }
    __wt_errx(session, "Earliest slot: %d", earliest);
}
#endif

/*
 * __wt_log_slot_activate --
 *     Initialize a slot to become active.
 */
void
__wt_log_slot_activate(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;

    conn = S2C(session);
    log = conn->log;

    /*
     * !!! slot_release_lsn must be set outside this function because
     * this function may be called after a log file switch and the
     * slot_release_lsn must refer to the end of the previous log.
     * !!! We cannot initialize flags here because it may already be
     * set for closing the file handle on a log file switch.  The flags
     * are reset when the slot is freed.  See log_slot_free.
     */
    slot->slot_unbuffered = 0;
    WT_ASSIGN_LSN(&slot->slot_start_lsn, &log->alloc_lsn);
    WT_ASSIGN_LSN(&slot->slot_end_lsn, &slot->slot_start_lsn);
    slot->slot_start_offset = log->alloc_lsn.l.offset;
    slot->slot_last_offset = log->alloc_lsn.l.offset;
    slot->slot_fh = log->log_fh;
    slot->slot_error = 0;
    WT_DIAGNOSTIC_YIELD;
    /*
     * Set the slot state last. Other threads may have a stale pointer to this slot and could try to
     * alter the state and other fields once they see the state cleared.
     */
    WT_PUBLISH(slot->slot_state, 0);
}

/*
 * __log_slot_close --
 *     Close out the slot the caller is using. The slot may already be closed or freed by another
 *     thread.
 */
static int
__log_slot_close(WT_SESSION_IMPL *session, WT_LOGSLOT *slot, bool *releasep, bool forced)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    int64_t end_offset, new_state, old_state;
#ifdef HAVE_DIAGNOSTIC
    uint64_t time_start, time_stop;
    int count;
#endif

    *releasep = false;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));
    conn = S2C(session);
    log = conn->log;
    if (slot == NULL)
        return (WT_NOTFOUND);
retry:
    old_state = slot->slot_state;
    /*
     * If this close is coming from a forced close and a thread is in the middle of using the slot,
     * return EBUSY. The caller can decide if retrying is necessary or not.
     */
    if (forced && WT_LOG_SLOT_INPROGRESS(old_state))
        return (__wt_set_return(session, EBUSY));
    /*
     * If someone else is switching out this slot we lost. Nothing to do but return. Return
     * WT_NOTFOUND anytime the given slot was processed by another closing thread. Only return 0
     * when we actually closed the slot.
     */
    if (WT_LOG_SLOT_CLOSED(old_state)) {
        WT_STAT_CONN_INCR(session, log_slot_close_race);
        return (WT_NOTFOUND);
    }
    /*
     * If someone completely processed this slot, we're done.
     */
    if (FLD_LOG_SLOT_ISSET((uint64_t)slot->slot_state, WT_LOG_SLOT_RESERVED)) {
        WT_STAT_CONN_INCR(session, log_slot_close_race);
        return (WT_NOTFOUND);
    }
    new_state = (old_state | WT_LOG_SLOT_CLOSE);
    /*
     * Close this slot. If we lose the race retry.
     */
    if (!__wt_atomic_casiv64(&slot->slot_state, old_state, new_state))
        goto retry;
    /*
     * We own the slot now. No one else can join. Set the end LSN.
     */
    WT_STAT_CONN_INCR(session, log_slot_closes);
    if (WT_LOG_SLOT_DONE(new_state))
        *releasep = true;
    WT_ASSIGN_LSN(&slot->slot_end_lsn, &slot->slot_start_lsn);
/*
 * A thread setting the unbuffered flag sets the unbuffered size after setting the flag. There could
 * be a delay between a thread setting the flag, a thread closing the slot, and the original thread
 * setting that value. If the state is unbuffered, wait for the unbuffered size to be set.
 */
#ifdef HAVE_DIAGNOSTIC
    count = 0;
    time_start = __wt_clock(session);
#endif
    if (WT_LOG_SLOT_UNBUFFERED_ISSET(old_state)) {
        while (slot->slot_unbuffered == 0) {
            WT_STAT_CONN_INCR(session, log_slot_close_unbuf);
            __wt_yield();
#ifdef HAVE_DIAGNOSTIC
            ++count;
            if (count > WT_MILLION) {
                time_stop = __wt_clock(session);
                if (WT_CLOCKDIFF_SEC(time_stop, time_start) > 10) {
                    __wt_errx(session,
                      "SLOT_CLOSE: Slot %" PRIu32 " Timeout unbuffered, state 0x%" PRIx64
                      " unbuffered %" PRId64,
                      (uint32_t)(slot - &log->slot_pool[0]), (uint64_t)slot->slot_state,
                      slot->slot_unbuffered);
                    __log_slot_dump(session);
                    __wt_abort(session);
                }
                count = 0;
            }
#endif
        }
    }

    end_offset = WT_LOG_SLOT_JOINED_BUFFERED(old_state) + slot->slot_unbuffered;
    slot->slot_end_lsn.l.offset += (uint32_t)end_offset;
    WT_STAT_CONN_INCRV(session, log_slot_consolidated, end_offset);
    /*
     * XXX Would like to change so one piece of code advances the LSN.
     */
    WT_ASSIGN_LSN(&log->alloc_lsn, &slot->slot_end_lsn);
    WT_ASSERT(session, log->alloc_lsn.l.file >= log->write_lsn.l.file);
    return (0);
}

/*
 * __log_slot_dirty_max_check --
 *     If we've passed the maximum of dirty system pages, schedule an asynchronous sync that will be
 *     performed when this slot is written.
 */
static void
__log_slot_dirty_max_check(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    WT_LSN *current, *last_sync;

    if (S2C(session)->log_dirty_max == 0)
        return;

    conn = S2C(session);
    log = conn->log;
    current = &slot->slot_release_lsn;

    if (__wt_log_cmp(&log->dirty_lsn, &log->sync_lsn) < 0)
        last_sync = &log->sync_lsn;
    else
        last_sync = &log->dirty_lsn;
    if (current->l.file == last_sync->l.file && current->l.offset > last_sync->l.offset &&
      current->l.offset - last_sync->l.offset > conn->log_dirty_max) {
        /* Schedule the asynchronous sync */
        F_SET(slot, WT_SLOT_SYNC_DIRTY);
        WT_ASSIGN_LSN(&log->dirty_lsn, &slot->slot_release_lsn);
    }
}

/*
 * __log_slot_new --
 *     Find a free slot and switch it as the new active slot. Must be called holding the slot lock.
 */
static int
__log_slot_new(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    int32_t i, pool_i;
#ifdef HAVE_DIAGNOSTIC
    uint64_t time_start, time_stop;
    int count;
#endif

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));
    conn = S2C(session);
    log = conn->log;
#ifdef HAVE_DIAGNOSTIC
    count = 0;
    time_start = __wt_clock(session);
#endif
    /*
     * Keep trying until we can find a free slot.
     */
    for (;;) {
        /*
         * Although this function is single threaded, multiple threads could be trying to set a new
         * active slot sequentially. If we find an active slot that is valid, return. This check is
         * inside the loop because this function may release the lock and needs to check again after
         * acquiring it again.
         */
        if ((slot = log->active_slot) != NULL && WT_LOG_SLOT_OPEN(slot->slot_state))
            return (0);
        /*
         * Rotate among the slots to lessen collisions.
         */
        WT_RET(WT_SESSION_CHECK_PANIC(session));
        for (i = 0, pool_i = log->pool_index; i < WT_SLOT_POOL; i++, pool_i++) {
            if (pool_i >= WT_SLOT_POOL)
                pool_i = 0;
            slot = &log->slot_pool[pool_i];
            if (slot->slot_state == WT_LOG_SLOT_FREE) {
                /*
                 * Acquire our starting position in the log file. Assume the full buffer size.
                 */
                WT_RET(__wt_log_acquire(session, log->slot_buf_size, slot));
                /*
                 * We have a new, initialized slot to use. Set it as the active slot.
                 */
                log->active_slot = slot;
                log->pool_index = pool_i;
                __log_slot_dirty_max_check(session, slot);
                return (0);
            }
        }
        /*
         * If we didn't find any free slots signal the worker thread. Release the lock so that any
         * threads waiting for it can acquire and possibly move things forward.
         */
        WT_STAT_CONN_INCR(session, log_slot_no_free_slots);
        __wt_cond_signal(session, conn->log_wrlsn_cond);
        __wt_spin_unlock(session, &log->log_slot_lock);
        __wt_yield();
        __wt_spin_lock(session, &log->log_slot_lock);
#ifdef HAVE_DIAGNOSTIC
        ++count;
        if (count > WT_MILLION) {
            time_stop = __wt_clock(session);
            if (WT_CLOCKDIFF_SEC(time_stop, time_start) > 10) {
                __wt_errx(session, "SLOT_NEW: Timeout free slot");
                __log_slot_dump(session);
                __wt_abort(session);
            }
            count = 0;
        }
#endif
    }
    /* NOTREACHED */
}

/*
 * __log_slot_switch_internal --
 *     Switch out the current slot and set up a new one.
 */
static int
__log_slot_switch_internal(WT_SESSION_IMPL *session, WT_MYSLOT *myslot, bool forced, bool *did_work)
{
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    uint32_t joined;
    bool free_slot, release;

    log = S2C(session)->log;
    release = false;
    slot = myslot->slot;

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));

    /*
     * If someone else raced us to closing this specific slot, we're done here.
     */
    if (slot != log->active_slot)
        return (0);
    /*
     * If the current active slot is unused and this is a forced switch, we're done. If this is a
     * non-forced switch we always switch because the slot could be part of an unbuffered operation.
     */
    joined = WT_LOG_SLOT_JOINED(slot->slot_state);
    if (joined == 0 && forced && !F_ISSET(log, WT_LOG_FORCE_NEWFILE)) {
        WT_STAT_CONN_INCR(session, log_force_write_skip);
        if (did_work != NULL)
            *did_work = false;
        return (0);
    }

    /*
     * We may come through here multiple times if we were not able to set up a new one. If we closed
     * it already, don't try to do it again but still set up the new slot.
     */
    if (!F_ISSET(myslot, WT_MYSLOT_CLOSE)) {
        ret = __log_slot_close(session, slot, &release, forced);
        /*
         * If close returns WT_NOTFOUND it means that someone else is processing the slot change.
         */
        if (ret == WT_NOTFOUND)
            return (0);
        WT_RET(ret);
        /*
         * Set that we have closed this slot because we may call in here multiple times if we retry
         * creating a new slot. Similarly set retain whether this slot needs releasing so that we
         * don't lose that information if we retry.
         */
        F_SET(myslot, WT_MYSLOT_CLOSE);
        if (release)
            F_SET(myslot, WT_MYSLOT_NEEDS_RELEASE);
    }
    /*
     * Now that the slot is closed, set up a new one so that joining threads don't have to wait on
     * writing the previous slot if we release it. Release after setting a new one.
     */
    WT_RET(__log_slot_new(session));
    F_CLR(myslot, WT_MYSLOT_CLOSE);
    if (F_ISSET(myslot, WT_MYSLOT_NEEDS_RELEASE)) {
        /*
         * The release here must be done while holding the slot lock. The reason is that a forced
         * slot switch needs to be sure that any earlier slot switches have completed, including
         * writing out the buffer contents of earlier slots.
         */
        WT_RET(__wt_log_release(session, slot, &free_slot));
        F_CLR(myslot, WT_MYSLOT_NEEDS_RELEASE);
        if (free_slot)
            __wt_log_slot_free(session, slot);
    }
    return (ret);
}

/*
 * __wt_log_slot_switch --
 *     Switch out the current slot and set up a new one.
 */
int
__wt_log_slot_switch(
  WT_SESSION_IMPL *session, WT_MYSLOT *myslot, bool retry, bool forced, bool *did_work)
{
    WT_DECL_RET;
    WT_LOG *log;

    log = S2C(session)->log;

    /*
     * !!! Since the WT_WITH_SLOT_LOCK macro is a do-while loop, the
     * compiler does not like it combined directly with the while loop
     * here.
     *
     * The loop conditional is a bit complex.  We have to retry if we
     * closed the slot but were unable to set up a new slot.  In that
     * case the flag indicating we have closed the slot will still be set.
     * We have to retry in that case regardless of the retry setting
     * because we are responsible for setting up the new slot.
     */
    do {
        WT_WITH_SLOT_LOCK(
          session, log, ret = __log_slot_switch_internal(session, myslot, forced, did_work));
        /*
         * If we get an unexpected error, we need to panic. If we cannot switch the slot because of
         * a real error, such as running out of space, there's nothing we can do.
         */
        if (ret == EBUSY) {
            WT_STAT_CONN_INCR(session, log_slot_switch_busy);
            __wt_yield();
        } else if (ret != 0)
            WT_TRET(__wt_panic(session, ret, "log slot switch fatal error"));

        WT_RET(WT_SESSION_CHECK_PANIC(session));
        if (F_ISSET(S2C(session), WT_CONN_CLOSING))
            break;
    } while (F_ISSET(myslot, WT_MYSLOT_CLOSE) || (retry && ret == EBUSY));
    return (ret);
}

/*
 * __wt_log_slot_init --
 *     Initialize the slot array.
 */
int
__wt_log_slot_init(WT_SESSION_IMPL *session, bool alloc)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    int32_t i;

    conn = S2C(session);
    log = conn->log;
    for (i = 0; i < WT_SLOT_POOL; i++)
        log->slot_pool[i].slot_state = WT_LOG_SLOT_FREE;

    /*
     * Allocate memory for buffers now that the arrays are setup. Separate this from the loop above
     * to make error handling simpler.
     */
    /*
     * !!! If the buffer size is too close to the log file size, we will
     * switch log files very aggressively.  Scale back the buffer for
     * small log file sizes.
     */
    if (alloc) {
        log->slot_buf_size =
          (uint32_t)WT_MIN((size_t)conn->log_file_max / 10, WT_LOG_SLOT_BUF_SIZE);
        for (i = 0; i < WT_SLOT_POOL; i++) {
            WT_ERR(__wt_buf_init(session, &log->slot_pool[i].slot_buf, log->slot_buf_size));
            F_SET(&log->slot_pool[i], WT_SLOT_INIT_FLAGS);
        }
        WT_STAT_CONN_SET(session, log_buffer_size, log->slot_buf_size * WT_SLOT_POOL);
    }
    /*
     * Set up the available slot from the pool the first time.
     */
    slot = &log->slot_pool[0];
    /*
     * We cannot initialize the release LSN in the activate function because that function can be
     * called after a log file switch. The release LSN is usually the same as the slot_start_lsn
     * except around a log file switch.
     */
    WT_ASSIGN_LSN(&slot->slot_release_lsn, &log->alloc_lsn);
    __wt_log_slot_activate(session, slot);
    log->active_slot = slot;
    log->pool_index = 0;

    if (0) {
err:
        while (--i >= 0)
            __wt_buf_free(session, &log->slot_pool[i].slot_buf);
    }
    return (ret);
}

/*
 * __wt_log_slot_destroy --
 *     Clean up the slot array on shutdown.
 */
int
__wt_log_slot_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    int64_t rel;
    int i;

    conn = S2C(session);
    log = conn->log;

    /*
     * Write out any remaining buffers. Free the buffer.
     */
    for (i = 0; i < WT_SLOT_POOL; i++) {
        slot = &log->slot_pool[i];
        if (!FLD_LOG_SLOT_ISSET((uint64_t)slot->slot_state, WT_LOG_SLOT_RESERVED)) {
            rel = WT_LOG_SLOT_RELEASED_BUFFERED(slot->slot_state);
            if (rel != 0)
                /* Writes are not throttled. */
                WT_RET(__wt_write(session, slot->slot_fh, slot->slot_start_offset, (size_t)rel,
                  slot->slot_buf.mem));
        }
        __wt_buf_free(session, &log->slot_pool[i].slot_buf);
    }
    return (0);
}

/*
 * __wt_log_slot_join --
 *     Join a consolidated logging slot.
 */
void
__wt_log_slot_join(WT_SESSION_IMPL *session, uint64_t mysize, uint32_t flags, WT_MYSLOT *myslot)
{
    WT_CONNECTION_IMPL *conn;
    WT_LOG *log;
    WT_LOGSLOT *slot;
    uint64_t time_start, time_stop, usecs;
    int64_t flag_state, new_state, old_state, released;
    int32_t join_offset, new_join, wait_cnt;
    bool closed, diag_yield, raced, slept, unbuffered, yielded;

    conn = S2C(session);
    log = conn->log;
    time_start = 0;

    WT_ASSERT(session, !FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SLOT));
    WT_ASSERT(session, mysize != 0);

    /*
     * There should almost always be a slot open.
     */
    unbuffered = yielded = false;
    closed = raced = slept = false;
    wait_cnt = 0;
#ifdef HAVE_DIAGNOSTIC
    diag_yield = (++log->write_calls % 7) == 0;
    if ((log->write_calls % WT_THOUSAND) == 0 || mysize > WT_LOG_SLOT_BUF_MAX) {
#else
    diag_yield = false;
    if (mysize > WT_LOG_SLOT_BUF_MAX) {
#endif
        unbuffered = true;
        F_SET(myslot, WT_MYSLOT_UNBUFFERED);
    }
    for (;;) {
        WT_BARRIER();
        slot = log->active_slot;
        old_state = slot->slot_state;
        if (WT_LOG_SLOT_OPEN(old_state)) {
            /*
             * Try to join our size into the existing size and atomically write it back into the
             * state.
             */
            flag_state = WT_LOG_SLOT_FLAGS(old_state);
            released = WT_LOG_SLOT_RELEASED(old_state);
            join_offset = WT_LOG_SLOT_JOINED(old_state);
            if (unbuffered)
                new_join = join_offset + WT_LOG_SLOT_UNBUFFERED;
            else
                new_join = join_offset + (int32_t)mysize;
            new_state = (int64_t)WT_LOG_SLOT_JOIN_REL(
              (int64_t)new_join, (int64_t)released, (int64_t)flag_state);

            /*
             * Braces used due to potential empty body warning.
             */
            if (diag_yield) {
                WT_DIAGNOSTIC_YIELD;
            }
            /*
             * Attempt to swap our size into the state.
             */
            if (__wt_atomic_casiv64(&slot->slot_state, old_state, new_state))
                break;
            WT_STAT_CONN_INCR(session, log_slot_races);
            raced = true;
        } else {
            WT_STAT_CONN_INCR(session, log_slot_active_closed);
            closed = true;
            ++wait_cnt;
        }
        if (!yielded)
            time_start = __wt_clock(session);
        yielded = true;
        /*
         * The slot is no longer open or we lost the race to update it. Yield and try again.
         */
        if (wait_cnt < WT_THOUSAND)
            __wt_yield();
        else {
            __wt_sleep(0, WT_THOUSAND);
            slept = true;
        }
    }
    /*
     * We joined this slot. Fill in our information to return to the caller.
     */
    if (!yielded)
        WT_STAT_CONN_INCR(session, log_slot_immediate);
    else {
        WT_STAT_CONN_INCR(session, log_slot_yield);
        time_stop = __wt_clock(session);
        usecs = WT_CLOCKDIFF_US(time_stop, time_start);
        WT_STAT_CONN_INCRV(session, log_slot_yield_duration, usecs);
        if (closed)
            WT_STAT_CONN_INCR(session, log_slot_yield_close);
        if (raced)
            WT_STAT_CONN_INCR(session, log_slot_yield_race);
        if (slept)
            WT_STAT_CONN_INCR(session, log_slot_yield_sleep);
    }
    if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
        F_SET(slot, WT_SLOT_SYNC_DIR);
    if (LF_ISSET(WT_LOG_FLUSH))
        F_SET(slot, WT_SLOT_FLUSH);
    if (LF_ISSET(WT_LOG_FSYNC))
        F_SET(slot, WT_SLOT_SYNC);
    if (F_ISSET(myslot, WT_MYSLOT_UNBUFFERED)) {
        WT_ASSERT(session, slot->slot_unbuffered == 0);
        WT_STAT_CONN_INCR(session, log_slot_unbuffered);
        slot->slot_unbuffered = (int64_t)mysize;
    }
    myslot->slot = slot;
    myslot->offset = join_offset;
    myslot->end_offset = (wt_off_t)((uint64_t)join_offset + mysize);
}

/*
 * __wt_log_slot_release --
 *     Each thread in a consolidated group releases its portion to signal it has completed copying
 *     its piece of the log into the memory buffer.
 */
int64_t
__wt_log_slot_release(WT_MYSLOT *myslot, int64_t size)
{
    WT_LOGSLOT *slot;
    wt_off_t cur_offset, my_start;
    int64_t my_size, rel_size;

    slot = myslot->slot;
    my_start = slot->slot_start_offset + myslot->offset;
    /*
     * We maintain the last starting offset within this slot. This is used to know the offset of the
     * last record that was written rather than the beginning record of the slot.
     */
    while ((cur_offset = slot->slot_last_offset) < my_start) {
        /*
         * Set our offset if we are larger.
         */
        if (__wt_atomic_casiv64(&slot->slot_last_offset, cur_offset, my_start))
            break;
        /*
         * If we raced another thread updating this, try again.
         */
        WT_BARRIER();
    }
    /*
     * Add my size into the state and return the new size.
     */
    rel_size = size;
    if (F_ISSET(myslot, WT_MYSLOT_UNBUFFERED))
        rel_size = WT_LOG_SLOT_UNBUFFERED;
    my_size = (int64_t)WT_LOG_SLOT_JOIN_REL((int64_t)0, rel_size, 0);
    return (__wt_atomic_addiv64(&slot->slot_state, my_size));
}

/*
 * __wt_log_slot_free --
 *     Free a slot back into the pool.
 */
void
__wt_log_slot_free(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
    /*
     * Make sure flags don't get retained between uses. We have to reset them here and not in
     * log_slot_activate because some flags (such as closing the file handle) may be set before we
     * initialize the rest of the slot.
     */
    WT_UNUSED(session);
    slot->flags = WT_SLOT_INIT_FLAGS;
    slot->slot_error = 0;
    slot->slot_state = WT_LOG_SLOT_FREE;
}
