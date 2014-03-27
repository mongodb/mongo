/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_worker --
 *	The async worker threads.
 */
void *
__wt_async_worker(void *arg)
{
	WT_ASYNC *async;
	WT_ASYNC_OP_IMPL *op;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	int locked;

	session = arg;
	conn = S2C(session);
	async = conn->async;

	locked = 0;
	fprintf(stderr, "Async worker %p started\n",pthread_self());
	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Wait until the next event. */
		__wt_spin_lock(session, &async->opsq_lock);
		locked = 1;
		fprintf(stderr, "Async worker %p check flushing 0x%x\n",
		    pthread_self(), async->opsq_flush);
		if (FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSHING)) {
			/*
			 * Worker flushing going on.  Last worker to the party
			 * needs to clear the FLUSHING flag and signal the cond.
			 * If FLUSHING is going on, we do not take anything off
			 * the queue.
			 */
			if (++async->flush_count == conn->async_workers) {
				/*
				 * We're last.  All workers accounted for so
				 * signal the condition and clear the flushing
				 * flag to release the other worker threads.
				 * Set the complete flag so that the
				 * caller can return to the application.
				 */
				fprintf(stderr, "Worker %p complete flush\n",
				    pthread_self());
				FLD_SET(async->opsq_flush,
				    WT_ASYNC_FLUSH_COMPLETE);
				FLD_CLR(async->opsq_flush, WT_ASYNC_FLUSHING);
				__wt_spin_unlock(session, &async->opsq_lock);
				locked = 0;
				fprintf(stderr, "Worker %p signal flush\n",
				    pthread_self());
				WT_ERR(__wt_cond_signal(session,
				    async->flush_cond));
				__wt_spin_lock(session, &async->opsq_lock);
				locked = 1;
			} else {
				/*
				 * We need to wait for the last worker to
				 * signal the condition.
				 */
				fprintf(stderr, "Worker %p flush checkin %d\n",
				    pthread_self(), async->flush_count);
				while (FLD_ISSET(
				    async->opsq_flush, WT_ASYNC_FLUSHING)) {
					__wt_spin_unlock(
					    session, &async->opsq_lock);
					locked = 0;
					WT_ERR_TIMEDOUT_OK(__wt_cond_wait(
					    session, async->flush_cond, 10000));
					__wt_spin_lock(
					    session, &async->opsq_lock);
					locked = 1;
				}
			}
		}
		/*
		 * Dequeue op.  We get here with the opqs lock held.
		 */
		op = &async->flush_op;
		if (op == &async->flush_op && FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSH_IN_PROGRESS)) {
			/*
			 * We're the worker to take the flush op off the queue.
			 * Set the flushing flag and set count to 1.
			 */
			fprintf(stderr, "Worker %p start flush %d\n",
			    pthread_self(), async->flush_count);
			FLD_SET(async->opsq_flush, WT_ASYNC_FLUSHING);
			async->flush_count = 1;
		}
		__wt_spin_unlock(session, &async->opsq_lock);
		locked = 0;
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->ops_cond, 100000));
	}

	if (0) {
err:		__wt_err(session, ret, "async worker error");
		if (locked)
			__wt_spin_unlock(session, &async->opsq_lock);
	}
	return (NULL);
}
