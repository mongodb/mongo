/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * This file implements the consolidated array algorithm as described in
 * the paper:
 * Scalability of write-ahead logging on multicore and multisocket hardware
 * by Ryan Johnson, Ippokratis Pandis, Radu Stoica, Manos Athanassoulis
 * and Anastasia Ailamaki.
 *
 * It appeared in The VLDB Journal, DOI 10.1007/s00778-011-0260-8 and can
 * be found at:
 * http://infoscience.epfl.ch/record/170505/files/aether-smpfulltext.pdf
 */

/*
 * __wt_log_slot_init --
 *	Initialize the slot array.
 */
int
__wt_log_slot_init(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	uint32_t i;

	conn = S2C(session);
	log = conn->log;
	for (i = 0; i < SLOT_POOL; i++) {
		log->slot_pool[i].slot_state = WT_LOG_SLOT_FREE;
		log->slot_pool[i].slot_index = SLOT_INVALID_INDEX;
		/* TODO: Memory leak on error. */
		WT_RET(__wt_buf_init(session,
		    &log->slot_pool[i].slot_buf, WT_LOG_SLOT_BUF_INIT_SIZE));
		F_SET(&log->slot_pool[i], SLOT_BUFFERED);
	}

	/*
	 * Set up the available slots from the pool the first time.
	 */
	for (i = 0; i < SLOT_ACTIVE; i++) {
		slot = &log->slot_pool[i];
		slot->slot_index = i;
		slot->slot_state = WT_LOG_SLOT_READY;
		log->slot_array[i] = slot;
	}
	return (0);
}

/*
 * __wt_log_slot_join --
 *	Join a consolidated logging slot. Callers should be prepared to deal
 *	with a ENOMEM return - which indicates no slots could accommodate
 *	the log record.
 */
int
__wt_log_slot_join(WT_SESSION_IMPL *session, uint64_t mysize,
    uint32_t flags, WT_MYSLOT *myslotp)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int64_t cur_state, new_state, old_state;
	uint32_t allocated_slot, slot_grow_attempts;

	conn = S2C(session);
	log = conn->log;
	slot_grow_attempts = 0;
find_slot:
	allocated_slot = __wt_random() % SLOT_ACTIVE;
	slot = log->slot_array[allocated_slot];
	old_state = slot->slot_state;
join_slot:
	/*
	 * WT_LOG_SLOT_READY and higher means the slot is available for
	 * joining.  Any other state means it is in use and transitioning
	 * from the active array.
	 */
	if (old_state < WT_LOG_SLOT_READY) {
		WT_CSTAT_INCR(session, log_slot_transitions);
		goto find_slot;
	}
	/*
	 * Add in our size to the state and then atomically swap that
	 * into place if it is still the same value.
	 */
	new_state = old_state + (int64_t)mysize;
	if (new_state < old_state) {
		/* Our size doesn't fit here. */
		WT_CSTAT_INCR(session, log_slot_toobig);
		goto find_slot;
	}
	/*
	 * If the slot buffer isn't big enough to hold this update, mark
	 * the slot for a buffer size increase and find another slot.
	 * TODO: Solve this if our record isn't going to fit into any
	 * slots buffer and there aren't any other writes completing (i.e
	 * __log_release is never called, so the buffer never grows).
	 */
	if (new_state > (int64_t)slot->slot_buf.memsize) {
		F_SET(slot, SLOT_BUF_GROW);
		if (++slot_grow_attempts > 5)
			return (ENOMEM);
		goto find_slot;
	}
	cur_state = WT_ATOMIC_CAS_VAL(slot->slot_state, old_state, new_state);
	/*
	 * We lost a race to add our size into this slot.  Check the state
	 * and try again.
	 */
	if (cur_state != old_state) {
		old_state = cur_state;
		WT_CSTAT_INCR(session, log_slot_races);
		goto join_slot;
	}
	WT_ASSERT(session, myslotp != NULL);
	/*
	 * We joined this slot.  Fill in our information to return to
	 * the caller.
	 */
	WT_CSTAT_INCR(session, log_slot_joins);
	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(slot, SLOT_SYNC);
	myslotp->slot = slot;
	myslotp->offset = (off_t)old_state - WT_LOG_SLOT_READY;
	return (0);
}

/*
 * __wt_log_slot_close --
 *	Close a slot and do not allow any other threads to join this slot.
 *	Remove this from the active slot array and move a new slot from
 *	the pool into its place.  Set up the size of this group;
 *	Must be called with the logging spinlock held.
 */
int
__wt_log_slot_close(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *newslot;
	int64_t old_state;
	uint32_t pool_i;

	conn = S2C(session);
	log = conn->log;
retry:
	/*
	 * Find an unused slot in the pool.
	 */
	pool_i = log->pool_index;
	newslot = &log->slot_pool[pool_i];
	if (++log->pool_index >= SLOT_POOL)
		log->pool_index = 0;
	if (newslot->slot_state != WT_LOG_SLOT_FREE)
		goto retry;

	/*
	 * Sleep for a small amount of time to allow other threads a
	 * chance to consolidate.
	 */
	__wt_sleep(0, 25);

	/*
	 * Swap out the slot we're going to use and put a free one in the
	 * slot array in its place so that threads can use it right away.
	 */
	WT_CSTAT_INCR(session, log_slot_closes);
	newslot->slot_state = WT_LOG_SLOT_READY;
	newslot->slot_index = slot->slot_index;
	log->slot_array[newslot->slot_index] = &log->slot_pool[pool_i];
	old_state = WT_ATOMIC_STORE(slot->slot_state, WT_LOG_SLOT_PENDING);
	slot->slot_group_size = (uint64_t)(old_state - WT_LOG_SLOT_READY);
	/*
	 * Note that this statistic may be much bigger than in reality,
	 * especially when compared with the total bytes written in
	 * __log_fill.  The reason is that this size reflects any
	 * rounding up that is needed and the total bytes in __log_fill
	 * is the amount of user bytes.
	 */
	WT_CSTAT_INCRV(session,
	    log_slot_consolidated, (uint64_t)slot->slot_group_size);
	return (0);
}

/*
 * __wt_log_slot_notify --
 *	Notify all threads waiting for the state to be < WT_LOG_SLOT_DONE.
 */
int
__wt_log_slot_notify(WT_LOGSLOT *slot)
{
	slot->slot_state =
	    (int64_t)WT_LOG_SLOT_DONE - (int64_t)slot->slot_group_size;
	return (0);
}

/*
 * __wt_log_slot_wait --
 *	Wait for slot leader to allocate log area and tell us our log offset.
 */
int
__wt_log_slot_wait(WT_LOGSLOT *slot)
{
	while (slot->slot_state > WT_LOG_SLOT_DONE)
		__wt_yield();
	return (0);
}

/*
 * _wt_log_slot_release --
 *	Each thread in a consolidated group releases its portion to
 *	signal it has completed writing its piece of the log.
 */
int64_t
__wt_log_slot_release(WT_LOGSLOT *slot, uint64_t size)
{
	int64_t newsize;

	/*
	 * Add my size into the state.  When it reaches WT_LOG_SLOT_DONE
	 * all participatory threads have completed copying their piece.
	 */
	newsize = WT_ATOMIC_ADD(slot->slot_state, (int64_t)size);
	return (newsize);
}

/*
 * __wt_log_slot_free --
 *	Free a slot back into the pool.
 */
int
__wt_log_slot_free(WT_LOGSLOT *slot)
{
	slot->slot_state = WT_LOG_SLOT_FREE;
	return (0);
}

/*
 * __wt_log_slot_grow_buffers --
 *	Increase the buffer size of all available slots in the buffer pool.
 *	Go to some lengths to include active (but unused) slots to handle
 *	the case where all log write record sizes exceed the size of the
 *	active buffer.
 */
int
__wt_log_slot_grow_buffers(WT_SESSION_IMPL *session, int64_t newsize)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LOGSLOT *slot;
	int64_t orig_state;
	uint32_t i;

	conn = S2C(session);
	log = conn->log;
	/*
	 * Take the log slot lock to prevent other threads growing buffers
	 * at the same time. Could tighten the scope of this lock, or have
	 * a separate lock if there is contention.
	 */
	__wt_spin_lock(session, &log->log_slot_lock);
	for (i = 0; i < SLOT_POOL; i++) {
		slot = &log->slot_pool[i];
		if (slot->slot_state != WT_LOG_SLOT_FREE &&
		    slot->slot_state != WT_LOG_SLOT_READY)
			continue;
		orig_state = WT_ATOMIC_CAS_VAL(
		    slot->slot_state, WT_LOG_SLOT_FREE, WT_LOG_SLOT_PENDING);
		if (orig_state != WT_LOG_SLOT_FREE) {
			orig_state = WT_ATOMIC_CAS_VAL(slot->slot_state,
			    WT_LOG_SLOT_READY, WT_LOG_SLOT_PENDING);
			if (orig_state != WT_LOG_SLOT_READY)
				continue;
		}

		F_CLR(slot, SLOT_BUF_GROW);
		WT_ERR(__wt_buf_grow(session, &slot->slot_buf,
		    WT_MAX(slot->slot_buf.memsize * 2, newsize)));
		WT_ATOMIC_STORE(slot->slot_state, orig_state);
	}
err:	__wt_spin_unlock(session, &log->log_slot_lock);
	return (ret);
}
