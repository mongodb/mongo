/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_thread_run --
 *	General wrapper for any thread.
 */
WT_THREAD_RET
__wt_thread_run(void *arg)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_THREAD *thread;

	thread = (WT_THREAD*)arg;
	session = thread->session;

	ret = thread->run_func(session, thread);

	if (ret != 0 && F_ISSET(thread, WT_THREAD_PANIC_FAIL))
		WT_PANIC_MSG(session, ret,
		    "Unrecoverable utility thread error");

	/*
	 * The three cases when threads are expected to stop are:
	 * 1.  When recovery is done.
	 * 2.  When the connection is closing.
	 * 3.  When a shutdown has been requested via clearing the run flag.
	 */
	WT_ASSERT(session, !F_ISSET(thread, WT_THREAD_RUN) ||
	    F_ISSET(S2C(session), WT_CONN_CLOSING | WT_CONN_RECOVERING));

	return (WT_THREAD_RET_VALUE);
}

/*
 * __thread_group_grow --
 *	Increase the number of running threads in the group.
 */
static int
__thread_group_grow(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group, uint32_t new_count)
{
	WT_THREAD *thread;

	WT_ASSERT(session,
	    __wt_rwlock_islocked(session, group->lock));

	/*
	 * Any bounds checking is done by the caller so we know that
	 * there is space in the array for new threads.
	 */
	while (group->current_threads < new_count) {
		thread = group->threads[group->current_threads++];
		__wt_verbose(session, WT_VERB_THREAD_GROUP,
		    "Starting utility thread: %p:%"PRIu32"\n",
		    group, thread->id);
		F_SET(thread, WT_THREAD_RUN);
		WT_ASSERT(session, thread->session != NULL);
		WT_RET(__wt_thread_create(thread->session,
		    &thread->tid, __wt_thread_run, thread));
	}
	return (0);
}

/*
 * __thread_group_shrink --
 *	Decrease the number of running threads in the group, and free any
 *	memory associated with slots larger than the new count.
 */
static int
__thread_group_shrink(WT_SESSION_IMPL *session,
    WT_THREAD_GROUP *group, uint32_t new_count)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_THREAD *thread;
	uint32_t current_slot;

	WT_ASSERT(session,
	    __wt_rwlock_islocked(session, group->lock));

	for (current_slot = group->alloc; current_slot > new_count; ) {
		/*
		 * The offset value is a counter not an array index,
		 * so adjust it before finding the last thread in the group.
		 */
		thread = group->threads[--current_slot];

		if (thread == NULL)
			continue;

		/* Wake threads to ensure they notice the state change */
		if (thread->tid != 0) {
			__wt_verbose(session, WT_VERB_THREAD_GROUP,
			    "Stopping utility thread: %p:%"PRIu32"\n",
			    group, thread->id);
			F_CLR(thread, WT_THREAD_RUN);
			__wt_cond_signal(session, group->wait_cond);
			WT_TRET(__wt_thread_join(session, thread->tid));
			thread->tid = 0;
		}

		if (thread->session != NULL) {
			wt_session = (WT_SESSION *)thread->session;
			WT_TRET(wt_session->close(wt_session, NULL));
			thread->session = NULL;
		}
		__wt_free(session, thread);
		group->threads[current_slot] = NULL;
	}

	/* Update the thread group state to match our changes */
	group->current_threads = current_slot;
	return (ret);
}

/*
 * __thread_group_resize --
 *	Resize an array of utility threads already holding the lock.
 */
static int
__thread_group_resize(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group,
    uint32_t new_min, uint32_t new_max, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_THREAD *thread;
	size_t alloc;
	uint32_t i, session_flags;

	conn = S2C(session);
	session_flags = 0;

	WT_ASSERT(session,
	    group->current_threads <= group->alloc &&
	    __wt_rwlock_islocked(session, group->lock));

	if (new_min == group->min && new_max == group->max)
		return (0);

	/*
	 * Coll shrink to reduce the number of thread structures and running
	 * threads if required by the change in group size.
	 */
	WT_RET(__thread_group_shrink(session, group, new_max));

	/*
	 * Only reallocate the thread array if it is the largest ever, since
	 * our realloc doesn't support shrinking the allocated size.
	 */
	if (group->alloc < new_max) {
		alloc = group->alloc * sizeof(*group->threads);
		WT_RET(__wt_realloc(session, &alloc,
		    new_max * sizeof(*group->threads), &group->threads));
		group->alloc = new_max;
	}

	/*
	 * Initialize the structures based on the previous group size, not
	 * the previous allocated size.
	 */
	for (i = group->max; i < new_max; i++) {
		WT_ERR(__wt_calloc_one(session, &thread));
		/*
		 * Threads get their own session and lookaside table cursor
		 * if the lookaside table is open. Note that threads are
		 * started during recovery, before the lookaside table is
		 * created.
		 */
		if (LF_ISSET(WT_THREAD_CAN_WAIT))
			session_flags = WT_SESSION_CAN_WAIT;
		if (F_ISSET(conn, WT_CONN_LAS_OPEN))
			FLD_SET(session_flags, WT_SESSION_LOOKASIDE_CURSOR);
		WT_ERR(__wt_open_internal_session(conn, group->name,
		    false, session_flags, &thread->session));
		if (LF_ISSET(WT_THREAD_PANIC_FAIL))
			F_SET(thread, WT_THREAD_PANIC_FAIL);
		thread->id = i;
		thread->run_func = group->run_func;
		WT_ASSERT(session, group->threads[i] == NULL);
		group->threads[i] = thread;
	}

	if (group->current_threads < new_min)
		WT_ERR(__thread_group_grow(session, group, new_min));

err:	/*
	 * Update the thread group information even on failure to improve our
	 * chances of cleaning up properly.
	 */
	group->max = new_max;
	group->min = new_min;

	/*
	 * An error resizing a thread array is fatal, it should only happen
	 * in an out of memory situation.
	 */
	if (ret != 0) {
		WT_TRET(__wt_thread_group_destroy(session, group));
		WT_PANIC_RET(session, ret,
		    "Error while resizing thread group");
	}
	return (ret);
}

/*
 * __wt_thread_group_resize --
 *	Resize an array of utility threads taking the lock.
 */
int
__wt_thread_group_resize(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group,
    uint32_t new_min, uint32_t new_max, uint32_t flags)
{
	WT_DECL_RET;

	__wt_verbose(session, WT_VERB_THREAD_GROUP,
	    "Resize thread group: %p, from min: %" PRIu32 " -> %" PRIu32
	    " from max: %" PRIu32 " -> %" PRIu32 "\n",
	    group, group->min, new_min, group->max, new_max);

	__wt_writelock(session, group->lock);
	WT_TRET(__thread_group_resize(
	    session, group, new_min, new_max, flags));
	__wt_writeunlock(session, group->lock);
	return (ret);
}

/*
 * __wt_thread_group_create --
 *	Create a new thread group, assumes incoming group structure is
 *	zero initialized.
 */
int
__wt_thread_group_create(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group, const char *name,
    uint32_t min, uint32_t max, uint32_t flags,
    int (*run_func)(WT_SESSION_IMPL *session, WT_THREAD *context))
{
	WT_DECL_RET;
	bool cond_alloced;

	/* Check that the structure is initialized as expected */
	WT_ASSERT(session, group->alloc == 0);

	cond_alloced = false;

	__wt_verbose(session, WT_VERB_THREAD_GROUP,
	    "Creating thread group: %p\n", group);

	WT_RET(__wt_rwlock_alloc(session, &group->lock, "Thread group"));
	WT_ERR(__wt_cond_alloc(
	    session, "Thread group cond", false, &group->wait_cond));
	cond_alloced = true;

	__wt_writelock(session, group->lock);
	group->run_func = run_func;
	group->name = name;

	WT_TRET(__thread_group_resize(session, group, min, max, flags));
	__wt_writeunlock(session, group->lock);

	/* Cleanup on error to avoid leaking resources */
err:	if (ret != 0) {
		if (cond_alloced)
			WT_TRET(__wt_cond_destroy(session, &group->wait_cond));
		__wt_rwlock_destroy(session, &group->lock);
	}
	return (ret);
}

/*
 * __wt_thread_group_destroy --
 *	Shut down a thread group.  Our caller must hold the lock.
 */
int
__wt_thread_group_destroy(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group)
{
	WT_DECL_RET;

	__wt_verbose(session, WT_VERB_THREAD_GROUP,
	    "Destroying thread group: %p\n", group);

	WT_ASSERT(session,
	    __wt_rwlock_islocked(session, group->lock));

	/* Shut down all threads and free associated resources. */
	WT_TRET(__thread_group_shrink(session, group, 0));

	__wt_free(session, group->threads);

	WT_TRET(__wt_cond_destroy(session, &group->wait_cond));
	__wt_rwlock_destroy(session, &group->lock);

	/*
	 * Clear out any settings from the group, some structures are reused
	 * for different thread groups - in particular the eviction thread
	 * group for recovery and then normal runtime.
	 */
	memset(group, 0, sizeof(*group));

	return (ret);
}

/*
 * __wt_thread_group_start_one --
 *	Start a new thread if possible.
 */
int
__wt_thread_group_start_one(
    WT_SESSION_IMPL *session, WT_THREAD_GROUP *group, bool wait)
{
	WT_DECL_RET;

	if (group->current_threads >= group->max)
		return (0);

	if (wait)
		__wt_writelock(session, group->lock);
	else if (__wt_try_writelock(session, group->lock) != 0)
		return (0);

	/* Recheck the bounds now that we hold the lock */
	if (group->current_threads < group->max)
		WT_TRET(__thread_group_grow(
		    session, group, group->current_threads + 1));
	__wt_writeunlock(session, group->lock);

	return (ret);
}
