/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

/*
 * Spinlock list, blocking matrix.
 *
 * The connection may be opened/closed, but we want to track spinlock behavior
 * across those opens/closes (and, we use static variables inside the library
 * to identify caller locations, which only works if we initialize them exactly
 * once).  For that reason, reference static memory from the connection.
 */
static WT_SPINLOCK *__spinlock_list[WT_SPINLOCK_MAX];
static WT_CONNECTION_STATS_SPINLOCK __spinlock_block[WT_SPINLOCK_MAX];

/*
 * __wt_spin_lock_stat_init --
 *	Set up the spinlock statistics.
 */
void
__wt_spin_lock_stat_init(WT_CONNECTION_IMPL *conn)
{
	conn->spinlock_list = __spinlock_list;
	conn->spinlock_block = __spinlock_block;
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
	u_int i;
	const char *s;

	*idp = WT_SPINLOCK_REGISTER_FAILED;

	conn = S2C(session);

	/* If this spinlock isn't yet on our spinlock list, add it. */
	for (i = 0; i < WT_ELEMENTS(__spinlock_list); ++i) {
		if (conn->spinlock_list[i] == t)
			break;
		if (conn->spinlock_list[i] == NULL &&
		    WT_ATOMIC_CAS(conn->spinlock_list[i], NULL, t))
			break;
	}
	if (i == WT_ELEMENTS(__spinlock_list)) {
		__wt_err(session, ENOMEM,
		    "spin-lock reference list allocation failed, too many "
		    "spinlocks");
		return;
	}

	/*
	 * Walk the connection's spinlock blocking matrix, looking for an empty
	 * slot.
	 */
	if ((s = strrchr(file, '/')) == NULL)
		s = file;
	else
		++s;
	for (i = 0; i < WT_ELEMENTS(__spinlock_block); ++i) {
		p = &conn->spinlock_block[i];
		if (p->file == NULL && WT_ATOMIC_CAS(p->file, NULL, s)) {
			p->line = line;
			p->name = t->name;
			*idp = (int)i;
			break;
		}
	}

	if (i == WT_ELEMENTS(__spinlock_block))
		__wt_err(session, ENOMEM,
		    "spin-lock blocking matrix allocation failed, too many "
		    "spinlocks");
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
	for (i = 0; i < WT_ELEMENTS(__spinlock_list); ++i) {
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
	for (i = 0; i < WT_ELEMENTS(__spinlock_block); ++i) {
		p = &conn->spinlock_block[i];
		if (p->file == NULL)
			break;

		WT_RET_TEST((fprintf(conn->stat_fp,
		    "%s %d %s spinlock %s acquired by %s(%d)\n",
		    conn->stat_stamp,
		    p->total <= ignore ? 0 : p->total,
		    tag,
		    p->name, p->file, p->line) < 0), __wt_errno());
		if (conn->stat_clear)
			p->total = 0;

		for (j = 0; j < WT_ELEMENTS(__spinlock_block); ++j) {
			t = &conn->spinlock_block[j];
			if (t->file == NULL)
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
