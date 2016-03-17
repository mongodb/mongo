/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_worker_general_op(
    WT_SESSION_IMPL *, WT_LSM_WORKER_ARGS *, bool *);
static WT_THREAD_RET __lsm_worker(void *);

/*
 * __wt_lsm_worker_start --
 *	A wrapper around the LSM worker thread start.
 */
int
__wt_lsm_worker_start(WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *args)
{
	WT_RET(__wt_verbose(session, WT_VERB_LSM_MANAGER,
	    "Start LSM worker %u type %#" PRIx32, args->id, args->type));
	return (__wt_thread_create(session, &args->tid, __lsm_worker, args));
}

/*
 * __lsm_worker_general_op --
 *	Execute a single bloom, drop or flush work unit.
 */
static int
__lsm_worker_general_op(
    WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *cookie, bool *completed)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORK_UNIT *entry;
	bool force;

	*completed = false;
	/*
	 * Return if this thread cannot process a bloom, drop or flush.
	 */
	if (!FLD_ISSET(cookie->type,
	    WT_LSM_WORK_BLOOM | WT_LSM_WORK_DROP | WT_LSM_WORK_FLUSH))
		return (WT_NOTFOUND);

	if ((ret = __wt_lsm_manager_pop_entry(session,
	    cookie->type, &entry)) != 0 || entry == NULL)
		return (ret);

	if (entry->type == WT_LSM_WORK_FLUSH) {
		force = F_ISSET(entry, WT_LSM_WORK_FORCE);
		F_CLR(entry, WT_LSM_WORK_FORCE);
		WT_ERR(__wt_lsm_get_chunk_to_flush(session,
		    entry->lsm_tree, force, &chunk));
		/*
		 * If we got a chunk to flush, checkpoint it.
		 */
		if (chunk != NULL) {
			WT_ERR(__wt_verbose(session, WT_VERB_LSM,
			    "Flush%s chunk %" PRIu32 " %s",
			    force ? " w/ force" : "", chunk->id, chunk->uri));
			ret = __wt_lsm_checkpoint_chunk(
			    session, entry->lsm_tree, chunk);
			WT_ASSERT(session, chunk->refcnt > 0);
			(void)__wt_atomic_sub32(&chunk->refcnt, 1);
			WT_ERR(ret);
		}
	} else if (entry->type == WT_LSM_WORK_DROP)
		WT_ERR(__wt_lsm_free_chunks(session, entry->lsm_tree));
	else if (entry->type == WT_LSM_WORK_BLOOM)
		WT_ERR(__wt_lsm_work_bloom(session, entry->lsm_tree));
	*completed = true;

err:	__wt_lsm_manager_free_work_unit(session, entry);
	return (ret);
}

/*
 * __lsm_worker --
 *	A thread that executes work units for all open LSM trees.
 */
static WT_THREAD_RET
__lsm_worker(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_WORK_UNIT *entry;
	WT_LSM_WORKER_ARGS *cookie;
	WT_SESSION_IMPL *session;
	bool progress, ran;

	cookie = (WT_LSM_WORKER_ARGS *)arg;
	session = cookie->session;
	conn = S2C(session);

	entry = NULL;
	while (F_ISSET(conn, WT_CONN_SERVER_RUN) &&
	    F_ISSET(cookie, WT_LSM_WORKER_RUN)) {
		progress = false;

		/*
		 * Workers process the different LSM work queues.  Some workers
		 * can handle several or all work unit types.  So the code is
		 * prioritized so important operations happen first.
		 * Switches are the highest priority.
		 */
		while (FLD_ISSET(cookie->type, WT_LSM_WORK_SWITCH) &&
		    (ret = __wt_lsm_manager_pop_entry(
		    session, WT_LSM_WORK_SWITCH, &entry)) == 0 &&
		    entry != NULL)
			WT_ERR(
			    __wt_lsm_work_switch(session, &entry, &progress));
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		/*
		 * Next the general operations.
		 */
		ret = __lsm_worker_general_op(session, cookie, &ran);
		if (ret == EBUSY || ret == WT_NOTFOUND)
			ret = 0;
		WT_ERR(ret);
		progress = progress || ran;

		/*
		 * Finally see if there is any merge work we can do.  This is
		 * last because the earlier operations may result in adding
		 * merge work to the queue.
		 */
		if (FLD_ISSET(cookie->type, WT_LSM_WORK_MERGE) &&
		    (ret = __wt_lsm_manager_pop_entry(
		    session, WT_LSM_WORK_MERGE, &entry)) == 0 &&
		    entry != NULL) {
			WT_ASSERT(session, entry->type == WT_LSM_WORK_MERGE);
			ret = __wt_lsm_merge(session,
			    entry->lsm_tree, cookie->id);
			if (ret == WT_NOTFOUND) {
				F_CLR(entry->lsm_tree, WT_LSM_TREE_COMPACTING);
				ret = 0;
			} else if (ret == EBUSY || ret == EINTR)
				ret = 0;

			/* Paranoia: clear session state. */
			session->dhandle = NULL;

			__wt_lsm_manager_free_work_unit(session, entry);
			entry = NULL;
			progress = true;
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		/* Don't busy wait if there was any work to do. */
		if (!progress) {
			WT_ERR(
			    __wt_cond_wait(session, cookie->work_cond, 10000));
			continue;
		}
	}

	if (ret != 0) {
err:		__wt_lsm_manager_free_work_unit(session, entry);
		WT_PANIC_MSG(session, ret,
		    "Error in LSM worker thread %u", cookie->id);
	}
	return (WT_THREAD_RET_VALUE);
}
