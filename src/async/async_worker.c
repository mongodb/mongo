/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_flush_wait --
 *	Wait for the final worker to finish flushing.
 *	Assumes it is called with spinlock held and returns it locked.
 */
static int
__async_flush_wait(WT_SESSION_IMPL *session, WT_ASYNC *async, int *locked)
{
	WT_DECL_RET;

	while (FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSHING)) {
		__wt_spin_unlock(session, &async->opsq_lock);
		*locked= 0;
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->flush_cond, 10000));
		__wt_spin_lock(session, &async->opsq_lock);
		*locked= 1;
	}
err:	return (ret);
}

/*
 * __async_worker_op --
 *	A worker thread handles an individual op.
 */
static int
__async_worker_op(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op)
{
	WT_ASYNC *async;
	WT_ASYNC_OP *asyncop;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int cb_ret, t_ret;

	conn = S2C(session);
	async = conn->async;
	asyncop = (WT_ASYNC_OP *)op;

	ret = 0;
	/*
	 * XXX need to get txn cfg.
	 */
	WT_RET(__wt_txn_begin(session, NULL));
	WT_ASSERT(session, op->state == WT_ASYNCOP_WORKING);
	/*
	 * Perform op.
	 *	Hash config and uri.  Find/create cursor.
	 *	Perform op.
	 */
	fprintf(stderr, "Worker %p op %d txn %" PRIu64 "\n",
	    pthread_self(), op->optype, session->txn.id);
	if (op->cb != NULL && op->cb->notify != NULL) {
		cb_ret = op->cb->notify(op->cb, asyncop, ret, 0);
	}
	if ((ret == 0 || ret == WT_NOTFOUND) && cb_ret == 0) {
		/*
		 * XXX need to get txn cfg.
		 */
		WT_TRET(__wt_txn_commit(session, NULL));
	} else {
		/*
		 * XXX need to get txn cfg.
		 */
		WT_TRET(__wt_txn_rollback(session, NULL));
	}
	/*
	 * After the callback returns, release the op back to
	 * the free pool.
	 */
	op->state = WT_ASYNCOP_FREE;
	return (ret);
}

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
				WT_ERR(__async_flush_wait(
				    session, async, &locked));
			}
		}
		/*
		 * Dequeue op.  We get here with the opsq lock held.
		 * Remove from the head of the queue.
		 */
		op = STAILQ_FIRST(&async->opqh);
		if (op == NULL) {
			__wt_spin_unlock(session, &async->opsq_lock);
			locked = 0;
			fprintf(stderr, "Worker %p no work now.\n",
			    pthread_self());
			goto wait_for_work;
		}

		/*
		 * There is work to do.
		 */
		STAILQ_REMOVE_HEAD(&async->opqh, q);
		WT_ASSERT(session, async->cur_queue > 0);
		--async->cur_queue;
		WT_ASSERT(session, op->state == WT_ASYNCOP_ENQUEUED);
		op->state = WT_ASYNCOP_WORKING;
		fprintf(stderr, "Worker %p got op %d %" PRIu64 "\n",
		    pthread_self(), op->internal_id, op->unique_id);
		if (op == &async->flush_op) {
			WT_ASSERT(session, FLD_ISSET(async->opsq_flush,
			    WT_ASYNC_FLUSH_IN_PROGRESS));
			/*
			 * We're the worker to take the flush op off the queue.
			 * Set the flushing flag and set count to 1.
			 */
			fprintf(stderr, "Worker %p start flush %d\n",
			    pthread_self(), async->flush_count);
			FLD_SET(async->opsq_flush, WT_ASYNC_FLUSHING);
			async->flush_count = 1;
			WT_ERR(__async_flush_wait(session, async, &locked));
		}
		/*
		 * Release the lock before performing the op.
		 */
		__wt_spin_unlock(session, &async->opsq_lock);
		locked = 0;
		if (op != &async->flush_op) {
			fprintf(stderr, "Worker %p got optype %d\n",
			    pthread_self(), op->optype);
			/*
			 * Perform op now.
			 */
			(void)__async_worker_op(session, op);
		}

wait_for_work:
		WT_ASSERT(session, locked == 0);
		/* Wait until the next event. */
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
