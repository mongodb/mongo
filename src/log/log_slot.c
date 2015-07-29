/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
 * __wt_log_slot_switch --
 *	Find a new free slot and switch out the old active slot.
 */
int
__wt_log_slot_switch(WT_SESSION_IMPL *session, wt_off_t new_offset, int *rel)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	WT_LOGSLOT *current, *slot;
	int64_t new_state, old_state;
	int32_t i;

	conn = S2C(session);
	log = conn->log;
	current = log->active_slot;
	if (rel != NULL)
		*rel = 0;
	/*
	 * Keep trying until we can find a slot to switch.
	 */
	for (;;) {
		/*
		 * For now just restart at 0.  We could use log->pool_index
		 * if that is inefficient.
		 */
retry:
		for (i = 0; i < WT_SLOT_POOL; i++) {
			slot = &log->slot_pool[i];
			if (slot->slot_state == WT_LOG_SLOT_FREE) {
				old_state = current->slot_state;
				/*
				 * If someone else is switching out this slot
				 * we lost.  Nothing to do but return.
				 */
				new_state = (old_state | WT_LOG_SLOT_CLOSE);
				if (WT_LOG_SLOT_CLOSED(old_state))
					return (0);
				/*
				 * Close this slot.  If we lose the race retry.
				 */
				if (!WT_ATOMIC_CAS8(
				    current->slot_state, old_state, new_state))
					goto retry;
				/*
				 * We own the slot now.  No one else can join.
				 * Set the end LSN.  Then check for file change.
				 */
				if (WT_LOG_SLOT_DONE(new_state) && rel != NULL)
					*rel = 1;
				current->slot_end_lsn = current->slot_start_lsn;
				current->slot_end_lsn.offset += new_offset;
				log->alloc_lsn = current->slot_end_lsn;
				WT_RET(__wt_log_acquire(session,
				    WT_LOG_SLOT_BUF_SIZE, slot));
				/*
				 * We have a new, free slot to use.  Initialize.
				 */
				slot->slot_state = 0;
				slot->slot_unbuffered = 0;
				log->active_slot = slot;
				return (0);
			}
		}
		/*
		 * If we didn't find any free slots signal the worker thread.
		 */
		(void)__wt_cond_signal(session, conn->log_wrlsn_cond);
		__wt_yield();
	}
}

/*
 * __wt_log_slot_init --
 *	Initialize the slot array.
 */
int
__wt_log_slot_init(WT_SESSION_IMPL *session)
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
	 * Allocate memory for buffers now that the arrays are setup. Split
	 * this out to make error handling simpler.
	 */
	/*
	 * Cap the slot buffer to the log file size times two if needed.
	 * That means we try to fill to half the buffer but allow some
	 * extra space.
	 */
	log->slot_buf_size = (uint32_t)WT_MIN(
	    (size_t)conn->log_file_max, WT_LOG_SLOT_BUF_SIZE);
	for (i = 0; i < WT_SLOT_POOL; i++) {
		WT_ERR(__wt_buf_init(session,
		    &log->slot_pool[i].slot_buf, log->slot_buf_size));
		F_SET(&log->slot_pool[i], WT_SLOT_INIT_FLAGS);
	}
	WT_STAT_FAST_CONN_INCRV(session,
	    log_buffer_size, log->slot_buf_size * WT_SLOT_POOL);
	/*
	 * Set up the available slot from the pool the first time.
	 */
	slot = &log->slot_pool[0];
	slot->slot_state = 0;
	slot->slot_start_lsn = log->alloc_lsn;
	slot->slot_start_offset = log->alloc_lsn.offset;
	slot->slot_release_lsn = log->alloc_lsn;
	slot->slot_fh = log->log_fh;
	log->active_slot = slot;

	if (0) {
err:		while (--i >= 0)
			__wt_buf_free(session, &log->slot_pool[i].slot_buf);
	}
	return (ret);
}

/*
 * __wt_log_slot_destroy --
 *	Clean up the slot array on shutdown.
 */
int
__wt_log_slot_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;
	int i;

	conn = S2C(session);
	log = conn->log;

	for (i = 0; i < WT_SLOT_POOL; i++)
		__wt_buf_free(session, &log->slot_pool[i].slot_buf);
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
	int64_t flag_state, new_state, old_state;
	int32_t join_offset, new_join, released;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Make sure the length cannot overflow.
	 */
	if (mysize >= (uint64_t)WT_LOG_SLOT_MAXIMUM) {
		WT_STAT_FAST_CONN_INCR(session, log_slot_toobig);
		return (ENOMEM);
	}

	/*
	 * There should almost always be a slot open.
	 */
	for (;;) {
		WT_BARRIER();
		slot = log->active_slot;
		old_state = slot->slot_state;
		flag_state = WT_LOG_SLOT_FLAGS(old_state);
		released = WT_LOG_SLOT_RELEASED(old_state);
		join_offset = WT_LOG_SLOT_JOINED(old_state);
		new_join = join_offset + (int32_t)mysize;
		new_state = WT_LOG_SLOT_JOIN_REL(
		    (uint64_t)new_join, released, flag_state);

		/*
		 * Check if the slot is open for joining and we are able to
		 * swap in our size into the state.
		 */
		if (WT_LOG_SLOT_OPEN(old_state) &&
		    WT_ATOMIC_CAS8(slot->slot_state, old_state, new_state))
			break;
		else
			/*
			 * The slot is no longer open or we lost the race to
			 * update it.  Yield and try again.
			 */
			__wt_yield();
	}
	/*
	 * We joined this slot.  Fill in our information to return to
	 * the caller.
	 */
	if (mysize != 0)
		WT_STAT_FAST_CONN_INCR(session, log_slot_joins);
	if (LF_ISSET(WT_LOG_DSYNC | WT_LOG_FSYNC))
		F_SET(slot, WT_SLOT_SYNC_DIR);
	if (LF_ISSET(WT_LOG_FSYNC))
		F_SET(slot, WT_SLOT_SYNC);
	myslotp->slot = slot;
	myslotp->offset = join_offset;
	myslotp->end_offset = join_offset + mysize;
	return (0);
}

/*
 * __wt_log_slot_release --
 *	Each thread in a consolidated group releases its portion to
 *	signal it has completed writing its piece of the log.
 */
int64_t
__wt_log_slot_release(WT_LOGSLOT *slot, int64_t size)
{
	int64_t my_size, newsize;

	/*
	 * Add my size into the state and return the new size.
	 */
	my_size = WT_LOG_SLOT_JOIN_REL((uint64_t)0, size, 0);
	newsize = WT_ATOMIC_ADD8(slot->slot_state, my_size);
	return (newsize);
}

/*
 * __wt_log_slot_free --
 *	Free a slot back into the pool.
 */
int
__wt_log_slot_free(WT_SESSION_IMPL *session, WT_LOGSLOT *slot)
{

	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	/*
	 * Make sure flags don't get retained between uses.
	 * We have to reset them them here because multiple threads may
	 * change the flags when joining the slot.
	 */
	slot->flags = WT_SLOT_INIT_FLAGS;
	slot->slot_error = 0;
	slot->slot_state = WT_LOG_SLOT_FREE;
	WT_ASSERT(session, slot != log->active_slot);
	return (0);
}
