/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * __spin_lock_next_id --
 *	Return the next spinlock caller ID.
 */
static void
__spin_lock_next_id(WT_SESSION_IMPL *session, int *idp)
{
	static int lock_id = 0, next_id = 0;

	/* If we've already registered this location, we're done. */
	if (*idp != WT_SPINLOCK_REGISTER)
		return;

	/*
	 * We can't use the global spinlock to lock the ID allocation (duh!),
	 * use a CAS instruction to serialize access to a local variable.
	 * This work only gets done once per library instantiation, there
	 * isn't a performance concern.
	 */
	while (!WT_ATOMIC_CAS(lock_id, 0, 1))
		__wt_yield();

	if (next_id < WT_SPINLOCK_MAX) {
		if (*idp == WT_SPINLOCK_REGISTER)
			*idp = next_id++;
	} else
		__wt_errx(session,
		    "spinlock register allocation failed, too many spinlocks");

	lock_id = 0;
}

/*
 * __wt_spin_lock_register --
 *	Register a spin-lock caller.
 */
void
__wt_spin_lock_register(WT_SESSION_IMPL *session,
    WT_SPINLOCK *t, const char *file, int line, int *idp)
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
	 * First, allocate an ID if needed.
	 */
	__spin_lock_next_id(session, idp);

	/*
	 * Add this spinlock to our list and the caller's information to the
	 * blocking matrix.  We could race here (if two threads of control
	 * register the same mutex at the same time), but we don't care, both
	 * threads are setting identical information.
	 */
	conn->spinlock_list[*idp] = t;

	p = &conn->spinlock_block[*idp];
	p->name = t->name;
	if ((p->file = strrchr(file, '/')) == NULL)
		p->file = file;
	else
		++p->file;
	p->line = line;
}

/*
 * __wt_spin_lock_unregister --
 *	Unregister a spinlock
 */
void
__wt_spin_lock_unregister(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
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
	 */
	WT_FULL_BARRIER();
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
	uint64_t ignore;
	u_int i, j;

	/*
	 * Ignore rare acquisition of a spinlock using a base value of 10 per
	 * second so we don't create graphs we don't care about.
	 */
	ignore = (uint64_t)(conn->stat_usecs / 1000000) * 10;

	/* Output the number of times each spinlock was acquired. */
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_list); ++i) {
		if ((spin = conn->spinlock_list[i]) == NULL)
			continue;

		WT_RET_TEST((fprintf(conn->stat_fp,
		    "%s %" PRIu64 " %s spinlock %s: acquisitions\n",
		    conn->stat_stamp,
		    spin->counter <= ignore ? 0 : spin->counter,
		    tag, spin->name) < 0),
		    __wt_errno());
		if (conn->stat_clear)
			spin->counter = 0;
	}

	/*
	 * Output the number of times each location acquire its spinlock and
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
		if (conn->stat_clear)
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
			if (conn->stat_clear)
				p->blocked[j] = 0;
		}
	}

	WT_FULL_BARRIER();			/* Minimize the window. */

	return (0);
}

#endif /* SPINLOCK_PTHREAD_MUTEX_LOGGING */
