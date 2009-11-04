/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_queue_op_check(ENV *);

/*
 * __wt_workq --
 *      Routine to process the WT_TOC work queue.
 */
void *
__wt_workq(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_TOC *toc;
	int notfound;

	env = arg;
	ienv = env->ienv;

	/* Walk the WT_TOC queue, executing work. */
	notfound = 1;
	while (F_ISSET(ienv, WT_RUNNING)) {
		/* Walk the threads, looking for work requests. */
		TAILQ_FOREACH(toc, &ienv->tocqh, q)
			if (toc->request != NULL) {
				WT_STAT_INCR(ienv->stats,
				    WORKQ_REQUESTS, "workQ requests");
				notfound = 0;
				toc->request(toc);
			}

		WT_STAT_INCR(ienv->stats, WORKQ_PASSES, "workQ queue passes");

		__wt_queue_op_check(env);

		/*
		 * If we didn't find work, yield the processor.  If we
		 * don't find work for awhile, sleep.
		 */
		if (notfound++)
			if (notfound >= 100000) {
				WT_STAT_INCR(ienv->stats,
				    WORKQ_SLEEP, "workQ sleeps");
				__wt_sleep(0, notfound);
				notfound = 100000;
			} else {
				WT_STAT_INCR(ienv->stats,
				    WORKQ_YIELD, "workQ yields");
				__wt_yield();
			}

	}

	/*
	 * One final check, in case we race with the thread telling us to
	 * stop.
	 */
	__wt_queue_op_check(env);

	return (NULL);
}

/*
 * __wt_queue_op_check --
 *	Check for ENV queue operations.
 */
static void
__wt_queue_op_check(ENV *env)
{
	IENV *ienv;

	ienv = env->ienv;

	/* Check to see if there's a WT_TOC queue operation waiting. */
	if (ienv->toc_add != NULL) {
		TAILQ_INSERT_TAIL(&ienv->tocqh, ienv->toc_add, q);
		ienv->toc_add = NULL;
		__wt_unlock(&ienv->mtx);
	}
	if (ienv->toc_del != NULL) {
		TAILQ_REMOVE(&ienv->tocqh, ienv->toc_del, q);
		__wt_free(env, ienv->toc_del);
		ienv->toc_del = NULL;
		__wt_unlock(&ienv->mtx);
	}
}
