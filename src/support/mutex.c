/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX_LOGGING

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
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_list); ++i) {
		if (conn->spinlock_list[i] == t)
			break;
		if (conn->spinlock_list[i] == NULL &&
		    WT_ATOMIC_CAS(conn->spinlock_list[i], NULL, t))
			break;
	}
	if (i == WT_ELEMENTS(conn->spinlock_list)) {
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
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_stats); ++i) {
		p = &conn->spinlock_stats[i];
		if (p->file == NULL && WT_ATOMIC_CAS(p->file, NULL, s)) {
			p->line = line;
			p->name = t->name;
			*idp = (int)i;
			break;
		}
	}

	if (i == WT_ELEMENTS(conn->spinlock_stats))
		__wt_err(session, ENOMEM,
		    "spin-lock blocking matrix allocation failed, too many "
		    "spinlocks");
}

/*
 * __wt_statlog_spinlock_dump --
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

	/* Dump the reference list. */
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_list); ++i) {
		spin = conn->spinlock_list[i];
		if (spin == NULL)
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

	/* Dump the blocking matrix. */
	for (i = 0; i < WT_ELEMENTS(conn->spinlock_stats); ++i) {
		p = &conn->spinlock_stats[i];
		if (p->file == NULL)
			break;

		for (j = 0; j < WT_ELEMENTS(conn->spinlock_stats); ++j) {
			t = &conn->spinlock_stats[j];
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
	return (0);
}
#endif /* SPINLOCK_PTHREAD_MUTEX_LOGGING */
