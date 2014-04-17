/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_op_dequeue --
 *	Wait for work to be available.  Then atomically take it off
 *	the work queue.
 */
static int
__async_op_dequeue(WT_CONNECTION_IMPL *conn, WT_SESSION_IMPL *session,
    WT_ASYNC_OP_IMPL **op)
{
	WT_ASYNC *async;
	uint64_t cur_tail, last_consume, my_consume, my_slot, prev_slot;
	uint32_t max_tries, tries;

	async = conn->async;
	*op = NULL;
	max_tries = 100;
	tries = 0;
	/*
	 * Wait for work to do.  Work is available when async->head moves.
	 * Then grab the slot containing the work.  If we lose, try again.
	 */
retry:
	WT_ORDERED_READ(last_consume, async->alloc_tail);
	while (last_consume == async->head && ++tries < max_tries &&
	    async->flush_state != WT_ASYNC_FLUSHING) {
		__wt_yield();
		WT_ORDERED_READ(last_consume, async->alloc_tail);
	}
	if (F_ISSET(conn, WT_CONN_PANIC))
		__wt_panic(session);
	if (tries >= max_tries ||
	    async->flush_state == WT_ASYNC_FLUSHING)
		return (0);
	/*
	 * Try to increment the tail to claim this slot.  If we lose
	 * a race, try again.
	 */
	my_consume = last_consume + 1;
	if (!WT_ATOMIC_CAS(async->alloc_tail, last_consume, my_consume))
		goto retry;
	/*
	 * This item of work is ours to process.  Clear it out of the
	 * queue and return.
	 */
	my_slot = my_consume % async->async_qsize;
	prev_slot = last_consume % async->async_qsize;
	*op = WT_ATOMIC_STORE(async->async_queue[my_slot], NULL);

	WT_ASSERT(session, async->cur_queue > 0);
	WT_ASSERT(session, *op != NULL);
	WT_ASSERT(session, (*op)->state == WT_ASYNCOP_ENQUEUED);
	WT_ATOMIC_SUB(async->cur_queue, 1);
	(*op)->state = WT_ASYNCOP_WORKING;

	if (*op == &async->flush_op)
		/*
		 * We're the worker to take the flush op off the queue.
		 */
		WT_PUBLISH(async->flush_state, WT_ASYNC_FLUSHING);
	WT_ORDERED_READ(cur_tail, async->tail_slot);
	while (cur_tail != prev_slot) {
		__wt_yield();
		WT_ORDERED_READ(cur_tail, async->tail_slot);
	}
	WT_PUBLISH(async->tail_slot, my_slot);
	return (0);
}

/*
 * __async_flush_wait --
 *	Wait for the final worker to finish flushing.
 */
static int
__async_flush_wait(WT_SESSION_IMPL *session, WT_ASYNC *async, uint64_t my_gen)
{
	WT_DECL_RET;

	while (async->flush_state == WT_ASYNC_FLUSHING &&
	    async->flush_gen == my_gen)
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->flush_cond, 10000));
err:	return (ret);
}

/*
 * __async_worker_cursor --
 *	Return a cursor for the worker thread to use for its op.
 *	The worker thread caches cursors.  So first search for one
 *	with the same config/uri signature.  Otherwise open a new
 *	cursor and cache it.
 */
static int
__async_worker_cursor(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_ASYNC_WORKER_STATE *worker, WT_CURSOR **cursorp)
{
	WT_ASYNC_CURSOR *ac;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	wt_session = (WT_SESSION *)session;
	*cursorp = NULL;
	WT_ASSERT(session, op->format != NULL);
	STAILQ_FOREACH(ac, &worker->cursorqh, q) {
		if (op->format->cfg_hash == ac->cfg_hash &&
		    op->format->uri_hash == ac->uri_hash) {
			/*
			 * If one of our cached cursors has a matching
			 * signature, use it and we're done.
			 */
			*cursorp = ac->c;
			return (0);
		}
	}
	/*
	 * We didn't find one in our cache.  Open one and cache it.
	 * Insert it at the head expecting LRU usage.
	 */
	WT_RET(__wt_calloc_def(session, 1, &ac));
	WT_ERR(wt_session->open_cursor(
	    wt_session, op->format->uri, NULL, op->format->config, &c));
	ac->cfg_hash = op->format->cfg_hash;
	ac->uri_hash = op->format->uri_hash;
	ac->c = c;
	STAILQ_INSERT_HEAD(&worker->cursorqh, ac, q);
	worker->num_cursors++;
	*cursorp = c;
	return (0);

err:	__wt_free(session, ac);
	return (ret);
}

/*
 * __async_worker_execop --
 *	A worker thread executes an individual op with a cursor.
 */
static int
__async_worker_execop(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_CURSOR *cursor)
{
	WT_ASYNC_OP *asyncop;
	WT_DECL_RET;
	WT_ITEM val;

	asyncop = (WT_ASYNC_OP *)op;
	/*
	 * Set the key of our local cursor from the async op handle.
	 * If needed, also set the value.
	 */
	__wt_cursor_set_raw_key(cursor, &asyncop->c.key);
	if (op->optype != WT_AOP_SEARCH && op->optype != WT_AOP_REMOVE)
		__wt_cursor_set_raw_value(cursor, &asyncop->c.value);
	switch (op->optype) {
		case WT_AOP_INSERT:
		case WT_AOP_UPDATE:
			WT_ERR(cursor->insert(cursor));
			break;
		case WT_AOP_REMOVE:
			WT_ERR(cursor->remove(cursor));
			break;
		case WT_AOP_SEARCH:
			WT_ERR(cursor->search(cursor));
			/*
			 * Get the value from the cursor and put it into
			 * the op for op->get_value.
			 */
			__wt_cursor_get_raw_value(cursor, &val);
			__wt_cursor_set_raw_value(&asyncop->c, &val);
			break;
		default:
			WT_ERR_MSG(session, EINVAL, "Unknown async optype %d\n",
			    op->optype);
	}
err:
	return (ret);
}

/*
 * __async_worker_op --
 *	A worker thread handles an individual op.
 */
static int
__async_worker_op(WT_SESSION_IMPL *session, WT_ASYNC_OP_IMPL *op,
    WT_ASYNC_WORKER_STATE *worker)
{
	WT_ASYNC_OP *asyncop;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int cb_ret;

	asyncop = (WT_ASYNC_OP *)op;

	ret = 0;
	cb_ret = 0;

	WT_RET(__wt_txn_begin(session, NULL));
	WT_ASSERT(session, op->state == WT_ASYNCOP_WORKING);
	WT_RET(__async_worker_cursor(session, op, worker, &cursor));
	/*
	 * Perform op and invoke the callback.
	 */
	ret = __async_worker_execop(session, op, cursor);
	if (op->cb != NULL && op->cb->notify != NULL)
		cb_ret = op->cb->notify(op->cb, asyncop, ret, 0);

	/*
	 * If the operation succeeded and the user callback returned
	 * zero then commit.  Otherwise rollback.
	 */
	if ((ret == 0 || ret == WT_NOTFOUND) && cb_ret == 0)
		WT_TRET(__wt_txn_commit(session, NULL));
	else
		WT_TRET(__wt_txn_rollback(session, NULL));
	/*
	 * After the callback returns, and the transaction resolved release
	 * the op back to the free pool and reset our cached cursor.
	 * We do this regardless of success or failure.
	 */
	F_CLR(&asyncop->c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	WT_PUBLISH(op->state, WT_ASYNCOP_FREE);
	cursor->reset(cursor);
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
	WT_ASYNC_CURSOR *ac, *acnext;
	WT_ASYNC_OP_IMPL *op;
	WT_ASYNC_WORKER_STATE worker;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	uint64_t flush_gen;
	uint32_t fc;

	session = arg;
	conn = S2C(session);
	async = conn->async;

	worker.num_cursors = 0;
	STAILQ_INIT(&worker.cursorqh);
	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		WT_ERR(__async_op_dequeue(conn, session, &op));
		if (op != NULL && op != &async->flush_op)
			WT_ERR(__async_worker_op(session, op, &worker));
		else if (async->flush_state == WT_ASYNC_FLUSHING) {
			/*
			 * Worker flushing going on.  Last worker to the party
			 * needs to clear the FLUSHING flag and signal the cond.
			 * If FLUSHING is going on, we do not take anything off
			 * the queue.
			 */
			WT_ORDERED_READ(flush_gen, async->flush_gen);
			if ((fc = WT_ATOMIC_ADD(async->flush_count, 1)) ==
			    conn->async_workers) {
				/*
				 * We're last.  All workers accounted for so
				 * signal the condition and clear the FLUSHING
				 * flag to release the other worker threads.
				 * Set the FLUSH_COMPLETE flag so that the
				 * caller can return to the application.
				 */
				WT_PUBLISH(async->flush_state,
				    WT_ASYNC_FLUSH_COMPLETE);
				WT_ERR(__wt_cond_signal(session,
				    async->flush_cond));
			} else
				/*
				 * We need to wait for the last worker to
				 * signal the condition.
				 */
				WT_ERR(__async_flush_wait(
				    session, async, flush_gen));
		}
	}

	if (0) {
err:		__wt_err(session, ret, "async worker error");
	}
	/*
	 * Worker thread cleanup, close our cached cursors and
	 * free all the WT_ASYNC_CURSOR structures.
	 */
	ac = STAILQ_FIRST(&worker.cursorqh);
	while (ac != NULL) {
		acnext = STAILQ_NEXT(ac, q);
		WT_TRET(ac->c->close(ac->c));
		__wt_free(session, ac);
		ac = acnext;
	}
	return (NULL);
}
