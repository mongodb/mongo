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
    const char *file, int line, const char *name, int *idp)
{
	WT_CONNECTION_STATS_SPINLOCK *p;
	int i;
	const char *s;

	/*
	 * Walk the connection's array of spinlock statistics, looking for an
	 * empty slot.
	 */
	if ((s = strrchr(file, '/')) == NULL)
		s = file;
	else
		++s;
	for (i = 0,
	    p = S2C(session)->spinlock_stats;
	    i < WT_STATS_SPINLOCK_MAX; ++i, ++p)
		if (p->file == NULL && WT_ATOMIC_CAS(p->file, NULL, s)) {
			p->line = line;
			p->name = name;
			*idp = i;
			return;
		}

	__wt_err(session, ENOMEM,
	    "spin-lock registration failed, too many spinlocks");
	*idp = WT_SPINLOCK_REGISTER_FAILED;
}

/*
 * __wt_statlog_spinlock_dump --
 *	Log the spin-lock statistics.
 */
int
__wt_statlog_dump_spinlock(WT_CONNECTION_IMPL *conn, const char *name)
{
	WT_CONNECTION_STATS_SPINLOCK *p, *t;
	u_int i, j;

	for (i = 0,
	    p = conn->spinlock_stats; i < WT_STATS_SPINLOCK_MAX; ++i, ++p) {
		if (p->file == NULL)
			break;

		for (j = 0; j < WT_STATS_SPINLOCK_MAX; ++j) {
			t = &conn->spinlock_stats[j];
			if (t->file == NULL)
				continue;

			WT_RET_TEST((fprintf(conn->stat_fp,
			    "%s %d %s spinlock %s: %s(%d) blocked by %s(%d)\n",
			    conn->stat_stamp, p->blocked[j], name, p->name,
			    p->file, p->line,
			    t->file, t->line) < 0), __wt_errno());

			    if (conn->stat_clear)
				p->blocked[j] = 0;
		}
	}
	return (0);
}

#endif /* SPINLOCK_PTHREAD_MUTEX_LOGGING */
