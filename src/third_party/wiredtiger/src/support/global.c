/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

WT_PROCESS __wt_process;			/* Per-process structure */
static int __wt_pthread_once_failed;		/* If initialization failed */

/*
 * __wt_endian_check --
 *	Check the build matches the machine.
 */
static int
__wt_endian_check(void)
{
	uint64_t v;
	bool big;
	const char *e;

	v = 1;
	big = *((uint8_t *)&v) == 0;

#ifdef WORDS_BIGENDIAN
	if (big)
		return (0);
	e = "big-endian";
#else
	if (!big)
		return (0);
	e = "little-endian";
#endif
	fprintf(stderr,
	    "This is a %s build of the WiredTiger data engine, incompatible "
	    "with this system\n", e);
	return (EINVAL);
}

/*
 * __wt_global_once --
 *	Global initialization, run once.
 */
static void
__wt_global_once(void)
{
	WT_DECL_RET;

	if ((ret =
	    __wt_spin_init(NULL, &__wt_process.spinlock, "global")) != 0) {
		__wt_pthread_once_failed = ret;
		return;
	}

	__wt_cksum_init();

	TAILQ_INIT(&__wt_process.connqh);

#ifdef HAVE_DIAGNOSTIC
	/* Load debugging code the compiler might optimize out. */
	(void)__wt_breakpoint();
#endif
}

/*
 * __wt_library_init --
 *	Some things to do, before we do anything else.
 */
int
__wt_library_init(void)
{
	static bool first = true;
	WT_DECL_RET;

	/* Check the build matches the machine. */
	WT_RET(__wt_endian_check());

	/*
	 * Do per-process initialization once, before anything else, but only
	 * once.  I don't know how heavy-weight the function (pthread_once, in
	 * the POSIX world), might be, so I'm front-ending it with a local
	 * static and only using that function to avoid a race.
	 */
	if (first) {
		if ((ret = __wt_once(__wt_global_once)) != 0)
			__wt_pthread_once_failed = ret;
		first = false;
	}
	return (__wt_pthread_once_failed);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_breakpoint --
 *	A simple place to put a breakpoint, if you need one.
 */
int
__wt_breakpoint(void)
{
	return (0);
}

/*
 * __wt_attach --
 *	A routine to wait for the debugging to attach.
 */
void
__wt_attach(WT_SESSION_IMPL *session)
{
#ifdef HAVE_ATTACH
	u_int i;

	__wt_errx(session, "process ID %" PRIdMAX
	    ": waiting for debugger...", (intmax_t)getpid());

	/* Sleep forever, the debugger will interrupt us when it attaches. */
	for (i = 0; i < WT_MILLION; ++i)
		__wt_sleep(10, 0);
#else
	WT_UNUSED(session);
#endif
}
#endif
