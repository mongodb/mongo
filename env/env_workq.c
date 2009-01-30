/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void *__wt_engine(void *);

/*
 * wt_start --
 *	Start the engine.
 */
int
wt_start(u_int32_t flags)
{
	static int initial_tasks = 0;
	int ret;

	/*
	 * No matter what we're doing, we end up here before we do any real
	 * work.   The first time, check the build itself and initialize the
	 * global structure.
	 */
	if (!initial_tasks) {
		if ((ret = __wt_build_verify()) != 0)
			return (ret);
		if ((ret = __wt_global_init()) != 0)
			return (ret);
		initial_tasks = 1;
	}

	/* Spawn the engine, and wait until it's ready to proceed. */
	if (pthread_create(&WT_GLOBAL(tid), NULL, __wt_engine, NULL) != 0)
		return (WT_ERROR);
	while (!WT_GLOBAL(running))
		__wt_sleep(0, 1000);

	return (0);
}

/*
 * wt_stop --
 *	Stop the engine.
 */
int
wt_stop(u_int32_t flags)
{
	/*
	 * We'll need real cleanup at some point, but for now, just flag the
	 * engine to quit and wait for the thread to exit.
	 */
	WT_GLOBAL(running) = 0;
	WT_FLUSH_MEMORY;

	(void)pthread_join(WT_GLOBAL(tid), NULL);

	return (0);
}

/*
 * __wt_engine --
 *      Engine main routine.
 */
static void *
__wt_engine(void *notused)
{
	WT_TOC **q, **eq, *toc;
	int not_found, ret, tenths;

	/* Allocate the work queue. */
#define	WT_WORKQ_SIZE	32
	WT_GLOBAL(workq_entries) = WT_WORKQ_SIZE;
	if (__wt_calloc(
	    NULL, WT_WORKQ_SIZE, sizeof(WT_TOC *), &WT_GLOBAL(workq)) != 0)
		return (NULL);

	/* We're running. */
	WT_GLOBAL(running) = 1;
	WT_FLUSH_MEMORY;

	/* Walk the queue, executing work. */
	for (not_found = 1, tenths = 0,
	    q = WT_GLOBAL(workq), eq = q + WT_WORKQ_SIZE;;) {
		if ((toc = *q) != NULL) {
			not_found = 0;

			F_SET(toc, WT_RUNNING);
			toc->ret = __wt_api_switch(toc);
			F_CLR(toc, WT_RUNNING);

			/* Clear the slot and flush memory. */
			*q = NULL;
			WT_FLUSH_MEMORY;

			/* Wake the waiting thread. */
			__wt_unlock(toc->mtx);
		}
		if (++q == eq) {
			/*
			 * If we didn't find work, yield the processor.  If
			 * we don't find work in 100 loops, start sleeping,
			 * from a 10th of a second to a second, by 10ths of
			 * a second.
			 */
			if (not_found == 0) {
				tenths = 0;
				not_found = 1;
			} else if (not_found >= 100) {
				if (tenths < 10)
					++tenths;
				__wt_sleep(0, tenths * 100000);
			} else {
				++not_found;
				pthread_yield();
			}
			q = WT_GLOBAL(workq);
		}
		if (WT_GLOBAL(running) == 0)
			break;
	}

	WT_FREE_AND_CLEAR(NULL, WT_GLOBAL(workq));
}
