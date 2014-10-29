/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * __wt_spin_lock_register_lock --
 *	Add a lock to the connection's list.
 */
int
__wt_spin_lock_register_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_CONNECTION_IMPL *conn;
	u_int i;

	/*
	 * There is a spinlock we initialize before we have a connection, the
	 * global library lock.  In that case, the session will be NULL and
	 * we can't track the lock.
	 */
	if (session == NULL)
		return (0);

	conn = S2C(session);

	for (i = 0; i < WT_SPINLOCK_MAX; i++)
		if (conn->spinlock_list[i] == NULL &&
		    WT_ATOMIC_CAS(conn->spinlock_list[i], NULL, t))
			return (0);

	WT_RET_MSG(session, ENOMEM,
	    "spinlock connection registry failed, increase the connection's "
	    "spinlock list size");
}

/*
 * __wt_spin_lock_unregister_lock --
 *	Remove a lock from the connection's list.
 */
void
__wt_spin_lock_unregister_lock(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_CONNECTION_IMPL *conn;
	u_int i;

	conn = S2C(session);

	for (i = 0; i < WT_SPINLOCK_MAX; i++)
		if (conn->spinlock_list[i] == t)
			    conn->spinlock_list[i] = NULL;

	/*
	 * XXX
	 * The statistics thread reads through this array, there's a possible
	 * race: if that thread reads the pointer then goes to sleep, then we
	 * free the spinlock, then the statistics thread wakes up, it can read
	 * free'd memory.
	 *
	 * This is performance debugging code, so we're not fixing the race for
	 * now, minimize the window.
	 */
	WT_FULL_BARRIER();
}

/*
 * __spin_lock_next_id --
 *	Return the next spinlock caller ID.
 */
static int
__spin_lock_next_id(WT_SESSION_IMPL *session, int *idp)
{
	static int lock_id = 0, next_id = 0;
	WT_DECL_RET;

	/* If we've ever registered this location, we already have an ID. */
	if (*idp != WT_SPINLOCK_REGISTER)
		return (0);

	/*
	 * We can't use the global spinlock to lock the ID allocation (duh!),
	 * use a CAS instruction to serialize access to a local variable.
	 * This work only gets done once per library instantiation, there
	 * isn't a performance concern.
	 */
	while (!WT_ATOMIC_CAS(lock_id, 0, 1))
		__wt_yield();

	/* Allocate a blocking ID for this location. */
	if (*idp == WT_SPINLOCK_REGISTER) {
		if (next_id < WT_SPINLOCK_MAX_LOCATION_ID)
			*idp = next_id++;
		else
			WT_ERR_MSG(session, ENOMEM,
			    "spinlock caller location registry failed, "
			    "increase the connection's blocking matrix size");
	}

err:	WT_PUBLISH(lock_id, 0);
	return (ret);
}

/*
 * __wt_spin_lock_register_caller --
 *	Register a spin-lock caller's location information in the blocking
 * matrix.
 */
int
__wt_spin_lock_register_caller(WT_SESSION_IMPL *session,
    const char *name, const char *file, int line, int *idp)
{
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS_SPINLOCK *p;

	conn = S2C(session);

	/*
	 * The caller's location ID is a static offset into a per-connection
	 * structure, and that has problems: first, if there are multiple
	 * connections, we'll need to hold some kind of lock to avoid racing
	 * when setting that value, and second, if/when there are multiple
	 * connections and/or a single connection is closed and re-opened, the
	 * variable may be initialized and underlying connection information
	 * may not.
	 *
	 * First, allocate a location ID if needed.
	 */
	WT_RET(__spin_lock_next_id(session, idp));

	/*
	 * Add the caller's information to the blocking matrix.  We could race
	 * here (if two threads of control register the same lock at the same
	 * time), but we don't care as both threads are setting the identical
	 * information.
	 */
	p = &conn->spinlock_block[*idp];
	p->name = name;
	if ((p->file = strrchr(file, '/')) == NULL)
		p->file = file;
	else
		++p->file;
	p->line = line;
	return (0);
}

/*
 * __wt_statlog_dump_spinlock --
 *	Log the spin-lock statistics.
 */
int
__wt_statlog_dump_spinlock(WT_CONNECTION_IMPL *conn, const char *tag)
{
	WT_SPINLOCK *spin;
	WT_CONNECTION_STATS_SPINLOCK *p, *t;
	uint64_t block_manager, btree_page, ignore;
	u_int i, j;

	/*
	 * Ignore rare acquisition of a spinlock using a base value of 10 per
	 * second so we don't create graphs we don't care about.
	 */
	ignore = (uint64_t)(conn->stat_usecs / 1000000) * 10;

	/* Output the number of times each spinlock was acquired. */
	block_manager = btree_page = 0;
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_list); ++i) {
		if ((spin = conn->spinlock_list[i]) == NULL)
			continue;

		/*
		 * There are two sets of spinlocks we aggregate, the btree page
		 * locks and the block manager per-file locks.  The reason is
		 * the block manager locks grow with the number of files open
		 * (and LSM and bloom filters can open a lot of files), and
		 * there are 16 btree page locks and splitting them out has not
		 * historically been that informative.
		 */
		if (strcmp(spin->name, "block manager") == 0) {
			block_manager += spin->counter;
			if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
				spin->counter = 0;
			continue;
		}
		if (strcmp(spin->name, "btree page") == 0) {
			btree_page += spin->counter;
			if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
				spin->counter = 0;
			continue;
		}

		WT_RET_TEST((fprintf(conn->stat_fp,
		    "%s %" PRIu64 " %s spinlock %s: acquisitions\n",
		    conn->stat_stamp,
		    spin->counter <= ignore ? 0 : spin->counter,
		    tag, spin->name) < 0),
		    __wt_errno());
		if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
			spin->counter = 0;
	}
	WT_RET_TEST((fprintf(conn->stat_fp,
	    "%s %" PRIu64 " %s spinlock %s: acquisitions\n",
	    conn->stat_stamp,
	    block_manager <= ignore ? 0 : block_manager,
	    tag, "block manager") < 0),
	    __wt_errno());
	WT_RET_TEST((fprintf(conn->stat_fp,
	    "%s %" PRIu64 " %s spinlock %s: acquisitions\n",
	    conn->stat_stamp,
	    btree_page <= ignore ? 0 : btree_page,
	    tag, "btree page") < 0),
	    __wt_errno());

	/*
	 * Output the number of times each location acquires its spinlock and
	 * the blocking matrix.
	 */
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_block); ++i) {
		p = &conn->spinlock_block[i];
		if (p->name == NULL)
			continue;

		WT_RET_TEST((fprintf(conn->stat_fp,
		    "%s %d %s spinlock %s acquired by %s(%d)\n",
		    conn->stat_stamp,
		    p->total <= ignore ? 0 : p->total,
		    tag,
		    p->name, p->file, p->line) < 0), __wt_errno());
		if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
			p->total = 0;

		for (j = 0; j < WT_ELEMENTS(conn->spinlock_block); ++j) {
			t = &conn->spinlock_block[j];
			if (t->name == NULL)
				continue;

			WT_RET_TEST((fprintf(conn->stat_fp,
			    "%s %d %s spinlock %s: %s(%d) blocked by %s(%d)\n",
			    conn->stat_stamp,
			    p->blocked[j] <= ignore ? 0 : p->blocked[j],
			    tag,
			    p->name, p->file, p->line,
			    t->file, t->line) < 0), __wt_errno());
			if (FLD_ISSET(conn->stat_flags, WT_CONN_STAT_CLEAR))
				p->blocked[j] = 0;
		}
	}

	WT_FULL_BARRIER();			/* Minimize the window. */
	return (0);
}

#endif /* SPINLOCK_PTHREAD_MUTEX_LOGGING */
