/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#if SPINLOCK_TYPE == SPINLOCK_GCC

int
__wt_spin_init(WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
	WT_UNUSED(session);
	WT_UNUSED(name);

	*(t) = 0;
	return (0);
}

void
__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	*(t) = 0;
}

#elif SPINLOCK_TYPE == SPINLOCK_PTHREAD_MUTEX

int
/* s_prototypes */__wt_spin_init(
    WT_SESSION_IMPL *session, WT_SPINLOCK *t, const char *name)
{
#ifdef HAVE_MUTEX_ADAPTIVE
	pthread_mutexattr_t attr;

	WT_RET(pthread_mutexattr_init(&attr));
	WT_RET(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP));
	WT_RET(pthread_mutex_init(&t->lock, &attr));
#else
	WT_RET(pthread_mutex_init(&t->lock, NULL));
#endif

	t->name = name;
	t->initialized = 1;

	WT_UNUSED(session);
	return (0);
}

void
/* s_prototypes */__wt_spin_destroy(WT_SESSION_IMPL *session, WT_SPINLOCK *t)
{
	WT_UNUSED(session);

	if (t->initialized) {
		(void)pthread_mutex_destroy(&t->lock);
		t->initialized = 0;
	}
}

/*
 * __wt_spin_lock_register --
 *	Register a spin-lock caller.
 */
void
__wt_spin_lock_register(WT_SESSION_IMPL *session,
    const char *file, int line, const char *name, int *slnop)
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
			*slnop = i;
			return;
		}

	__wt_err(session, ENOMEM,
	    "spin-lock registration failed, too many spinlocks");
	*slnop = WT_SPINLOCK_REGISTER_FAILED;
}

/*
 * __wt_statlog_spinlock_dump --
 *	Log the spin-lock statistics.
 */
int
__wt_statlog_spinlock_dump(
    WT_CONNECTION_IMPL *conn, const char *name)
{
	WT_CONNECTION_STATS_SPINLOCK *p, *t;
	int i, j;

	for (i = 0,
	    p = conn->spinlock_stats; i < WT_STATS_SPINLOCK_MAX; ++i, ++p) {
		if (p->file == NULL)
			break;

		for (j = 0; j < WT_STATS_SPINLOCK_MAX; ++j) {
			if (p->blocked[j] == 0)
				continue;

			t = &conn->spinlock_stats[j];
			WT_RET_TEST((fprintf(conn->stat_fp,
			    "%s %d %s spinlock %s: %s(%d) blocked by %s(%d)\n",
			    conn->stat_stamp, p->blocked[j], name, p->name,
			    p->file, p->line,
			    t->file, t->line) < 0), __wt_errno());
			p->blocked[j] = 0;
		}
	}
	return (0);
}
#endif
