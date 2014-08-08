/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_manager_aggressive_update(WT_SESSION_IMPL *, WT_LSM_TREE *);
static int __lsm_manager_pop_entry(
    WT_SESSION_IMPL *, uint32_t , WT_LSM_WORK_UNIT **);
static int __lsm_manager_run_server(WT_SESSION_IMPL *);
static int __lsm_manager_worker_setup(WT_SESSION_IMPL *);

static void * __lsm_worker(void *);
static void * __lsm_worker_manager(void *);

/*
 * __wt_lsm_manager_start --
 *	Start the LSM management infrastructure.
 */
int
__wt_lsm_manager_start(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_SESSION_IMPL *worker_session;
	uint32_t i;

	manager = &S2C(session)->lsm_manager;

	/*
	 * We need at least a manager, a switch thread and a generic
	 * worker.
	 */
	WT_ASSERT(session, manager->lsm_workers_max > 2);
	WT_RET(__wt_calloc_def(session,
	    manager->lsm_workers_max, &manager->lsm_worker_tids));
	WT_ERR(__wt_calloc_def(session,
	    manager->lsm_workers_max, &manager->lsm_worker_sessions));

	/*
	 * Open sessions for all potential worker threads here - it's not
	 * safe to have worker threads open/close sessions themselves.
	 * All the LSM worker threads do their operations on read-only
	 * files. Use read-uncommitted isolation to avoid keeping
	 * updates in cache unnecessarily.
	 */
	for (i = 0; i < manager->lsm_workers_max; i++) {
		WT_ERR(__wt_open_internal_session(
		    S2C(session), "lsm-worker", 1, 0, &worker_session));
		worker_session->isolation = TXN_ISO_READ_UNCOMMITTED;
		manager->lsm_worker_sessions[i] = worker_session;
	}

	/* Start the LSM manager thread. */
	WT_ERR(__wt_thread_create(session, &manager->lsm_worker_tids[0],
	    __lsm_worker_manager, manager->lsm_worker_sessions[0]));

	F_SET(S2C(session), WT_CONN_SERVER_LSM);

	if (0) {
err:		for (i = 0;
		    (worker_session = manager->lsm_worker_sessions[i]) != NULL;
		    i++)
			WT_TRET((&worker_session->iface)->close(
			    &worker_session->iface, NULL));
		__wt_free(session, manager->lsm_worker_sessions);
		__wt_free(session, manager->lsm_worker_tids);
	}
	return (ret);
}

/*
 * __lsm_manager_free_work_unit --
 *	Release an LSM tree work unit.
 */
static int
__lsm_manager_free_work_unit(WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT *entry)
{
	if (entry == NULL)
		return (0);
	WT_ASSERT(session, entry->lsm_tree->queue_ref > 0);
	(void)WT_ATOMIC_SUB(entry->lsm_tree->queue_ref, 1);
	__wt_free(session, entry);
	return (0);
}

/*
 * __wt_lsm_manager_destroy --
 *	Destroy the LSM manager threads and subsystem.
 */
int
__wt_lsm_manager_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *current, *next;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;
	uint32_t i;
	uint64_t removed;

	session = conn->default_session;
	manager = &conn->lsm_manager;
	removed = 0;

	if (manager->lsm_worker_tids == NULL)
		return (0);

	/* Wait for the server to notice and wrap up. */
	while (F_ISSET(conn, WT_CONN_SERVER_LSM))
		__wt_yield();

	/* Clean up open LSM handles. */
	ret = __wt_lsm_tree_close_all(conn->default_session);

	WT_TRET(__wt_thread_join(session, manager->lsm_worker_tids[0]));
	manager->lsm_worker_tids[0] = 0;

	/* Release memory from any operations left on the queue. */
	for (current = TAILQ_FIRST(&manager->switchqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		TAILQ_REMOVE(&manager->switchqh, current, q);
		++removed;
		__lsm_manager_free_work_unit(session, current);
	}
	for (current = TAILQ_FIRST(&manager->appqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		TAILQ_REMOVE(&manager->appqh, current, q);
		++removed;
		__lsm_manager_free_work_unit(session, current);
	}
	for (current = TAILQ_FIRST(&manager->managerqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		TAILQ_REMOVE(&manager->managerqh, current, q);
		++removed;
		__lsm_manager_free_work_unit(session, current);
	}

	/* Close all LSM worker sessions. */
	for (i = 0; i < manager->lsm_workers_max; i++) {
		wt_session = &manager->lsm_worker_sessions[i]->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		manager->lsm_worker_sessions[i] = NULL;
	}

	WT_STAT_FAST_CONN_INCRV(session, lsm_work_units_discarded, removed);

	__wt_spin_destroy(session, &manager->switch_lock);
	__wt_spin_destroy(session, &manager->app_lock);
	__wt_spin_destroy(session, &manager->manager_lock);

	__wt_free(session, manager->lsm_worker_tids);
	__wt_free(session, manager->lsm_worker_sessions);

	return (ret);
}

/*
 * __lsm_manager_aggressive_update --
 *	Update the merge aggressiveness for a single LSM tree.
 */
static int
__lsm_manager_aggressive_update(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	struct timespec now;
	u_int chunk_wait, old_aggressive, stallms;

	WT_RET(__wt_epoch(session, &now));
	stallms = WT_TIMEDIFF(now, lsm_tree->last_flush_ts) / WT_MILLION;
	/*
	 * Get aggressive if more than enough chunks for a merge should have
	 * been created by now. Use 10 seconds as a default if we don't have an
	 * estimate.
	 */
	chunk_wait = stallms / (lsm_tree->chunk_fill_ms == 0 ?
	    10000 : lsm_tree->chunk_fill_ms);
	old_aggressive = lsm_tree->merge_aggressiveness;
	lsm_tree->merge_aggressiveness = chunk_wait / lsm_tree->merge_min;

	if (lsm_tree->merge_aggressiveness > old_aggressive)
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
		     "LSM merge got aggressive (%u), "
		     "%u / %" PRIu64,
		     lsm_tree->merge_aggressiveness, stallms,
		     lsm_tree->chunk_fill_ms));
	return (0);
}

/*
 * __lsm_manager_worker_setup --
 *	Do setup owned by the LSM manager thread includes starting the worker
 *	threads.
 */
static int
__lsm_manager_worker_setup(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LSM_MANAGER *manager;
	WT_LSM_WORKER_ARGS *worker_args;

	conn = S2C(session);
	manager = &conn->lsm_manager;

	WT_ASSERT(session, manager->lsm_workers == 1);

	/* Setup the spin locks for the queues. */
	WT_RET(__wt_spin_init(
	    session, &manager->app_lock, "LSM application queue lock"));
	WT_RET(__wt_spin_init(
	    session, &manager->manager_lock, "LSM manager queue lock"));
	WT_RET(__wt_spin_init(
	    session, &manager->switch_lock, "LSM switch queue lock"));

	/* Freed by the worker thread when it shuts down */
	WT_RET(__wt_calloc_def(session, 1, &worker_args));
	worker_args->session = manager->lsm_worker_sessions[1];
	worker_args->id = manager->lsm_workers++;
	worker_args->flags = WT_LSM_WORK_SWITCH;
	/* Start the switch thread. */
	WT_RET(__wt_thread_create(session, &manager->lsm_worker_tids[1],
	    __lsm_worker, worker_args));

	/*
	 * Start the remaining worker threads.
	 * TODO: This should get more sophisticated in the future - only
	 * launching as many worker threads as are required to keep up with
	 * demand.
	 */
	for (; manager->lsm_workers < manager->lsm_workers_max;
	    manager->lsm_workers++) {
		/* Freed by the worker thread when it shuts down */
		WT_RET(__wt_calloc_def(session, 1, &worker_args));
		worker_args->session =
		    manager->lsm_worker_sessions[manager->lsm_workers];
		worker_args->id = manager->lsm_workers;
		worker_args->flags =
		    WT_LSM_WORK_BLOOM |
		    WT_LSM_WORK_DROP |
		    WT_LSM_WORK_FLUSH |
		    WT_LSM_WORK_SWITCH;
		/*
		 * Only allow half of the threads to run merges to avoid all
		 * all workers getting stuck in long-running merge operations.
		 * Make sure the first worker is allowed, so that there is at
		 * least one thread capable of running merges.
		 */
		if (manager->lsm_workers % 2 == 1)
			F_SET(worker_args, WT_LSM_WORK_MERGE);
		WT_RET(__wt_thread_create(session,
		    &manager->lsm_worker_tids[manager->lsm_workers],
		    __lsm_worker, worker_args));
	}
	return (0);
}

/*
 * __lsm_manager_worker_shutdown --
 *	Shutdown the LSM manager and worker threads.
 */
static int
__lsm_manager_worker_shutdown(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_MANAGER *manager;
	u_int i;

	manager = &S2C(session)->lsm_manager;

	/*
	 * Wait for the rest of the LSM workers to shutdown. Stop at index
	 * one - since we (the manager) are at index 0.
	 */
	for (i = 1; i < manager->lsm_workers; i++) {
		WT_ASSERT(session, manager->lsm_worker_tids[i] != 0);
		WT_ASSERT(session, manager->lsm_worker_sessions[i] != NULL);
		WT_TRET(__wt_thread_join(
		    session, manager->lsm_worker_tids[i]));
		manager->lsm_worker_tids[i] = 0;
	}
	return (ret);
}

/*
 * __lsm_manager_run_server --
 *	Run manager thread operations.
 */
static int
__lsm_manager_run_server(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LSM_TREE *lsm_tree;
	struct timespec now;
	u_int fillms, pushms;

	conn = S2C(session);
	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		if (TAILQ_EMPTY(&conn->lsmqh)) {
			__wt_sleep(0, 10000);
			continue;
		}
		__wt_sleep(0, 10000);
		TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
				continue;
			WT_RET(__lsm_manager_aggressive_update(
			    session, lsm_tree));
			WT_RET(__wt_epoch(session, &now));
			pushms = lsm_tree->work_push_ts.tv_sec == 0 ? 0 :
			    WT_TIMEDIFF(
			    now, lsm_tree->work_push_ts) / WT_MILLION;
			fillms = 3 * lsm_tree->chunk_fill_ms;
			if (fillms == 0)
				fillms = 10000;
			/*
			 * If the tree appears to not be triggering enough
			 * LSM maintenance, help it out. Additional work units
			 * don't hurt, and can be necessary if some work
			 * units aren't completed for some reason.
			 * If the tree hasn't been modified, and there are
			 * more than 1 chunks - try to get the tree smaller
			 * so queries run faster.
			 * If we are getting aggressive - ensure there are
			 * enough work units that we can get chunks merged.
			 * If we aren't pushing enough work units, compared
			 * to how often new chunks are being created add some
			 * more.
			 */
			if ((!lsm_tree->modified && lsm_tree->nchunks > 1) ||
			    lsm_tree->merge_aggressiveness > 3 ||
			    (lsm_tree->queue_ref == 0 && lsm_tree->nchunks > 1) ||
			    pushms > fillms) {
				WT_RET(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_SWITCH, lsm_tree));
				WT_RET(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_FLUSH, lsm_tree));
				WT_RET(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_BLOOM, lsm_tree));
				WT_RET(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_MERGE, lsm_tree));
			}
			if (lsm_tree->queue_ref == 0 &&
			    lsm_tree->nold_chunks != 0) {
				WT_RET(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_DROP, lsm_tree));
			}
		}
	}

	return (0);
}

/*
 * __lsm_worker_manager --
 *	A thread that manages all open LSM trees, and the shared LSM worker
 *	threads.
 */
static void *
__lsm_worker_manager(void *arg)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)arg;

	WT_ERR(__lsm_manager_worker_setup(session));
	WT_ERR(__lsm_manager_run_server(session));
	WT_ERR(__lsm_manager_worker_shutdown(session));

	if (ret != 0) {
err:		__wt_err(session, ret, "LSM worker manager thread error");
	}
	F_CLR(S2C(session), WT_CONN_SERVER_LSM);
	return (NULL);
}

/*
 * __wt_lsm_manager_clear_tree --
 *	Remove all entries for a tree from the LSM manager queues. This
 *	introduces an inefficiency if LSM trees are being opened and closed
 *	regularly.
 */
int
__wt_lsm_manager_clear_tree(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *current, *next;
	uint64_t removed;

	manager = &S2C(session)->lsm_manager;
	removed = 0;

	/* Clear out the tree from the switch queue */
	__wt_spin_lock(session, &manager->switch_lock);

	/* Structure the loop so that it's safe to free as we iterate */
	for (current = TAILQ_FIRST(&manager->switchqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;
		++removed;
		TAILQ_REMOVE(&manager->switchqh, current, q);
		__lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->switch_lock);
	/* Clear out the tree from the application queue */
	__wt_spin_lock(session, &manager->app_lock);
	for (current = TAILQ_FIRST(&manager->appqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;
		++removed;
		TAILQ_REMOVE(&manager->appqh, current, q);
		__lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->app_lock);
	/* Clear out the tree from the manager queue */
	__wt_spin_lock(session, &manager->manager_lock);
	for (current = TAILQ_FIRST(&manager->managerqh);
	    current != NULL; current = next) {
		next = TAILQ_NEXT(current, q);
		if (current->lsm_tree != lsm_tree)
			continue;
		++removed;
		TAILQ_REMOVE(&manager->managerqh, current, q);
		__lsm_manager_free_work_unit(session, current);
	}
	__wt_spin_unlock(session, &manager->manager_lock);
	WT_STAT_FAST_CONN_INCRV(session, lsm_work_units_discarded, removed);
	return (0);
}

/*
 * __lsm_manager_pop_entry --
 *	Retrieve the head of the queue, if it matches the requested work
 *	unit type.
 */
static int
__lsm_manager_pop_entry(
    WT_SESSION_IMPL *session, uint32_t type, WT_LSM_WORK_UNIT **entryp)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;
	*entryp = NULL;
	entry = NULL;

	switch (type) {
	case WT_LSM_WORK_SWITCH:
		if (TAILQ_EMPTY(&manager->switchqh))
			return (0);

		__wt_spin_lock(session, &manager->switch_lock);
		if (!TAILQ_EMPTY(&manager->switchqh)) {
			entry = TAILQ_FIRST(&manager->switchqh);
			WT_ASSERT(session, entry != NULL);
			TAILQ_REMOVE(&manager->switchqh, entry, q);
		}
		__wt_spin_unlock(session, &manager->switch_lock);
		break;
	case WT_LSM_WORK_MERGE:
		if (TAILQ_EMPTY(&manager->managerqh))
			return (0);

		__wt_spin_lock(session, &manager->manager_lock);
		if (!TAILQ_EMPTY(&manager->managerqh)) {
			entry = TAILQ_FIRST(&manager->managerqh);
			WT_ASSERT(session, entry != NULL);
			if (F_ISSET(entry, type))
				TAILQ_REMOVE(&manager->managerqh, entry, q);
			else
				entry = NULL;
		}

		__wt_spin_unlock(session, &manager->manager_lock);
		break;
	default:
		/*
		 * The app queue is the only one that has multiple different
		 * work unit types, allow a request for a variety.
		 */
		WT_ASSERT(session, FLD_ISSET(type, WT_LSM_WORK_BLOOM) ||
		    FLD_ISSET(type, WT_LSM_WORK_DROP) ||
		    FLD_ISSET(type, WT_LSM_WORK_FLUSH));
		if (TAILQ_EMPTY(&manager->appqh))
			return (0);

		__wt_spin_lock(session, &manager->app_lock);
		if (!TAILQ_EMPTY(&manager->appqh)) {
			entry = TAILQ_FIRST(&manager->appqh);
			WT_ASSERT(session, entry != NULL);
			if (FLD_ISSET(type, entry->flags))
				TAILQ_REMOVE(&manager->appqh, entry, q);
			else
				entry = NULL;
		}
		__wt_spin_unlock(session, &manager->app_lock);
		break;
	}
	if (entry != NULL)
		WT_STAT_FAST_CONN_INCR(session, lsm_work_units_done);
	*entryp = entry;
	return (0);
}

/*
 * __wt_lsm_manager_push_entry --
 *	Add an entry to the end of the switch queue.
 */
int
__wt_lsm_manager_push_entry(
    WT_SESSION_IMPL *session, uint32_t type, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_MANAGER *manager;
	WT_LSM_WORK_UNIT *entry;

	manager = &S2C(session)->lsm_manager;

	WT_RET(__wt_epoch(session, &lsm_tree->work_push_ts));

	WT_RET(__wt_calloc_def(session, 1, &entry));
	entry->flags = type;
	entry->lsm_tree = lsm_tree;
	(void)WT_ATOMIC_ADD(lsm_tree->queue_ref, 1);
	WT_STAT_FAST_CONN_INCR(session, lsm_work_units_created);

	switch (type) {
	case WT_LSM_WORK_SWITCH:
		__wt_spin_lock(session, &manager->switch_lock);
		TAILQ_INSERT_TAIL(&manager->switchqh, entry, q);
		__wt_spin_unlock(session, &manager->switch_lock);
		break;
	case WT_LSM_WORK_BLOOM:
	case WT_LSM_WORK_DROP:
	case WT_LSM_WORK_FLUSH:
		__wt_spin_lock(session, &manager->app_lock);
		TAILQ_INSERT_TAIL(&manager->appqh, entry, q);
		__wt_spin_unlock(session, &manager->app_lock);
		break;
	case WT_LSM_WORK_MERGE:
		__wt_spin_lock(session, &manager->manager_lock);
		TAILQ_INSERT_TAIL(&manager->managerqh, entry, q);
		__wt_spin_unlock(session, &manager->manager_lock);
		break;
	}

	return (0);
}

/*
 * __lsm_get_chunk_to_flush --
 *	Find and pin a chunk in the LSM tree that is likely to need flushing.
 */
static int
__lsm_get_chunk_to_flush(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK **chunkp)
{
	u_int i;

	*chunkp = NULL;

	WT_ASSERT(session, lsm_tree->queue_ref > 0);
	WT_RET(__wt_lsm_tree_lock(session, lsm_tree, 0));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
		return (__wt_lsm_tree_unlock(session, lsm_tree));

	for (i = 0; i < lsm_tree->nchunks - 1; i++) {
		if (!F_ISSET(lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK)) {
			(void)WT_ATOMIC_ADD(lsm_tree->chunk[i]->refcnt, 1);
			*chunkp = lsm_tree->chunk[i];
			break;
		}
	}

	WT_RET(__wt_lsm_tree_unlock(session, lsm_tree));

	return (0);
}

/*
 * __lsm_worker --
 *	A thread that executes switches for all open LSM trees.
 */
static void *
__lsm_worker(void *arg) {
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORK_UNIT *entry;
	WT_LSM_WORKER_ARGS *cookie;
	WT_SESSION_IMPL *session;
	int flushed;

	cookie = (WT_LSM_WORKER_ARGS *)arg;
	session = cookie->session;
	conn = S2C(session);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Don't busy wait if there aren't any LSM trees. */
		if (TAILQ_EMPTY(&conn->lsmqh)) {
			__wt_sleep(0, 10000);
			continue;
		}

		/* Switches are always a high priority */
		while (F_ISSET(cookie, WT_LSM_WORK_SWITCH) &&
		    (ret = __lsm_manager_pop_entry(
		    session, WT_LSM_WORK_SWITCH, &entry)) == 0 &&
		    entry != NULL) {
			/*
			 * Don't exit the switch thread because a single
			 * switch fails. Keep trying until we are told to
			 * shut down.
			 */
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_lsm_tree_switch(session, entry->lsm_tree));
			if (ret != 0) {
				__wt_err(session, ret, "Error in LSM switch");
				ret = 0;
			}
			__lsm_manager_free_work_unit(session, entry);
			entry = NULL;
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		if ((F_ISSET(cookie, WT_LSM_WORK_FLUSH) ||
		    F_ISSET(cookie, WT_LSM_WORK_DROP) ||
		    F_ISSET(cookie, WT_LSM_WORK_BLOOM)) &&
		    (ret = __lsm_manager_pop_entry(
		    session, cookie->flags, &entry)) == 0 &&
		    entry != NULL) {
			if (entry->flags == WT_LSM_WORK_FLUSH) {
				WT_ERR(__lsm_get_chunk_to_flush(
				    session, entry->lsm_tree, &chunk));
				if (chunk != NULL) {
					WT_ERR(__wt_lsm_checkpoint_chunk(
					    session, entry->lsm_tree,
					    chunk, &flushed));
					WT_ASSERT(
					    session, chunk->refcnt > 0);
					(void)WT_ATOMIC_SUB(
					    chunk->refcnt, 1);
				}
			} else if (entry->flags == WT_LSM_WORK_DROP) {
				__wt_lsm_free_chunks(session, entry->lsm_tree);
			} else if (entry->flags == WT_LSM_WORK_BLOOM) {
				__wt_lsm_bloom_work(session, entry->lsm_tree);
				WT_ERR(__wt_lsm_manager_push_entry(session,
				    WT_LSM_WORK_MERGE, entry->lsm_tree));
			}
			/*
			 * If we completed some work from the application
			 * queue, go back and check on the switch queue.
			 */
			__lsm_manager_free_work_unit(session, entry);
			entry = NULL;
			continue;
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);

		if (F_ISSET(cookie, WT_LSM_WORK_MERGE) &&
		    (ret = __lsm_manager_pop_entry(
		    session, WT_LSM_WORK_MERGE, &entry)) == 0 &&
		    entry != NULL) {
			WT_ASSERT(session, entry->flags == WT_LSM_WORK_MERGE);
			ret = __wt_lsm_merge(session,
			    entry->lsm_tree, cookie->id);
			if (ret == WT_NOTFOUND) {
				F_CLR(entry->lsm_tree, WT_LSM_TREE_COMPACTING);
				ret = 0;
			}
			/* Clear any state */
			WT_CLEAR_BTREE_IN_SESSION(session);
			__lsm_manager_free_work_unit(session, entry);
			entry = NULL;
		}
		/* Flag an error if the pop failed. */
		WT_ERR(ret);
	}

	if (ret != 0) {
err:		__lsm_manager_free_work_unit(session, entry);
		__wt_err(session, ret,
		    "Error in LSM worker thread %d", cookie->id);
	}
	__wt_free(session, cookie);
	return (NULL);
}
