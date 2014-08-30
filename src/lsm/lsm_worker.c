/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_worker_general_op(
    WT_SESSION_IMPL *, WT_LSM_WORKER_ARGS *, int *);
static void * __lsm_worker(void *);

/*
 * __wt_lsm_worker_start --
 *	A wrapper around the LSM worker thread start
 */
int
__wt_lsm_worker_start(WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *args)
{
	return (__wt_thread_create(session, &args->tid, __lsm_worker, args));
}

/*
 * __lsm_worker_general_op --
 *	Execute a single bloom, drop or flush work unit
 */
static int
__lsm_worker_general_op(
    WT_SESSION_IMPL *session, WT_LSM_WORKER_ARGS *cookie, int *completed)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORK_UNIT *entry;

	*completed = 0;
	if (!F_ISSET(cookie, WT_LSM_WORK_FLUSH) &&
	    !F_ISSET(cookie, WT_LSM_WORK_DROP) &&
	    !F_ISSET(cookie, WT_LSM_WORK_BLOOM))
	    return (WT_NOTFOUND);

	if ((ret = __wt_lsm_manager_pop_entry(session,
	    cookie->flags, &entry)) != 0 || entry == NULL)
		return (ret);

	if (entry->flags == WT_LSM_WORK_FLUSH) {
		WT_ERR(__wt_lsm_get_chunk_to_flush(
		    session, entry->lsm_tree, &chunk));
		if (chunk != NULL) {
			ret = __wt_lsm_checkpoint_chunk(
			    session, entry->lsm_tree, chunk);
			WT_ASSERT(session, chunk->refcnt > 0);
			(void)WT_ATOMIC_SUB(chunk->refcnt, 1);
			WT_ERR(ret);
		}
	} else if (entry->flags == WT_LSM_WORK_DROP)
		WT_ERR(__wt_lsm_free_chunks(session, entry->lsm_tree));
	else if (entry->flags == WT_LSM_WORK_BLOOM) {
		WT_ERR(__wt_lsm_bloom_work(session, entry->lsm_tree));
		WT_ERR(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_MERGE, entry->lsm_tree));
	}
	*completed = 1;

err:	__wt_lsm_manager_free_work_unit(session, entry);
	return (ret);
}

/*
 * __lsm_worker --
 *	A thread that executes work units for all open LSM trees.
 */
static void *
__lsm_worker(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_WORK_UNIT *entry;
	WT_LSM_WORKER_ARGS *cookie;
	WT_SESSION_IMPL *session;
	int ran;

	cookie = (WT_LSM_WORKER_ARGS *)arg;
	session = cookie->session;
	conn = S2C(session);

	entry = NULL;
	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Don't busy wait if there aren't any LSM trees. */
		if (TAILQ_EMPTY(&conn->lsmqh)) {
			__wt_sleep(0, 10000);
			continue;
		}

		/* Switches are always a high priority */
		while (F_ISSET(cookie, WT_LSM_WORK_SWITCH) &&
		    (ret = __wt_lsm_manager_pop_entry(
		    session, WT_LSM_WORK_SWITCH, &entry)) == 0 &&
		    entry != NULL) {
			/*
			 * Don't exit the switch thread because a single
			 * switch fails. Keep trying until we are told to
			 * shut down.
			 */
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_lsm_tree_switch(session, entry->lsm_tree));

			__wt_lsm_manager_free_work_unit(session, entry);
			entry = NULL;

			if (ret == EBUSY)
				ret = 0;
			WT_ERR(ret);
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		ret = __lsm_worker_general_op(session, cookie, &ran);
		if (ret == EBUSY || ret == WT_NOTFOUND)
			ret = 0;
		WT_ERR(ret);

		if (F_ISSET(cookie, WT_LSM_WORK_MERGE) &&
		    (ret = __wt_lsm_manager_pop_entry(
		    session, WT_LSM_WORK_MERGE, &entry)) == 0 &&
		    entry != NULL) {
			WT_ASSERT(session, entry->flags == WT_LSM_WORK_MERGE);
			ret = __wt_lsm_merge(session,
			    entry->lsm_tree, cookie->id);
			if (ret == WT_NOTFOUND) {
				F_CLR(entry->lsm_tree, WT_LSM_TREE_COMPACTING);
				ret = 0;
			} else if (ret == EBUSY)
				ret = 0;
			/* Clear any state */
			WT_CLEAR_BTREE_IN_SESSION(session);
			__wt_lsm_manager_free_work_unit(session, entry);
			entry = NULL;
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);
	}

	if (ret != 0) {
err:		__wt_lsm_manager_free_work_unit(session, entry);
		__wt_err(session, ret,
		    "Error in LSM worker thread %d", cookie->id);
	}
	return (NULL);
}
