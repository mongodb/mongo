/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

WT_PROCESS __wt_process;			/* Per-process structure */
static int __wt_pthread_once_failed;		/* If initialization failed */

/*
 * __system_is_little_endian --
 *	Check if the system is little endian.
 */
static int
__system_is_little_endian(void)
{
	uint64_t v;
	bool little;

	v = 1;
	little = *((uint8_t *)&v) != 0;

	if (little)
		return (0);

	fprintf(stderr,
	    "This release of the WiredTiger data engine does not support "
	    "big-endian systems; contact WiredTiger for more information.\n");
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

	if ((ret = __system_is_little_endian()) != 0) {
		__wt_pthread_once_failed = ret;
		return;
	}

	if ((ret =
	    __wt_spin_init(NULL, &__wt_process.spinlock, "global")) != 0) {
		__wt_pthread_once_failed = ret;
		return;
	}

	__wt_cksum_init();

	TAILQ_INIT(&__wt_process.connqh);

#ifdef HAVE_DIAGNOSTIC
	/* Verify the pre-computed metadata hash. */
	WT_ASSERT(NULL, WT_METAFILE_NAME_HASH ==
	    __wt_hash_city64(WT_METAFILE_URI, strlen(WT_METAFILE_URI)));

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
