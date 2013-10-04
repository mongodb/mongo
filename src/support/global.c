/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

WT_PROCESS __wt_process;			/* Per-process structure */
static int __wt_pthread_once_failed;		/* If initialization failed */

static int
__system_is_little_endian(void)
{
	uint64_t v;
	int little;

	v = 1;
	little = *((uint8_t *)&v) == 0 ? 0 : 1;

	if (little)
		return (0);

	fprintf(stderr,
	    "This release of the WiredTiger data engine does not support "
	    "big-endian systems; contact WiredTiger for more information.\n");
	return (EINVAL);
}

static void
__wt_pthread_once(void)
{
	WT_DECL_RET;

	if ((ret = __wt_spin_init(NULL, &__wt_process.spinlock)) != 0)
		__wt_pthread_once_failed = ret;

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
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;
	static int first = 1;
	WT_DECL_RET;

	WT_RET(__system_is_little_endian());

	/*
	 * Do per-process initialization once, before anything else, but only
	 * once.  I don't know how heavy_weight pthread_once might be, so I'm
	 * front-ending it with a local static and only using pthread_once to
	 * avoid a race.
	 */
	if (first) {
		if ((ret = pthread_once(&once_control, __wt_pthread_once)) != 0)
			__wt_pthread_once_failed = ret;
		first = 0;
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
	__wt_session_dump_all(session);

	__wt_errx(session, "process ID %" PRIdMAX
	    ": waiting for debugger...", (intmax_t)getpid());

	/* Sleep forever, the debugger will interrupt us when it attaches. */
	for (;;)
		__wt_sleep(100, 0);
#else
	WT_UNUSED(session);
#endif
}
#endif
