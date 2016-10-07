/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wtperf.h"

/* Default values. */
#define	DEFAULT_HOME		"WT_TEST"
#define	DEFAULT_MONITOR_DIR	"WT_TEST"

static const char * const debug_cconfig = "";
static const char * const debug_tconfig = "";

static void	*checkpoint_worker(void *);
static int	 drop_all_tables(WTPERF *);
static int	 execute_populate(WTPERF *);
static int	 execute_workload(WTPERF *);
static int	 find_table_count(WTPERF *);
static void	*monitor(void *);
static void	*populate_thread(void *);
static void	 randomize_value(WTPERF_THREAD *, char *);
static void	 recreate_dir(const char *);
static int	 start_all_runs(WTPERF *);
static int	 start_run(WTPERF *);
static int	 start_threads(WTPERF *,
		    WORKLOAD *, WTPERF_THREAD *, u_int, void *(*)(void *));
static int	 stop_threads(WTPERF *, u_int, WTPERF_THREAD *);
static void	*thread_run_wtperf(void *);
static void	 update_value_delta(WTPERF_THREAD *);
static void	*worker(void *);

static uint64_t	 wtperf_rand(WTPERF_THREAD *);
static uint64_t	 wtperf_value_range(WTPERF *);

#define	INDEX_COL_NAMES	"columns=(key,val)"

/* Retrieve an ID for the next insert operation. */
static inline uint64_t
get_next_incr(WTPERF *wtperf)
{
	return (__wt_atomic_add64(&wtperf->insert_key, 1));
}

/*
 * Each time this function is called we will overwrite the first and one
 * other element in the value buffer.
 */
static void
randomize_value(WTPERF_THREAD *thread, char *value_buf)
{
	CONFIG_OPTS *opts;
	uint8_t *vb;
	uint32_t i, max_range, rand_val;

	opts = thread->wtperf->opts;

	/*
	 * Limit how much of the buffer we validate for length, this means
	 * that only threads that do growing updates will ever make changes to
	 * values outside of the initial value size, but that's a fair trade
	 * off for avoiding figuring out how long the value is more accurately
	 * in this performance sensitive function.
	 */
	if (thread->workload == NULL || thread->workload->update_delta == 0)
		max_range = opts->value_sz;
	else if (thread->workload->update_delta > 0)
		max_range = opts->value_sz_max;
	else
		max_range = opts->value_sz_min;

	/*
	 * Generate a single random value and re-use it. We generally only
	 * have small ranges in this function, so avoiding a bunch of calls
	 * is worthwhile.
	 */
	rand_val = __wt_random(&thread->rnd);
	i = rand_val % (max_range - 1);

	/*
	 * Ensure we don't write past the end of a value when configured for
	 * randomly sized values.
	 */
	while (value_buf[i] == '\0' && i > 0)
		--i;

	vb = (uint8_t *)value_buf;
	vb[0] = ((rand_val >> 8) % 255) + 1;
	/*
	 * If i happened to be 0, we'll be re-writing the same value
	 * twice, but that doesn't matter.
	 */
	vb[i] = ((rand_val >> 16) % 255) + 1;
}

/*
 * Partition data by key ranges.
 */
static uint32_t
map_key_to_table(CONFIG_OPTS *opts, uint64_t k)
{
	if (opts->range_partition) {
		/* Take care to return a result in [0..table_count-1]. */
		if (k > opts->icount + opts->random_range)
			return (0);
		return ((uint32_t)((k - 1) /
		    ((opts->icount + opts->random_range +
		    opts->table_count - 1) / opts->table_count)));
	} else
		return ((uint32_t)(k % opts->table_count));
}

/*
 * Figure out and extend the size of the value string, used for growing
 * updates. We know that the value to be updated is in the threads value
 * scratch buffer.
 */
static inline void
update_value_delta(WTPERF_THREAD *thread)
{
	CONFIG_OPTS *opts;
	WTPERF *wtperf;
	char * value;
	int64_t delta, len, new_len;

	wtperf = thread->wtperf;
	opts = wtperf->opts;
	value = thread->value_buf;
	delta = thread->workload->update_delta;
	len = (int64_t)strlen(value);

	if (delta == INT64_MAX)
		delta = __wt_random(&thread->rnd) %
		    (opts->value_sz_max - opts->value_sz);

	/* Ensure we aren't changing across boundaries */
	if (delta > 0 && len + delta > opts->value_sz_max)
		delta = opts->value_sz_max - len;
	else if (delta < 0 && len + delta < opts->value_sz_min)
		delta = opts->value_sz_min - len;

	/* Bail if there isn't anything to do */
	if (delta == 0)
		return;

	if (delta < 0)
		value[len + delta] = '\0';
	else {
		/* Extend the value by the configured amount. */
		for (new_len = len;
		    new_len < opts->value_sz_max && new_len - len < delta;
		    new_len++)
			value[new_len] = 'a';
	}
}

static int
cb_asyncop(WT_ASYNC_CALLBACK *cb, WT_ASYNC_OP *op, int ret, uint32_t flags)
{
	TRACK *trk;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_ASYNC_OPTYPE type;
	uint32_t *tables;
	int t_ret;
	char *value;

	(void)cb;
	(void)flags;

	wtperf = NULL;			/* -Wconditional-uninitialized */
	thread = NULL;			/* -Wconditional-uninitialized */

	type = op->get_type(op);
	if (type != WT_AOP_COMPACT) {
		thread = (WTPERF_THREAD *)op->app_private;
		wtperf = thread->wtperf;
	}

	trk = NULL;
	switch (type) {
	case WT_AOP_COMPACT:
		tables = (uint32_t *)op->app_private;
		(void)__wt_atomic_add32(tables, (uint32_t)-1);
		break;
	case WT_AOP_INSERT:
		trk = &thread->insert;
		break;
	case WT_AOP_SEARCH:
		trk = &thread->read;
		if (ret == 0 &&
		    (t_ret = op->get_value(op, &value)) != 0) {
			ret = t_ret;
			lprintf(wtperf, ret, 0, "get_value in read.");
			goto err;
		}
		break;
	case WT_AOP_UPDATE:
		trk = &thread->update;
		break;
	case WT_AOP_NONE:
	case WT_AOP_REMOVE:
		/* We never expect this type. */
		lprintf(wtperf,
		    ret, 0, "No type in op %" PRIu64, op->get_id(op));
		goto err;
	}

	/*
	 * Either we have success and we track it, or failure and panic.
	 *
	 * Reads and updates can fail with WT_NOTFOUND: we may be searching
	 * in a random range, or an insert op might have updated the
	 * last record in the table but not yet finished the actual insert.
	 */
	if (type == WT_AOP_COMPACT)
		return (0);
	if (ret == 0 || (ret == WT_NOTFOUND && type != WT_AOP_INSERT)) {
		if (!wtperf->in_warmup)
			(void)__wt_atomic_add64(&trk->ops, 1);
		return (0);
	}
err:
	/* Panic if error */
	lprintf(wtperf, ret, 0, "Error in op %" PRIu64, op->get_id(op));
	wtperf->error = wtperf->stop = true;
	return (1);
}

static WT_ASYNC_CALLBACK cb = { cb_asyncop };

/*
 * track_operation --
 *	Update an operation's tracking structure with new latency information.
 */
static inline void
track_operation(TRACK *trk, uint64_t usecs)
{
	uint64_t v;

					/* average microseconds per call */
	v = (uint64_t)usecs;

	trk->latency += usecs;		/* track total latency */

	if (v > trk->max_latency)	/* track max/min latency */
		trk->max_latency = (uint32_t)v;
	if (v < trk->min_latency)
		trk->min_latency = (uint32_t)v;

	/*
	 * Update a latency bucket.
	 * First buckets: usecs from 100us to 1000us at 100us each.
	 */
	if (v < 1000)
		++trk->us[v];

	/*
	 * Second buckets: milliseconds from 1ms to 1000ms, at 1ms each.
	 */
	else if (v < ms_to_us(1000))
		++trk->ms[us_to_ms(v)];

	/*
	 * Third buckets are seconds from 1s to 100s, at 1s each.
	 */
	else if (v < sec_to_us(100))
		++trk->sec[us_to_sec(v)];

	/* >100 seconds, accumulate in the biggest bucket. */
	else
		++trk->sec[ELEMENTS(trk->sec) - 1];
}

static const char *
op_name(uint8_t *op)
{
	switch (*op) {
	case WORKER_INSERT:
		return ("insert");
	case WORKER_INSERT_RMW:
		return ("insert_rmw");
	case WORKER_READ:
		return ("read");
	case WORKER_TRUNCATE:
		return ("truncate");
	case WORKER_UPDATE:
		return ("update");
	default:
		return ("unknown");
	}
	/* NOTREACHED */
}

static void *
worker_async(void *arg)
{
	CONFIG_OPTS *opts;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_ASYNC_OP *asyncop;
	WT_CONNECTION *conn;
	uint64_t next_val;
	uint8_t *op, *op_end;
	int ret;
	char *key_buf, *value_buf;

	thread = (WTPERF_THREAD *)arg;
	wtperf = thread->wtperf;
	opts = wtperf->opts;
	conn = wtperf->conn;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	op = thread->workload->ops;
	op_end = op + sizeof(thread->workload->ops);

	while (!wtperf->stop) {
		/*
		 * Generate the next key and setup operation specific
		 * statistics tracking objects.
		 */
		switch (*op) {
		case WORKER_INSERT:
		case WORKER_INSERT_RMW:
			if (opts->random_range)
				next_val = wtperf_rand(thread);
			else
				next_val = opts->icount + get_next_incr(wtperf);
			break;
		case WORKER_READ:
		case WORKER_UPDATE:
			next_val = wtperf_rand(thread);

			/*
			 * If the workload is started without a populate phase
			 * we rely on at least one insert to get a valid item
			 * id.
			 */
			if (wtperf_value_range(wtperf) < next_val)
				continue;
			break;
		default:
			goto err;		/* can't happen */
		}

		generate_key(opts, key_buf, next_val);

		/*
		 * Spread the data out around the multiple databases.
		 * Sleep to allow workers a chance to run and process async ops.
		 * Then retry to get an async op.
		 */
		while ((ret = conn->async_new_op(conn,
		    wtperf->uris[map_key_to_table(wtperf->opts, next_val)],
		    NULL, &cb, &asyncop)) == EBUSY)
			(void)usleep(10000);
		if (ret != 0)
			goto err;

		asyncop->app_private = thread;
		asyncop->set_key(asyncop, key_buf);
		switch (*op) {
		case WORKER_READ:
			ret = asyncop->search(asyncop);
			if (ret == 0)
				break;
			goto op_err;
		case WORKER_INSERT:
			if (opts->random_value)
				randomize_value(thread, value_buf);
			asyncop->set_value(asyncop, value_buf);
			if ((ret = asyncop->insert(asyncop)) == 0)
				break;
			goto op_err;
		case WORKER_UPDATE:
			if (opts->random_value)
				randomize_value(thread, value_buf);
			asyncop->set_value(asyncop, value_buf);
			if ((ret = asyncop->update(asyncop)) == 0)
				break;
			goto op_err;
		default:
op_err:			lprintf(wtperf, ret, 0,
			    "%s failed for: %s, range: %"PRIu64,
			    op_name(op), key_buf, wtperf_value_range(wtperf));
			goto err;		/* can't happen */
		}

		/* Schedule the next operation */
		if (++op == op_end)
			op = thread->workload->ops;
	}

	if (conn->async_flush(conn) != 0)
		goto err;

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}
	return (NULL);
}

/*
 * do_range_reads --
 *	If configured to execute a sequence of next operations after each
 *	search do them. Ensuring the keys we see are always in order.
 */
static int
do_range_reads(WTPERF *wtperf, WT_CURSOR *cursor)
{
	CONFIG_OPTS *opts;
	size_t range;
	uint64_t next_val, prev_val;
	char *range_key_buf;
	char buf[512];
	int ret;

	opts = wtperf->opts;
	ret = 0;

	if (opts->read_range == 0)
		return (0);

	memset(&buf[0], 0, 512 * sizeof(char));
	range_key_buf = &buf[0];

	/* Save where the first key is for comparisons. */
	testutil_check(cursor->get_key(cursor, &range_key_buf));
	extract_key(range_key_buf, &next_val);

	for (range = 0; range < opts->read_range; ++range) {
		prev_val = next_val;
		ret = cursor->next(cursor);
		/* We are done if we reach the end. */
		if (ret != 0)
			break;

		/* Retrieve and decode the key */
		testutil_check(cursor->get_key(cursor, &range_key_buf));
		extract_key(range_key_buf, &next_val);
		if (next_val < prev_val) {
			lprintf(wtperf, EINVAL, 0,
			    "Out of order keys %" PRIu64
			    " came before %" PRIu64,
			    prev_val, next_val);
			return (EINVAL);
		}
	}
	return (0);
}

static void *
worker(void *arg)
{
	struct timespec start, stop;
	CONFIG_OPTS *opts;
	TRACK *trk;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_CONNECTION *conn;
	WT_CURSOR **cursors, *cursor, *log_table_cursor, *tmp_cursor;
	WT_SESSION *session;
	size_t i;
	int64_t ops, ops_per_txn;
	uint64_t log_id, next_val, usecs;
	uint8_t *op, *op_end;
	int measure_latency, ret, truncated;
	char *value_buf, *key_buf, *value;
	char buf[512];

	thread = (WTPERF_THREAD *)arg;
	wtperf = thread->wtperf;
	opts = wtperf->opts;
	conn = wtperf->conn;
	cursors = NULL;
	log_table_cursor = NULL;	/* -Wconditional-initialized */
	ops = 0;
	ops_per_txn = thread->workload->ops_per_txn;
	session = NULL;
	trk = NULL;

	if ((ret = conn->open_session(
	    conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0, "worker: WT_CONNECTION.open_session");
		goto err;
	}
	cursors = dcalloc(opts->table_count, sizeof(WT_CURSOR *));
	for (i = 0; i < opts->table_count_idle; i++) {
		snprintf(buf, 512, "%s_idle%05d", wtperf->uris[0], (int)i);
		if ((ret = session->open_cursor(
		    session, buf, NULL, NULL, &tmp_cursor)) != 0) {
			lprintf(wtperf, ret, 0,
			    "Error opening idle table %s", buf);
			goto err;
		}
		if ((ret = tmp_cursor->close(tmp_cursor)) != 0) {
			lprintf(wtperf, ret, 0,
			    "Error closing idle table %s", buf);
			goto err;
		}
	}
	for (i = 0; i < opts->table_count; i++) {
		if ((ret = session->open_cursor(session,
		    wtperf->uris[i], NULL, NULL, &cursors[i])) != 0) {
			lprintf(wtperf, ret, 0,
			    "worker: WT_SESSION.open_cursor: %s",
			    wtperf->uris[i]);
			goto err;
		}
	}
	if (opts->log_like_table && (ret = session->open_cursor(session,
	    wtperf->log_table_uri, NULL, NULL, &log_table_cursor)) != 0) {
		lprintf(wtperf, ret, 0,
		    "worker: WT_SESSION.open_cursor: %s",
		    wtperf->log_table_uri);
		goto err;
	}

	/* Setup the timer for throttling. */
	if (thread->workload->throttle != 0)
		setup_throttle(thread);

	/* Setup for truncate */
	if (thread->workload->truncate != 0)
		if ((ret = setup_truncate(wtperf, thread, session)) != 0)
			goto err;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	op = thread->workload->ops;
	op_end = op + sizeof(thread->workload->ops);

	if ((ops_per_txn != 0 || opts->log_like_table) &&
		(ret = session->begin_transaction(session, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "First transaction begin failed");
		goto err;
	}

	while (!wtperf->stop) {
		/*
		 * Generate the next key and setup operation specific
		 * statistics tracking objects.
		 */
		switch (*op) {
		case WORKER_INSERT:
		case WORKER_INSERT_RMW:
			trk = &thread->insert;
			if (opts->random_range)
				next_val = wtperf_rand(thread);
			else
				next_val = opts->icount + get_next_incr(wtperf);
			break;
		case WORKER_READ:
			trk = &thread->read;
			/* FALLTHROUGH */
		case WORKER_UPDATE:
			if (*op == WORKER_UPDATE)
				trk = &thread->update;
			next_val = wtperf_rand(thread);

			/*
			 * If the workload is started without a populate phase
			 * we rely on at least one insert to get a valid item
			 * id.
			 */
			if (wtperf_value_range(wtperf) < next_val)
				continue;
			break;
		case WORKER_TRUNCATE:
			/* Required but not used. */
			next_val = wtperf_rand(thread);
			break;
		default:
			goto err;		/* can't happen */
		}

		generate_key(opts, key_buf, next_val);

		/*
		 * Spread the data out around the multiple databases.
		 */
		cursor = cursors[map_key_to_table(wtperf->opts, next_val)];

		/*
		 * Skip the first time we do an operation, when trk->ops
		 * is 0, to avoid first time latency spikes.
		 */
		measure_latency =
		    opts->sample_interval != 0 && trk != NULL &&
		    trk->ops != 0 && (trk->ops % opts->sample_rate == 0);
		if (measure_latency)
			__wt_epoch(NULL, &start);

		cursor->set_key(cursor, key_buf);

		switch (*op) {
		case WORKER_READ:
			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			ret = cursor->search(cursor);
			if (ret == 0) {
				if ((ret = cursor->get_value(
				    cursor, &value)) != 0) {
					lprintf(wtperf, ret, 0,
					    "get_value in read.");
					goto err;
				}
				/*
				 * If we want to read a range, then call next
				 * for several operations, confirming that the
				 * next key is in the correct order.
				 */
				ret = do_range_reads(wtperf, cursor);
			}

			if (ret == 0 || ret == WT_NOTFOUND)
				break;
			goto op_err;
		case WORKER_INSERT_RMW:
			if ((ret = cursor->search(cursor)) != WT_NOTFOUND)
				goto op_err;

			/* The error return reset the cursor's key. */
			cursor->set_key(cursor, key_buf);

			/* FALLTHROUGH */
		case WORKER_INSERT:
			if (opts->random_value)
				randomize_value(thread, value_buf);
			cursor->set_value(cursor, value_buf);
			if ((ret = cursor->insert(cursor)) == 0)
				break;
			goto op_err;
		case WORKER_TRUNCATE:
			if ((ret = run_truncate(wtperf,
			    thread, cursor, session, &truncated)) == 0) {
				if (truncated)
					trk = &thread->truncate;
				else
					trk = &thread->truncate_sleep;
				/* Pause between truncate attempts */
				(void)usleep(1000);
				break;
			}
			goto op_err;
		case WORKER_UPDATE:
			if ((ret = cursor->search(cursor)) == 0) {
				if ((ret = cursor->get_value(
				    cursor, &value)) != 0) {
					lprintf(wtperf, ret, 0,
					    "get_value in update.");
					goto err;
				}
				/*
				 * Copy as much of the previous value as is
				 * safe, and be sure to NUL-terminate.
				 */
				strncpy(value_buf,
				    value, opts->value_sz_max - 1);
				if (thread->workload->update_delta != 0)
					update_value_delta(thread);
				if (value_buf[0] == 'a')
					value_buf[0] = 'b';
				else
					value_buf[0] = 'a';
				if (opts->random_value)
					randomize_value(thread, value_buf);
				cursor->set_value(cursor, value_buf);
				if ((ret = cursor->update(cursor)) == 0)
					break;
				goto op_err;
			}

			/*
			 * Reads can fail with WT_NOTFOUND: we may be searching
			 * in a random range, or an insert thread might have
			 * updated the last record in the table but not yet
			 * finished the actual insert.  Count failed search in
			 * a random range as a "read".
			 */
			if (ret == WT_NOTFOUND)
				break;

op_err:			if (ret == WT_ROLLBACK && ops_per_txn != 0) {
				/*
				 * If we are running with explicit transactions
				 * configured and we hit a WT_ROLLBACK, then we
				 * should rollback the current transaction and
				 * attempt to continue.
				 * This does break the guarantee of insertion
				 * order in cases of ordered inserts, as we
				 * aren't retrying here.
				 */
				lprintf(wtperf, ret, 1,
				    "%s for: %s, range: %"PRIu64, op_name(op),
				    key_buf, wtperf_value_range(wtperf));
				if ((ret = session->rollback_transaction(
				    session, NULL)) != 0) {
					lprintf(wtperf, ret, 0,
					     "Failed rollback_transaction");
					goto err;
				}
				if ((ret = session->begin_transaction(
				    session, NULL)) != 0) {
					lprintf(wtperf, ret, 0,
					    "Worker begin transaction failed");
					goto err;
				}
				break;
			}
			lprintf(wtperf, ret, 0,
			    "%s failed for: %s, range: %"PRIu64,
			    op_name(op), key_buf, wtperf_value_range(wtperf));
			goto err;
		default:
			goto err;		/* can't happen */
		}

		/* Update the log-like table. */
		if (opts->log_like_table &&
		    (*op != WORKER_READ && *op != WORKER_TRUNCATE)) {
			log_id =
			    __wt_atomic_add64(&wtperf->log_like_table_key, 1);
			log_table_cursor->set_key(log_table_cursor, log_id);
			log_table_cursor->set_value(
			    log_table_cursor, value_buf);
			if ((ret =
			    log_table_cursor->insert(log_table_cursor)) != 0) {
				lprintf(wtperf, ret, 0, "Cursor insert failed");
				goto err;
			}
		}

		/* Release the cursor, if we have multiple tables. */
		if (opts->table_count > 1 && ret == 0 &&
		    *op != WORKER_INSERT && *op != WORKER_INSERT_RMW) {
			if ((ret = cursor->reset(cursor)) != 0) {
				lprintf(wtperf, ret, 0, "Cursor reset failed");
				goto err;
			}
		}

		/* Gather statistics */
		if (!wtperf->in_warmup) {
			if (measure_latency) {
				__wt_epoch(NULL, &stop);
				++trk->latency_ops;
				usecs = WT_TIMEDIFF_US(stop, start);
				track_operation(trk, usecs);
			}
			/* Increment operation count */
			++trk->ops;
		}

		/*
		 * Commit the transaction if grouping operations together
		 * or tracking changes in our log table.
		 */
		if ((opts->log_like_table && ops_per_txn == 0) ||
		    (ops_per_txn != 0 && ops++ % ops_per_txn == 0)) {
			if ((ret = session->commit_transaction(
			    session, NULL)) != 0) {
				lprintf(wtperf, ret, 0,
				    "Worker transaction commit failed");
				goto err;
			}
			if ((ret = session->begin_transaction(
			    session, NULL)) != 0) {
				lprintf(wtperf, ret, 0,
				    "Worker begin transaction failed");
				goto err;
			}
		}

		/* Schedule the next operation */
		if (++op == op_end)
			op = thread->workload->ops;

		/*
		 * Decrement throttle ops and check if we should sleep
		 * and then get more work to perform.
		 */
		if (--thread->throttle_cfg.ops_count == 0)
			worker_throttle(thread);

	}

	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "Session close in worker failed");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}
	free(cursors);

	return (NULL);
}

/*
 * run_mix_schedule_op --
 *	Replace read operations with another operation, in the configured
 * percentage.
 */
static void
run_mix_schedule_op(WORKLOAD *workp, int op, int64_t op_cnt)
{
	int jump, pass;
	uint8_t *p, *end;

	/* Jump around the array to roughly spread out the operations. */
	jump = 100 / op_cnt;

	/*
	 * Find a read operation and replace it with another operation.  This
	 * is roughly n-squared, but it's an N of 100, leave it.
	 */
	p = workp->ops;
	end = workp->ops + sizeof(workp->ops);
	while (op_cnt-- > 0) {
		for (pass = 0; *p != WORKER_READ; ++p)
			if (p == end) {
				/*
				 * Passed a percentage of total operations and
				 * should always be a read operation to replace,
				 * but don't allow infinite loops.
				 */
				if (++pass > 1)
					return;
				p = workp->ops;
			}
		*p = (uint8_t)op;

		if (end - jump < p)
			p = workp->ops;
		else
			p += jump;
	}
}

/*
 * run_mix_schedule --
 *	Schedule the mixed-run operations.
 */
static int
run_mix_schedule(WTPERF *wtperf, WORKLOAD *workp)
{
	CONFIG_OPTS *opts;
	int64_t pct;

	opts = wtperf->opts;

	/* Confirm reads, inserts, truncates and updates cannot all be zero. */
	if (workp->insert == 0 && workp->read == 0 &&
	    workp->truncate == 0 && workp->update == 0) {
		lprintf(wtperf, EINVAL, 0, "no operations scheduled");
		return (EINVAL);
	}

	/*
	 * Handle truncate first - it's a special case that can't be used in
	 * a mixed workload.
	 */
	if (workp->truncate != 0) {
		if (workp->insert != 0 ||
		    workp->read != 0 || workp->update != 0) {
			lprintf(wtperf, EINVAL, 0,
			    "Can't configure truncate in a mixed workload");
			return (EINVAL);
		}
		memset(workp->ops, WORKER_TRUNCATE, sizeof(workp->ops));
		return (0);
	}

	/*
	 * Check for a simple case where the thread is only doing insert or
	 * update operations (because the default operation for a
	 * job-mix is read, the subsequent code works fine if only reads are
	 * specified).
	 */
	if (workp->insert != 0 && workp->read == 0 && workp->update == 0) {
		memset(workp->ops,
		    opts->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT,
		    sizeof(workp->ops));
		return (0);
	}
	if (workp->insert == 0 && workp->read == 0 && workp->update != 0) {
		memset(workp->ops, WORKER_UPDATE, sizeof(workp->ops));
		return (0);
	}

	/*
	 * The worker thread configuration is done as ratios of operations.  If
	 * the caller gives us something insane like "reads=77,updates=23" (do
	 * 77 reads for every 23 updates), we don't want to do 77 reads followed
	 * by 23 updates, we want to uniformly distribute the read and update
	 * operations across the space.  Convert to percentages and then lay out
	 * the operations across an array.
	 *
	 * Percentage conversion is lossy, the application can do stupid stuff
	 * here, for example, imagine a configured ratio of "reads=1,inserts=2,
	 * updates=999999".  First, if the percentages are skewed enough, some
	 * operations might never be done.  Second, we set the base operation to
	 * read, which means any fractional results from percentage conversion
	 * will be reads, implying read operations in some cases where reads
	 * weren't configured.  We should be fine if the application configures
	 * something approaching a rational set of ratios.
	 */
	memset(workp->ops, WORKER_READ, sizeof(workp->ops));

	pct = (workp->insert * 100) /
	    (workp->insert + workp->read + workp->update);
	if (pct != 0)
		run_mix_schedule_op(workp,
		    opts->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT, pct);
	pct = (workp->update * 100) /
	    (workp->insert + workp->read + workp->update);
	if (pct != 0)
		run_mix_schedule_op(workp, WORKER_UPDATE, pct);
	return (0);
}

static void *
populate_thread(void *arg)
{
	struct timespec start, stop;
	CONFIG_OPTS *opts;
	TRACK *trk;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_CONNECTION *conn;
	WT_CURSOR **cursors, *cursor;
	WT_SESSION *session;
	size_t i;
	uint64_t op, usecs;
	uint32_t opcount;
	int intxn, measure_latency, ret, stress_checkpoint_due;
	char *value_buf, *key_buf;
	const char *cursor_config;

	thread = (WTPERF_THREAD *)arg;
	wtperf = thread->wtperf;
	opts = wtperf->opts;
	conn = wtperf->conn;
	session = NULL;
	cursors = NULL;
	ret = stress_checkpoint_due = 0;
	trk = &thread->insert;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	if ((ret = conn->open_session(
	    conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0, "populate: WT_CONNECTION.open_session");
		goto err;
	}

	/* Do bulk loads if populate is single-threaded. */
	cursor_config =
	    (opts->populate_threads == 1 && !opts->index) ? "bulk" : NULL;
	/* Create the cursors. */
	cursors = dcalloc(opts->table_count, sizeof(WT_CURSOR *));
	for (i = 0; i < opts->table_count; i++) {
		if ((ret = session->open_cursor(
		    session, wtperf->uris[i], NULL,
		    cursor_config, &cursors[i])) != 0) {
			lprintf(wtperf, ret, 0,
			    "populate: WT_SESSION.open_cursor: %s",
			    wtperf->uris[i]);
			goto err;
		}
	}

	/* Populate the databases. */
	for (intxn = 0, opcount = 0;;) {
		op = get_next_incr(wtperf);
		if (op > opts->icount)
			break;

		if (opts->populate_ops_per_txn != 0 && !intxn) {
			if ((ret = session->begin_transaction(
			    session, opts->transaction_config)) != 0) {
				lprintf(wtperf, ret, 0,
				    "Failed starting transaction.");
				goto err;
			}
			intxn = 1;
		}
		/*
		 * Figure out which table this op belongs to.
		 */
		cursor = cursors[map_key_to_table(wtperf->opts, op)];
		generate_key(opts, key_buf, op);
		measure_latency =
		    opts->sample_interval != 0 &&
		    trk->ops != 0 && (trk->ops % opts->sample_rate == 0);
		if (measure_latency)
			__wt_epoch(NULL, &start);
		cursor->set_key(cursor, key_buf);
		if (opts->random_value)
			randomize_value(thread, value_buf);
		cursor->set_value(cursor, value_buf);
		if ((ret = cursor->insert(cursor)) == WT_ROLLBACK) {
			lprintf(wtperf, ret, 0, "insert retrying");
			if ((ret = session->rollback_transaction(
			    session, NULL)) != 0) {
				lprintf(wtperf, ret, 0,
				    "Failed rollback_transaction");
				goto err;
			}
			intxn = 0;
			continue;
		} else if (ret != 0) {
			lprintf(wtperf, ret, 0, "Failed inserting");
			goto err;
		}
		/*
		 * Gather statistics.
		 * We measure the latency of inserting a single key.  If there
		 * are multiple tables, it is the time for insertion into all
		 * of them.
		 */
		if (measure_latency) {
			__wt_epoch(NULL, &stop);
			++trk->latency_ops;
			usecs = WT_TIMEDIFF_US(stop, start);
			track_operation(trk, usecs);
		}
		++thread->insert.ops;	/* Same as trk->ops */

		if (opts->checkpoint_stress_rate != 0 &&
		    (op % opts->checkpoint_stress_rate) == 0)
			stress_checkpoint_due = 1;

		if (opts->populate_ops_per_txn != 0) {
			if (++opcount < opts->populate_ops_per_txn)
				continue;
			opcount = 0;

			if ((ret = session->commit_transaction(
			    session, NULL)) != 0)
				lprintf(wtperf, ret, 0,
				    "Fail committing, transaction was aborted");
			intxn = 0;
		}

		if (stress_checkpoint_due && intxn == 0) {
			stress_checkpoint_due = 0;
			if ((ret = session->checkpoint(session, NULL)) != 0) {
				lprintf(wtperf, ret, 0, "Checkpoint failed");
				goto err;
			}
		}
	}
	if (intxn &&
	    (ret = session->commit_transaction(session, NULL)) != 0)
		lprintf(wtperf, ret, 0,
		    "Fail committing, transaction was aborted");

	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "Error closing session in populate");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}
	free(cursors);

	return (NULL);
}

static void *
populate_async(void *arg)
{
	struct timespec start, stop;
	CONFIG_OPTS *opts;
	TRACK *trk;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_ASYNC_OP *asyncop;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	uint64_t op, usecs;
	int measure_latency, ret;
	char *value_buf, *key_buf;

	thread = (WTPERF_THREAD *)arg;
	wtperf = thread->wtperf;
	opts = wtperf->opts;
	conn = wtperf->conn;
	session = NULL;
	ret = 0;
	trk = &thread->insert;

	key_buf = thread->key_buf;
	value_buf = thread->value_buf;

	if ((ret = conn->open_session(
	    conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0, "populate: WT_CONNECTION.open_session");
		goto err;
	}

	/*
	 * Measuring latency of one async op is not meaningful.  We
	 * will measure the time it takes to do all of them, including
	 * the time to process by workers.
	 */
	measure_latency =
	    opts->sample_interval != 0 &&
	    trk->ops != 0 && (trk->ops % opts->sample_rate == 0);
	if (measure_latency)
		__wt_epoch(NULL, &start);

	/* Populate the databases. */
	for (;;) {
		op = get_next_incr(wtperf);
		if (op > opts->icount)
			break;
		/*
		 * Allocate an async op for whichever table.
		 */
		while ((ret = conn->async_new_op(
		    conn, wtperf->uris[map_key_to_table(wtperf->opts, op)],
		    NULL, &cb, &asyncop)) == EBUSY)
			(void)usleep(10000);
		if (ret != 0)
			goto err;

		asyncop->app_private = thread;
		generate_key(opts, key_buf, op);
		asyncop->set_key(asyncop, key_buf);
		if (opts->random_value)
			randomize_value(thread, value_buf);
		asyncop->set_value(asyncop, value_buf);
		if ((ret = asyncop->insert(asyncop)) != 0) {
			lprintf(wtperf, ret, 0, "Failed inserting");
			goto err;
		}
	}

	/*
	 * Gather statistics.
	 * We measure the latency of inserting a single key.  If there
	 * are multiple tables, it is the time for insertion into all
	 * of them.  Note that currently every populate thread will call
	 * async_flush and those calls will convoy.  That is not the
	 * most efficient way, but we want to flush before measuring latency.
	 */
	if (conn->async_flush(conn) != 0)
		goto err;
	if (measure_latency) {
		__wt_epoch(NULL, &stop);
		++trk->latency_ops;
		usecs = WT_TIMEDIFF_US(stop, start);
		track_operation(trk, usecs);
	}
	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "Error closing session in populate");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}
	return (NULL);
}

static void *
monitor(void *arg)
{
	struct timespec t;
	struct tm *tm, _tm;
	CONFIG_OPTS *opts;
	FILE *fp;
	WTPERF *wtperf;
	size_t len;
	uint64_t min_thr, reads, inserts, updates;
	uint64_t cur_reads, cur_inserts, cur_updates;
	uint64_t last_reads, last_inserts, last_updates;
	uint32_t read_avg, read_min, read_max;
	uint32_t insert_avg, insert_min, insert_max;
	uint32_t update_avg, update_min, update_max;
	uint32_t latency_max, level;
	u_int i;
	int msg_err;
	const char *str;
	char buf[64], *path;

	wtperf = (WTPERF *)arg;
	opts = wtperf->opts;
	assert(opts->sample_interval != 0);

	fp = NULL;
	path = NULL;

	min_thr = (uint64_t)opts->min_throughput;
	latency_max = (uint32_t)ms_to_us(opts->max_latency);

	/* Open the logging file. */
	len = strlen(wtperf->monitor_dir) + 100;
	path = dmalloc(len);
	snprintf(path, len, "%s/monitor", wtperf->monitor_dir);
	if ((fp = fopen(path, "w")) == NULL) {
		lprintf(wtperf, errno, 0, "%s", path);
		goto err;
	}
	/* Set line buffering for monitor file. */
	__wt_stream_set_line_buffer(fp);
	fprintf(fp,
	    "#time,"
	    "totalsec,"
	    "read ops per second,"
	    "insert ops per second,"
	    "update ops per second,"
	    "checkpoints,"
	    "read average latency(uS),"
	    "read minimum latency(uS),"
	    "read maximum latency(uS),"
	    "insert average latency(uS),"
	    "insert min latency(uS),"
	    "insert maximum latency(uS),"
	    "update average latency(uS),"
	    "update min latency(uS),"
	    "update maximum latency(uS)"
	    "\n");
	last_reads = last_inserts = last_updates = 0;
	while (!wtperf->stop) {
		for (i = 0; i < opts->sample_interval; i++) {
			sleep(1);
			if (wtperf->stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (wtperf->stop)
			break;
		if (wtperf->in_warmup)
			continue;

		__wt_epoch(NULL, &t);
		tm = localtime_r(&t.tv_sec, &_tm);
		(void)strftime(buf, sizeof(buf), "%b %d %H:%M:%S", tm);

		reads = sum_read_ops(wtperf);
		inserts = sum_insert_ops(wtperf);
		updates = sum_update_ops(wtperf);
		latency_read(wtperf, &read_avg, &read_min, &read_max);
		latency_insert(wtperf, &insert_avg, &insert_min, &insert_max);
		latency_update(wtperf, &update_avg, &update_min, &update_max);

		cur_reads = (reads - last_reads) / opts->sample_interval;
		cur_updates = (updates - last_updates) / opts->sample_interval;
		/*
		 * For now the only item we need to worry about changing is
		 * inserts when we transition from the populate phase to
		 * workload phase.
		 */
		if (inserts < last_inserts)
			cur_inserts = 0;
		else
			cur_inserts =
			    (inserts - last_inserts) / opts->sample_interval;

		(void)fprintf(fp,
		    "%s,%" PRIu32
		    ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
		    ",%c"
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		    "\n",
		    buf, wtperf->totalsec,
		    cur_reads, cur_inserts, cur_updates,
		    wtperf->ckpt ? 'Y' : 'N',
		    read_avg, read_min, read_max,
		    insert_avg, insert_min, insert_max,
		    update_avg, update_min, update_max);

		if (latency_max != 0 &&
		    (read_max > latency_max || insert_max > latency_max ||
		     update_max > latency_max)) {
			if (opts->max_latency_fatal) {
				level = 1;
				msg_err = WT_PANIC;
				str = "ERROR";
			} else {
				level = 0;
				msg_err = 0;
				str = "WARNING";
			}
			lprintf(wtperf, msg_err, level,
			    "%s: max latency exceeded: threshold %" PRIu32
			    " read max %" PRIu32 " insert max %" PRIu32
			    " update max %" PRIu32, str, latency_max,
			    read_max, insert_max, update_max);
		}
		if (min_thr != 0 &&
		    ((cur_reads != 0 && cur_reads < min_thr) ||
		    (cur_inserts != 0 && cur_inserts < min_thr) ||
		    (cur_updates != 0 && cur_updates < min_thr))) {
			if (opts->min_throughput_fatal) {
				level = 1;
				msg_err = WT_PANIC;
				str = "ERROR";
			} else {
				level = 0;
				msg_err = 0;
				str = "WARNING";
			}
			lprintf(wtperf, msg_err, level,
			    "%s: minimum throughput not met: threshold %" PRIu64
			    " reads %" PRIu64 " inserts %" PRIu64
			    " updates %" PRIu64, str, min_thr, cur_reads,
			    cur_inserts, cur_updates);
		}
		last_reads = reads;
		last_inserts = inserts;
		last_updates = updates;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}

	if (fp != NULL)
		(void)fclose(fp);
	free(path);

	return (NULL);
}

static void *
checkpoint_worker(void *arg)
{
	CONFIG_OPTS *opts;
	WTPERF *wtperf;
	WTPERF_THREAD *thread;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timespec e, s;
	uint32_t i;
	int ret;

	thread = (WTPERF_THREAD *)arg;
	wtperf = thread->wtperf;
	opts = wtperf->opts;
	conn = wtperf->conn;
	session = NULL;

	if ((ret = conn->open_session(
	    conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0,
		    "open_session failed in checkpoint thread.");
		goto err;
	}

	while (!wtperf->stop) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < opts->checkpoint_interval; i++) {
			sleep(1);
			if (wtperf->stop)
				break;
		}
		/* If the workers are done, don't bother with a final call. */
		if (wtperf->stop)
			break;

		__wt_epoch(NULL, &s);

		wtperf->ckpt = true;
		if ((ret = session->checkpoint(session, NULL)) != 0) {
			lprintf(wtperf, ret, 0, "Checkpoint failed.");
			goto err;
		}
		wtperf->ckpt = false;
		++thread->ckpt.ops;

		__wt_epoch(NULL, &e);
	}

	if (session != NULL &&
	    ((ret = session->close(session, NULL)) != 0)) {
		lprintf(wtperf, ret, 0,
		    "Error closing session in checkpoint worker.");
		goto err;
	}

	/* Notify our caller we failed and shut the system down. */
	if (0) {
err:		wtperf->error = wtperf->stop = true;
	}

	return (NULL);
}

static int
execute_populate(WTPERF *wtperf)
{
	struct timespec start, stop;
	CONFIG_OPTS *opts;
	WTPERF_THREAD *popth;
	WT_ASYNC_OP *asyncop;
	pthread_t idle_table_cycle_thread;
	size_t i;
	uint64_t last_ops, msecs, print_ops_sec;
	uint32_t interval, tables;
	double print_secs;
	int elapsed, ret;
	void *(*pfunc)(void *);

	opts = wtperf->opts;

	lprintf(wtperf, 0, 1,
	    "Starting %" PRIu32
	    " populate thread(s) for %" PRIu32 " items",
	    opts->populate_threads, opts->icount);

	/* Start cycling idle tables if configured. */
	if ((ret =
	    start_idle_table_cycle(wtperf, &idle_table_cycle_thread)) != 0)
		return (ret);

	wtperf->insert_key = 0;

	wtperf->popthreads =
	    dcalloc(opts->populate_threads, sizeof(WTPERF_THREAD));
	if (wtperf->use_asyncops) {
		lprintf(wtperf, 0, 1, "Starting %" PRIu32 " async thread(s)",
		    opts->async_threads);
		pfunc = populate_async;
	} else
		pfunc = populate_thread;
	if ((ret = start_threads(wtperf, NULL,
	    wtperf->popthreads, opts->populate_threads, pfunc)) != 0)
		return (ret);

	__wt_epoch(NULL, &start);
	for (elapsed = 0, interval = 0, last_ops = 0;
	    wtperf->insert_key < opts->icount && !wtperf->error;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, each 100th increment of elapsed is a single
		 * increment of interval.
		 */
		(void)usleep(10000);
		if (opts->report_interval == 0 || ++elapsed < 100)
			continue;
		elapsed = 0;
		if (++interval < opts->report_interval)
			continue;
		interval = 0;
		wtperf->totalsec += opts->report_interval;
		wtperf->insert_ops = sum_pop_ops(wtperf);
		lprintf(wtperf, 0, 1,
		    "%" PRIu64 " populate inserts (%" PRIu64 " of %"
		    PRIu32 ") in %" PRIu32 " secs (%" PRIu32 " total secs)",
		    wtperf->insert_ops - last_ops, wtperf->insert_ops,
		    opts->icount, opts->report_interval, wtperf->totalsec);
		last_ops = wtperf->insert_ops;
	}
	__wt_epoch(NULL, &stop);

	/*
	 * Move popthreads aside to narrow possible race with the monitor
	 * thread. The latency tracking code also requires that popthreads be
	 * NULL when the populate phase is finished, to know that the workload
	 * phase has started.
	 */
	popth = wtperf->popthreads;
	wtperf->popthreads = NULL;
	ret = stop_threads(wtperf, opts->populate_threads, popth);
	free(popth);
	if (ret != 0)
		return (ret);

	/* Report if any worker threads didn't finish. */
	if (wtperf->error) {
		lprintf(wtperf, WT_ERROR, 0,
		    "Populate thread(s) exited without finishing.");
		return (WT_ERROR);
	}

	lprintf(wtperf,
	    0, 1, "Finished load of %" PRIu32 " items", opts->icount);
	msecs = WT_TIMEDIFF_MS(stop, start);

	/*
	 * This is needed as the divisions will fail if the insert takes no time
	 * which will only be the case when there is no data to insert.
	 */
	if (msecs == 0) {
		print_secs = 0;
		print_ops_sec = 0;
	} else {
		print_secs = (double)msecs / (double)MSEC_PER_SEC;
		print_ops_sec = (uint64_t)(opts->icount / print_secs);
	}
	lprintf(wtperf, 0, 1,
	    "Load time: %.2f\n" "load ops/sec: %" PRIu64,
	    print_secs, print_ops_sec);

	/*
	 * If configured, compact to allow LSM merging to complete.  We
	 * set an unlimited timeout because if we close the connection
	 * then any in-progress compact/merge is aborted.
	 */
	if (opts->compact) {
		assert(opts->async_threads > 0);
		lprintf(wtperf, 0, 1, "Compact after populate");
		__wt_epoch(NULL, &start);
		tables = opts->table_count;
		for (i = 0; i < opts->table_count; i++) {
			/*
			 * If no ops are available, retry.  Any other error,
			 * return.
			 */
			 while ((ret = wtperf->conn->async_new_op(
			     wtperf->conn, wtperf->uris[i],
			     "timeout=0", &cb, &asyncop)) == EBUSY)
				(void)usleep(10000);
			if (ret != 0)
				return (ret);

			asyncop->app_private = &tables;
			if ((ret = asyncop->compact(asyncop)) != 0) {
				lprintf(wtperf,
				    ret, 0, "Async compact failed.");
				return (ret);
			}
		}
		if ((ret = wtperf->conn->async_flush(wtperf->conn)) != 0) {
			lprintf(wtperf, ret, 0, "Populate async flush failed.");
			return (ret);
		}
		__wt_epoch(NULL, &stop);
		lprintf(wtperf, 0, 1,
		    "Compact completed in %" PRIu64 " seconds",
		    (uint64_t)(WT_TIMEDIFF_SEC(stop, start)));
		assert(tables == 0);
	}

	/* Stop cycling idle tables. */
	if ((ret = stop_idle_table_cycle(wtperf, idle_table_cycle_thread)) != 0)
		return (ret);

	return (0);
}

static int
close_reopen(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	int ret;

	opts = wtperf->opts;

	if (!opts->readonly && !opts->reopen_connection)
		return (0);
	/*
	 * Reopen the connection.  We do this so that the workload phase always
	 * starts with the on-disk files, and so that read-only workloads can
	 * be identified.  This is particularly important for LSM, where the
	 * merge algorithm is more aggressive for read-only trees.
	 */
	/* wtperf->conn is released no matter the return value from close(). */
	ret = wtperf->conn->close(wtperf->conn, NULL);
	wtperf->conn = NULL;
	if (ret != 0) {
		lprintf(wtperf, ret, 0, "Closing the connection failed");
		return (ret);
	}
	if ((ret = wiredtiger_open(
	    wtperf->home, NULL, wtperf->reopen_config, &wtperf->conn)) != 0) {
		lprintf(wtperf, ret, 0, "Re-opening the connection failed");
		return (ret);
	}
	/*
	 * If we started async threads only for the purposes of compact,
	 * then turn it off before starting the workload so that those extra
	 * threads looking for work that will never arrive don't affect
	 * performance.
	 */
	if (opts->compact && !wtperf->use_asyncops) {
		if ((ret = wtperf->conn->reconfigure(
		    wtperf->conn, "async=(enabled=false)")) != 0) {
			lprintf(wtperf, ret, 0, "Reconfigure async off failed");
			return (ret);
		}
	}
	return (0);
}

static int
execute_workload(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	WORKLOAD *workp;
	WTPERF_THREAD *threads;
	WT_CONNECTION *conn;
	WT_SESSION **sessions;
	pthread_t idle_table_cycle_thread;
	uint64_t last_ckpts, last_inserts, last_reads, last_truncates;
	uint64_t last_updates;
	uint32_t interval, run_ops, run_time;
	u_int i;
	int ret, t_ret;
	void *(*pfunc)(void *);

	opts = wtperf->opts;

	wtperf->insert_key = 0;
	wtperf->insert_ops = wtperf->read_ops = wtperf->truncate_ops = 0;
	wtperf->update_ops = 0;

	last_ckpts = last_inserts = last_reads = last_truncates = 0;
	last_updates = 0;
	ret = 0;

	sessions = NULL;

	/* Start cycling idle tables. */
	if ((ret =
	    start_idle_table_cycle(wtperf, &idle_table_cycle_thread)) != 0)
		return (ret);

	if (opts->warmup != 0)
		wtperf->in_warmup = true;

	/* Allocate memory for the worker threads. */
	wtperf->workers =
	    dcalloc((size_t)wtperf->workers_cnt, sizeof(WTPERF_THREAD));

	if (wtperf->use_asyncops) {
		lprintf(wtperf, 0, 1, "Starting %" PRIu32 " async thread(s)",
		    opts->async_threads);
		pfunc = worker_async;
	} else
		pfunc = worker;

	if (opts->session_count_idle != 0) {
		sessions = dcalloc((size_t)opts->session_count_idle,
		    sizeof(WT_SESSION *));
		conn = wtperf->conn;
		for (i = 0; i < opts->session_count_idle; ++i)
			if ((ret = conn->open_session(conn,
			    NULL, opts->sess_config, &sessions[i])) != 0) {
				lprintf(wtperf, ret, 0,
				    "execute_workload: idle open_session");
				goto err;
			}
	}
	/* Start each workload. */
	for (threads = wtperf->workers, i = 0,
	    workp = wtperf->workload; i < wtperf->workload_cnt; ++i, ++workp) {
		lprintf(wtperf, 0, 1,
		    "Starting workload #%u: %" PRId64 " threads, inserts=%"
		    PRId64 ", reads=%" PRId64 ", updates=%" PRId64
		    ", truncate=%" PRId64 ", throttle=%" PRId64,
		    i + 1, workp->threads, workp->insert,
		    workp->read, workp->update, workp->truncate,
		    workp->throttle);

		/* Figure out the workload's schedule. */
		if ((ret = run_mix_schedule(wtperf, workp)) != 0)
			goto err;

		/* Start the workload's threads. */
		if ((ret = start_threads(
		    wtperf, workp, threads, (u_int)workp->threads, pfunc)) != 0)
			goto err;
		threads += workp->threads;
	}

	if (opts->warmup != 0) {
		lprintf(wtperf, 0, 1,
		    "Waiting for warmup duration of %" PRIu32, opts->warmup);
		sleep(opts->warmup);
		wtperf->in_warmup = false;
	}

	for (interval = opts->report_interval,
	    run_time = opts->run_time, run_ops = opts->run_ops;
	    !wtperf->error;) {
		/*
		 * Sleep for one second at a time.
		 * If we are tracking run time, check to see if we're done, and
		 * if we're only tracking run time, go back to sleep.
		 */
		sleep(1);
		if (run_time != 0) {
			if (--run_time == 0)
				break;
			if (!interval && !run_ops)
				continue;
		}

		/* Sum the operations we've done. */
		wtperf->ckpt_ops = sum_ckpt_ops(wtperf);
		wtperf->insert_ops = sum_insert_ops(wtperf);
		wtperf->read_ops = sum_read_ops(wtperf);
		wtperf->update_ops = sum_update_ops(wtperf);
		wtperf->truncate_ops = sum_truncate_ops(wtperf);

		/* If we're checking total operations, see if we're done. */
		if (run_ops != 0 && run_ops <=
		    wtperf->insert_ops + wtperf->read_ops + wtperf->update_ops)
			break;

		/* If writing out throughput information, see if it's time. */
		if (interval == 0 || --interval > 0)
			continue;
		interval = opts->report_interval;
		wtperf->totalsec += opts->report_interval;

		lprintf(wtperf, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates, %" PRIu64 " truncates, %" PRIu64
		    " checkpoints in %" PRIu32 " secs (%" PRIu32 " total secs)",
		    wtperf->read_ops - last_reads,
		    wtperf->insert_ops - last_inserts,
		    wtperf->update_ops - last_updates,
		    wtperf->truncate_ops - last_truncates,
		    wtperf->ckpt_ops - last_ckpts,
		    opts->report_interval, wtperf->totalsec);
		last_reads = wtperf->read_ops;
		last_inserts = wtperf->insert_ops;
		last_updates = wtperf->update_ops;
		last_truncates = wtperf->truncate_ops;
		last_ckpts = wtperf->ckpt_ops;
	}

	/* Notify the worker threads they are done. */
err:	wtperf->stop = true;

	/* Stop cycling idle tables. */
	if ((ret = stop_idle_table_cycle(wtperf, idle_table_cycle_thread)) != 0)
		return (ret);

	if ((t_ret = stop_threads(wtperf,
	    (u_int)wtperf->workers_cnt, wtperf->workers)) != 0 && ret == 0)
		ret = t_ret;

	/* Drop tables if configured to and this isn't an error path */
	if (ret == 0 &&
	    opts->drop_tables && (ret = drop_all_tables(wtperf)) != 0)
		lprintf(wtperf, ret, 0, "Drop tables failed.");

	free(sessions);
	/* Report if any worker threads didn't finish. */
	if (wtperf->error) {
		lprintf(wtperf, WT_ERROR, 0,
		    "Worker thread(s) exited without finishing.");
		if (ret == 0)
			ret = WT_ERROR;
	}
	return (ret);
}

/*
 * Ensure that icount matches the number of records in the
 * existing table.
 */
static int
find_table_count(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint32_t i, max_icount, table_icount;
	int ret, t_ret;
	char *key;

	opts = wtperf->opts;
	conn = wtperf->conn;

	max_icount = 0;
	if ((ret = conn->open_session(
	    conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0,
		    "find_table_count: open_session failed");
		goto out;
	}
	for (i = 0; i < opts->table_count; i++) {
		if ((ret = session->open_cursor(session, wtperf->uris[i],
		    NULL, NULL, &cursor)) != 0) {
			lprintf(wtperf, ret, 0,
			    "find_table_count: open_cursor failed");
			goto err;
		}
		if ((ret = cursor->prev(cursor)) != 0) {
			lprintf(wtperf, ret, 0,
			    "find_table_count: cursor prev failed");
			goto err;
		}
		if ((ret = cursor->get_key(cursor, &key)) != 0) {
			lprintf(wtperf, ret, 0,
			    "find_table_count: cursor get_key failed");
			goto err;
		}
		table_icount = (uint32_t)atoi(key);
		if (table_icount > max_icount)
			max_icount = table_icount;

		if ((ret = cursor->close(cursor)) != 0) {
			lprintf(wtperf, ret, 0,
			    "find_table_count: cursor close failed");
			goto err;
		}
	}
err:	if ((t_ret = session->close(session, NULL)) != 0) {
		if (ret == 0)
			ret = t_ret;
		lprintf(wtperf, ret, 0,
		    "find_table_count: session close failed");
	}
	opts->icount = max_icount;
out:	return (ret);
}

/*
 * Populate the uri array.
 */
static void
create_uris(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	size_t len;
	uint32_t i;

	opts = wtperf->opts;

	wtperf->uris = dcalloc(opts->table_count, sizeof(char *));
	len = strlen("table:") + strlen(opts->table_name) + 20;
	for (i = 0; i < opts->table_count; i++) {
		/* If there is only one table, just use the base name. */
		wtperf->uris[i] = dmalloc(len);
		if (opts->table_count == 1)
			snprintf(wtperf->uris[i],
			    len, "table:%s", opts->table_name);
		else
			snprintf(wtperf->uris[i],
			    len, "table:%s%05d", opts->table_name, i);
	}

	/* Create the log-like-table URI. */
	len = strlen("table:") +
	    strlen(opts->table_name) + strlen("_log_table") + 1;
	wtperf->log_table_uri = dmalloc(len);
	snprintf(
	    wtperf->log_table_uri, len, "table:%s_log_table", opts->table_name);
}

static int
create_tables(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	WT_SESSION *session;
	size_t i;
	int ret;
	char buf[512];

	opts = wtperf->opts;

	if ((ret = wtperf->conn->open_session(
	    wtperf->conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0,
		    "Error opening a session on %s", wtperf->home);
		return (ret);
	}

	for (i = 0; i < opts->table_count_idle; i++) {
		snprintf(buf, 512, "%s_idle%05d", wtperf->uris[0], (int)i);
		if ((ret = session->create(
		    session, buf, opts->table_config)) != 0) {
			lprintf(wtperf, ret, 0,
			    "Error creating idle table %s", buf);
			return (ret);
		}
	}
	if (opts->log_like_table && (ret = session->create(session,
	    wtperf->log_table_uri, "key_format=Q,value_format=S")) != 0) {
		lprintf(wtperf, ret, 0, "Error creating log table %s", buf);
		return (ret);
	}

	for (i = 0; i < opts->table_count; i++) {
		if (opts->log_partial && i > 0) {
			if (((ret = session->create(session,
			    wtperf->uris[i], wtperf->partial_config)) != 0)) {
				lprintf(wtperf, ret, 0,
				    "Error creating table %s", wtperf->uris[i]);
				return (ret);
			}
		} else if ((ret = session->create(
		    session, wtperf->uris[i], opts->table_config)) != 0) {
			lprintf(wtperf, ret, 0,
			    "Error creating table %s", wtperf->uris[i]);
			return (ret);
		}
		if (opts->index) {
			snprintf(buf, 512, "index:%s:val_idx",
			    wtperf->uris[i] + strlen("table:"));
			if ((ret = session->create(
			    session, buf, "columns=(val)")) != 0) {
				lprintf(wtperf, ret, 0,
				    "Error creating index %s", buf);
				return (ret);
			}
		}
	}

	if ((ret = session->close(session, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "Error closing session");
		return (ret);
	}

	return (0);
}

/*
 * wtperf_copy --
 *	Create a new WTPERF structure as a duplicate of a previous one.
 */
static void
wtperf_copy(const WTPERF *src, WTPERF **retp)
{
	CONFIG_OPTS *opts;
	WTPERF *dest;
	size_t i;

	opts = src->opts;

	dest = dcalloc(1, sizeof(WTPERF));

	/*
	 * Don't copy the home and monitor directories, they are filled in by
	 * our caller, explicitly.
	 */

	if (src->partial_config != NULL)
		dest->partial_config = dstrdup(src->partial_config);
	if (src->reopen_config != NULL)
		dest->reopen_config = dstrdup(src->reopen_config);

	if (src->uris != NULL) {
		dest->uris = dcalloc(opts->table_count, sizeof(char *));
		for (i = 0; i < opts->table_count; i++)
			dest->uris[i] = dstrdup(src->uris[i]);
	}

	if (src->async_config != NULL)
		dest->async_config = dstrdup(src->async_config);

	dest->ckptthreads = NULL;
	dest->popthreads = NULL;

	dest->workers = NULL;
	dest->workers_cnt = src->workers_cnt;
	if (src->workload_cnt != 0) {
		dest->workload_cnt = src->workload_cnt;
		dest->workload = dcalloc(src->workload_cnt, sizeof(WORKLOAD));
		memcpy(dest->workload,
		    src->workload, src->workload_cnt * sizeof(WORKLOAD));
	}

	TAILQ_INIT(&dest->stone_head);

	dest->opts = src->opts;

	*retp = dest;
}

/*
 * wtperf_free --
 *	Free any storage allocated in the WTPERF structure.
 */
static void
wtperf_free(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	size_t i;

	opts = wtperf->opts;

	free(wtperf->home);
	free(wtperf->monitor_dir);
	free(wtperf->partial_config);
	free(wtperf->reopen_config);
	free(wtperf->log_table_uri);

	if (wtperf->uris != NULL) {
		for (i = 0; i < opts->table_count; i++)
			free(wtperf->uris[i]);
		free(wtperf->uris);
	}

	free(wtperf->async_config);

	free(wtperf->ckptthreads);
	free(wtperf->popthreads);

	free(wtperf->workers);
	free(wtperf->workload);

	cleanup_truncate_config(wtperf);
}

/*
 * config_compress --
 *	Parse the compression configuration.
 */
static int
config_compress(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	int ret;
	const char *s;

	opts = wtperf->opts;
	ret = 0;

	s = opts->compression;
	if (strcmp(s, "none") == 0) {
		wtperf->compress_ext = NULL;
		wtperf->compress_table = NULL;
	} else if (strcmp(s, "lz4") == 0) {
#ifndef HAVE_BUILTIN_EXTENSION_LZ4
		wtperf->compress_ext = LZ4_EXT;
#endif
		wtperf->compress_table = LZ4_BLK;
	} else if (strcmp(s, "snappy") == 0) {
#ifndef HAVE_BUILTIN_EXTENSION_SNAPPY
		wtperf->compress_ext = SNAPPY_EXT;
#endif
		wtperf->compress_table = SNAPPY_BLK;
	} else if (strcmp(s, "zlib") == 0) {
#ifndef HAVE_BUILTIN_EXTENSION_ZLIB
		wtperf->compress_ext = ZLIB_EXT;
#endif
		wtperf->compress_table = ZLIB_BLK;
	} else {
		fprintf(stderr,
	    "invalid compression configuration: %s\n", s);
		ret = EINVAL;
	}
	return (ret);

}

static int
start_all_runs(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	WTPERF *next_wtperf, **wtperfs;
	pthread_t *threads;
	size_t i, len;
	int ret, t_ret;

	opts = wtperf->opts;
	wtperfs = NULL;
	ret = 0;

	if (opts->database_count == 1)
		return (start_run(wtperf));

	/* Allocate an array to hold our WTPERF copies. */
	wtperfs = dcalloc(opts->database_count, sizeof(WTPERF *));

	/* Allocate an array to hold our thread IDs. */
	threads = dcalloc(opts->database_count, sizeof(pthread_t));

	for (i = 0; i < opts->database_count; i++) {
		wtperf_copy(wtperf, &next_wtperf);
		wtperfs[i] = next_wtperf;

		/*
		 * Set up unique home/monitor directories for each database.
		 * Re-create the directories if creating the databases.
		 */
		len = strlen(wtperf->home) + 5;
		next_wtperf->home = dmalloc(len);
		snprintf(
		    next_wtperf->home, len, "%s/D%02d", wtperf->home, (int)i);
		if (opts->create != 0)
			recreate_dir(next_wtperf->home);

		len = strlen(wtperf->monitor_dir) + 5;
		next_wtperf->monitor_dir = dmalloc(len);
		snprintf(next_wtperf->monitor_dir,
		    len, "%s/D%02d", wtperf->monitor_dir, (int)i);
		if (opts->create != 0 &&
		    strcmp(next_wtperf->home, next_wtperf->monitor_dir) != 0)
			recreate_dir(next_wtperf->monitor_dir);

		if ((ret = pthread_create(
		    &threads[i], NULL, thread_run_wtperf, next_wtperf)) != 0) {
			lprintf(wtperf, ret, 0, "Error creating thread");
			goto err;
		}
	}

	/* Wait for threads to finish. */
	for (i = 0; i < opts->database_count; i++)
		if ((t_ret = pthread_join(threads[i], NULL)) != 0) {
			lprintf(wtperf, ret, 0, "Error joining thread");
			if (ret == 0)
				ret = t_ret;
		}

err:	for (i = 0; i < opts->database_count && wtperfs[i] != NULL; i++) {
		wtperf_free(wtperfs[i]);
		free(wtperfs[i]);
	}
	free(wtperfs);
	free(threads);

	return (ret);
}

/* Run an instance of wtperf for a given configuration. */
static void *
thread_run_wtperf(void *arg)
{
	WTPERF *wtperf;
	int ret;

	wtperf = (WTPERF *)arg;
	if ((ret = start_run(wtperf)) != 0)
		lprintf(wtperf, ret, 0, "Run failed for: %s.", wtperf->home);
	return (NULL);
}

static int
start_run(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;
	pthread_t monitor_thread;
	uint64_t total_ops;
	uint32_t run_time;
	int monitor_created, ret, t_ret;

	opts = wtperf->opts;
	monitor_created = ret = 0;
					/* [-Wconditional-uninitialized] */
	memset(&monitor_thread, 0, sizeof(monitor_thread));

	if ((ret = setup_log_file(wtperf)) != 0)
		goto err;

	if ((ret = wiredtiger_open(	/* Open the real connection. */
	    wtperf->home, NULL, opts->conn_config, &wtperf->conn)) != 0) {
		lprintf(wtperf, ret, 0, "Error connecting to %s", wtperf->home);
		goto err;
	}

	create_uris(wtperf);

	/* If creating, create the tables. */
	if (opts->create != 0 && (ret = create_tables(wtperf)) != 0)
		goto err;

	/* Start the monitor thread. */
	if (opts->sample_interval != 0) {
		if ((ret = pthread_create(
		    &monitor_thread, NULL, monitor, wtperf)) != 0) {
			lprintf(wtperf,
			    ret, 0, "Error creating monitor thread.");
			goto err;
		}
		monitor_created = 1;
	}

	/* If creating, populate the table. */
	if (opts->create != 0 && execute_populate(wtperf) != 0)
		goto err;

	/* Optional workload. */
	if (wtperf->workers_cnt != 0 &&
	    (opts->run_time != 0 || opts->run_ops != 0)) {
		/*
		 * If we have a workload, close and reopen the connection so
		 * that LSM can detect read-only workloads.
		 */
		if (close_reopen(wtperf) != 0)
			goto err;

		/* Didn't create, set insert count. */
		if (opts->create == 0 &&
		    opts->random_range == 0 && find_table_count(wtperf) != 0)
			goto err;
		/* Start the checkpoint thread. */
		if (opts->checkpoint_threads != 0) {
			lprintf(wtperf, 0, 1,
			    "Starting %" PRIu32 " checkpoint thread(s)",
			    opts->checkpoint_threads);
			wtperf->ckptthreads = dcalloc(
			     opts->checkpoint_threads, sizeof(WTPERF_THREAD));
			if (start_threads(wtperf, NULL, wtperf->ckptthreads,
			    opts->checkpoint_threads, checkpoint_worker) != 0)
				goto err;
		}
		/* Execute the workload. */
		if ((ret = execute_workload(wtperf)) != 0)
			goto err;

		/* One final summation of the operations we've completed. */
		wtperf->read_ops = sum_read_ops(wtperf);
		wtperf->insert_ops = sum_insert_ops(wtperf);
		wtperf->truncate_ops = sum_truncate_ops(wtperf);
		wtperf->update_ops = sum_update_ops(wtperf);
		wtperf->ckpt_ops = sum_ckpt_ops(wtperf);
		total_ops =
		    wtperf->read_ops + wtperf->insert_ops + wtperf->update_ops;

		run_time = opts->run_time == 0 ? 1 : opts->run_time;
		lprintf(wtperf, 0, 1,
		    "Executed %" PRIu64 " read operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    wtperf->read_ops, (wtperf->read_ops * 100) / total_ops,
		    wtperf->read_ops / run_time);
		lprintf(wtperf, 0, 1,
		    "Executed %" PRIu64 " insert operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    wtperf->insert_ops, (wtperf->insert_ops * 100) / total_ops,
		    wtperf->insert_ops / run_time);
		lprintf(wtperf, 0, 1,
		    "Executed %" PRIu64 " truncate operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    wtperf->truncate_ops,
		    (wtperf->truncate_ops * 100) / total_ops,
		    wtperf->truncate_ops / run_time);
		lprintf(wtperf, 0, 1,
		    "Executed %" PRIu64 " update operations (%" PRIu64
		    "%%) %" PRIu64 " ops/sec",
		    wtperf->update_ops, (wtperf->update_ops * 100) / total_ops,
		    wtperf->update_ops / run_time);
		lprintf(wtperf, 0, 1,
		    "Executed %" PRIu64 " checkpoint operations",
		    wtperf->ckpt_ops);

		latency_print(wtperf);
	}

	if (0) {
err:		if (ret == 0)
			ret = EXIT_FAILURE;
	}

	/* Notify the worker threads they are done. */
	wtperf->stop = true;

	if ((t_ret = stop_threads(wtperf, 1, wtperf->ckptthreads)) != 0)
		if (ret == 0)
			ret = t_ret;

	if (monitor_created != 0 &&
	    (t_ret = pthread_join(monitor_thread, NULL)) != 0) {
		lprintf(wtperf, ret, 0, "Error joining monitor thread.");
		if (ret == 0)
			ret = t_ret;
	}

	if (wtperf->conn != NULL &&
	    (t_ret = wtperf->conn->close(wtperf->conn, NULL)) != 0) {
		lprintf(wtperf, t_ret, 0,
		    "Error closing connection to %s", wtperf->home);
		if (ret == 0)
			ret = t_ret;
	}

	if (ret == 0) {
		if (opts->run_time == 0 && opts->run_ops == 0)
			lprintf(wtperf, 0, 1, "Run completed");
		else
			lprintf(wtperf, 0, 1, "Run completed: %" PRIu32 " %s",
			    opts->run_time == 0 ?
			    opts->run_ops : opts->run_time,
			    opts->run_time == 0 ? "operations" : "seconds");
	}

	if (wtperf->logf != NULL) {
		if ((t_ret = fflush(wtperf->logf)) != 0 && ret == 0)
			ret = t_ret;
		if ((t_ret = fclose(wtperf->logf)) != 0 && ret == 0)
			ret = t_ret;
	}
	return (ret);
}

extern int __wt_optind, __wt_optreset;
extern char *__wt_optarg;
void (*custom_die)(void) = NULL;

/*
 * usage --
 *	wtperf usage print, no error.
 */
static void
usage(void)
{
	printf("wtperf [-C config] "
	    "[-H mount] [-h home] [-O file] [-o option] [-T config]\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-H <mount> configure Helium volume mount point\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-O <file> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\n");
	config_opt_usage();
}

int
main(int argc, char *argv[])
{
	CONFIG_OPTS *opts;
	WTPERF *wtperf, _wtperf;
	size_t req_len, sreq_len;
	bool monitor_set;
	int ch, ret;
	const char *cmdflags = "C:h:m:O:o:T:";
	const char *config_opts;
	char *cc_buf, *path, *sess_cfg, *tc_buf, *user_cconfig, *user_tconfig;

	/* The first WTPERF structure (from which all others are derived). */
	wtperf = &_wtperf;
	memset(wtperf, 0, sizeof(*wtperf));
	wtperf->home = dstrdup(DEFAULT_HOME);
	wtperf->monitor_dir = dstrdup(DEFAULT_MONITOR_DIR);
	TAILQ_INIT(&wtperf->stone_head);
	config_opt_init(&wtperf->opts);

	opts = wtperf->opts;
	monitor_set = false;
	ret = 0;
	config_opts = NULL;
	cc_buf = sess_cfg = tc_buf = user_cconfig = user_tconfig = NULL;

	/* Do a basic validation of options, and home is needed before open. */
	while ((ch = __wt_getopt("wtperf", argc, argv, cmdflags)) != EOF)
		switch (ch) {
		case 'C':
			if (user_cconfig == NULL)
				user_cconfig = dstrdup(__wt_optarg);
			else {
				user_cconfig = drealloc(user_cconfig,
				    strlen(user_cconfig) +
				    strlen(__wt_optarg) + 2);
				strcat(user_cconfig, ",");
				strcat(user_cconfig, __wt_optarg);
			}
			break;
		case 'h':
			free(wtperf->home);
			wtperf->home = dstrdup(__wt_optarg);
			break;
		case 'm':
			free(wtperf->monitor_dir);
			wtperf->monitor_dir = dstrdup(__wt_optarg);
			monitor_set = true;
			break;
		case 'O':
			config_opts = __wt_optarg;
			break;
		case 'T':
			if (user_tconfig == NULL)
				user_tconfig = dstrdup(__wt_optarg);
			else {
				user_tconfig = drealloc(user_tconfig,
				    strlen(user_tconfig) +
				    strlen(__wt_optarg) + 2);
				strcat(user_tconfig, ",");
				strcat(user_tconfig, __wt_optarg);
			}
			break;
		case '?':
			usage();
			goto einval;
		}

	/*
	 * If the user did not specify a monitor directory then set the
	 * monitor directory to the home dir.
	 */
	if (!monitor_set) {
		free(wtperf->monitor_dir);
		wtperf->monitor_dir = dstrdup(wtperf->home);
	}

	/* Parse configuration settings from configuration file. */
	if (config_opts != NULL && config_opt_file(wtperf, config_opts) != 0)
		goto einval;

	/* Parse options that override values set via a configuration file. */
	__wt_optreset = __wt_optind = 1;
	while ((ch = __wt_getopt("wtperf", argc, argv, cmdflags)) != EOF)
		switch (ch) {
		case 'o':
			/* Allow -o key=value */
			if (config_opt_str(wtperf, __wt_optarg) != 0)
				goto einval;
			break;
		}

	if (opts->populate_threads == 0 && opts->icount != 0) {
		lprintf(wtperf, 1, 0,
		    "Cannot have 0 populate threads when icount is set\n");
		goto err;
	}

	wtperf->async_config = NULL;
	/*
	 * If the user specified async_threads we use async for all ops.
	 * If the user wants compaction, then we also enable async for
	 * the compact operation, but not for the workloads.
	 */
	if (opts->async_threads > 0) {
		if (F_ISSET(wtperf, CFG_TRUNCATE)) {
			lprintf(wtperf,
			    1, 0, "Cannot run truncate and async\n");
			goto err;
		}
		wtperf->use_asyncops = true;
	}
	if (opts->compact && opts->async_threads == 0)
		opts->async_threads = 2;
	if (opts->async_threads > 0) {
		/*
		 * The maximum number of async threads is two digits, so just
		 * use that to compute the space we need.  Assume the default
		 * of 1024 for the max ops.  Although we could bump that up
		 * to 4096 if needed.
		 */
		req_len = strlen(",async=(enabled=true,threads=)") + 4;
		wtperf->async_config = dmalloc(req_len);
		snprintf(wtperf->async_config, req_len,
		    ",async=(enabled=true,threads=%" PRIu32 ")",
		    opts->async_threads);
	}
	if ((ret = config_compress(wtperf)) != 0)
		goto err;

	/* You can't have truncate on a random collection. */
	if (F_ISSET(wtperf, CFG_TRUNCATE) && opts->random_range) {
		lprintf(wtperf, 1, 0, "Cannot run truncate and random_range\n");
		goto err;
	}

	/* We can't run truncate with more than one table. */
	if (F_ISSET(wtperf, CFG_TRUNCATE) && opts->table_count > 1) {
		lprintf(wtperf, 1, 0, "Cannot truncate more than 1 table\n");
		goto err;
	}

	/* Make stdout line buffered, so verbose output appears quickly. */
	__wt_stream_set_line_buffer(stdout);

	/* Concatenate non-default configuration strings. */
	if (opts->verbose > 1 || user_cconfig != NULL ||
	    opts->session_count_idle > 0 || wtperf->compress_ext != NULL ||
	    wtperf->async_config != NULL) {
		req_len = strlen(debug_cconfig) + 20;
		if (user_cconfig != NULL)
			req_len += strlen(user_cconfig);
		if (wtperf->async_config != NULL)
			req_len += strlen(wtperf->async_config);
		if (wtperf->compress_ext != NULL)
			req_len += strlen(wtperf->compress_ext);
		if (opts->session_count_idle > 0) {
			sreq_len = strlen(",session_max=") + 6;
			req_len += sreq_len;
			sess_cfg = dmalloc(sreq_len);
			snprintf(sess_cfg, sreq_len,
			    ",session_max=%" PRIu32,
			    opts->session_count_idle +
			    wtperf->workers_cnt + opts->populate_threads + 10);
		}
		cc_buf = dmalloc(req_len);
		snprintf(cc_buf, req_len, "%s,%s,%s,%s,%s",
		    wtperf->async_config ? wtperf->async_config : "",
		    wtperf->compress_ext ? wtperf->compress_ext : "",
		    opts->verbose > 1 ? debug_cconfig : "",
		    sess_cfg != NULL ? sess_cfg : "",
		    user_cconfig != NULL ? user_cconfig : "");
		if (strlen(cc_buf) && (ret =
		    config_opt_name_value(wtperf, "conn_config", cc_buf)) != 0)
			goto err;
	}
	if (opts->verbose > 1 || opts->index ||
	    user_tconfig != NULL || wtperf->compress_table != NULL) {
		req_len = strlen(debug_tconfig) + 20;
		if (user_tconfig != NULL)
			req_len += strlen(user_tconfig);
		if (wtperf->compress_table != NULL)
			req_len += strlen(wtperf->compress_table);
		if (opts->index)
			req_len += strlen(INDEX_COL_NAMES);
		tc_buf = dmalloc(req_len);
		snprintf(tc_buf, req_len, "%s,%s,%s,%s",
		    opts->index ? INDEX_COL_NAMES : "",
		    wtperf->compress_table != NULL ?
		    wtperf->compress_table : "",
		    opts->verbose > 1 ? debug_tconfig : "",
		    user_tconfig ? user_tconfig : "");
		if (strlen(tc_buf) && (ret =
		    config_opt_name_value(wtperf, "table_config", tc_buf)) != 0)
			goto err;
	}
	if (opts->log_partial && opts->table_count > 1) {
		req_len = strlen(opts->table_config) +
		    strlen(LOG_PARTIAL_CONFIG) + 1;
		wtperf->partial_config = dmalloc(req_len);
		snprintf(wtperf->partial_config, req_len, "%s%s",
		    opts->table_config, LOG_PARTIAL_CONFIG);
	}
	/*
	 * Set the config for reopen.  If readonly add in that string.
	 * If not readonly then just copy the original conn_config.
	 */
	if (opts->readonly)
		req_len = strlen(opts->conn_config) +
		    strlen(READONLY_CONFIG) + 1;
	else
		req_len = strlen(opts->conn_config) + 1;
	wtperf->reopen_config = dmalloc(req_len);
	if (opts->readonly)
		snprintf(wtperf->reopen_config, req_len, "%s%s",
		    opts->conn_config, READONLY_CONFIG);
	else
		snprintf(wtperf->reopen_config,
		    req_len, "%s", opts->conn_config);

	/* Sanity-check the configuration. */
	if ((ret = config_sanity(wtperf)) != 0)
		goto err;

	/* If creating, remove and re-create the home directory. */
	if (opts->create != 0)
		recreate_dir(wtperf->home);

	/* Write a copy of the config. */
	req_len = strlen(wtperf->home) + strlen("/CONFIG.wtperf") + 1;
	path = dmalloc(req_len);
	snprintf(path, req_len, "%s/CONFIG.wtperf", wtperf->home);
	config_opt_log(opts, path);
	free(path);

	/* Display the configuration. */
	if (opts->verbose > 1)
		config_opt_print(wtperf);

	if ((ret = start_all_runs(wtperf)) != 0)
		goto err;

	if (0) {
einval:		ret = EINVAL;
	}

err:	wtperf_free(wtperf);
	config_opt_cleanup(opts);

	free(cc_buf);
	free(sess_cfg);
	free(tc_buf);
	free(user_cconfig);
	free(user_tconfig);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

static int
start_threads(WTPERF *wtperf,
    WORKLOAD *workp, WTPERF_THREAD *base, u_int num, void *(*func)(void *))
{
	CONFIG_OPTS *opts;
	WTPERF_THREAD *thread;
	u_int i;
	int ret;

	opts = wtperf->opts;

	/* Initialize the threads. */
	for (i = 0, thread = base; i < num; ++i, ++thread) {
		thread->wtperf = wtperf;
		thread->workload = workp;

		/*
		 * We don't want the threads executing in lock-step, seed each
		 * one differently.
		 */
		__wt_random_init_seed(NULL, &thread->rnd);

		/*
		 * Every thread gets a key/data buffer because we don't bother
		 * to distinguish between threads needing them and threads that
		 * don't, it's not enough memory to bother.  These buffers hold
		 * strings: trailing NUL is included in the size.
		 */
		thread->key_buf = dcalloc(opts->key_sz, 1);
		thread->value_buf = dcalloc(opts->value_sz_max, 1);

		/*
		 * Initialize and then toss in a bit of random values if needed.
		 */
		memset(thread->value_buf, 'a', opts->value_sz - 1);
		if (opts->random_value)
			randomize_value(thread, thread->value_buf);

		/*
		 * Every thread gets tracking information and is initialized
		 * for latency measurements, for the same reason.
		 */
		thread->ckpt.min_latency =
		thread->insert.min_latency = thread->read.min_latency =
		thread->update.min_latency = UINT32_MAX;
		thread->ckpt.max_latency = thread->insert.max_latency =
		thread->read.max_latency = thread->update.max_latency = 0;
	}

	/* Start the threads. */
	for (i = 0, thread = base; i < num; ++i, ++thread)
		if ((ret = pthread_create(
		    &thread->handle, NULL, func, thread)) != 0) {
			lprintf(wtperf, ret, 0, "Error creating thread");
			return (ret);
		}

	return (0);
}

static int
stop_threads(WTPERF *wtperf, u_int num, WTPERF_THREAD *threads)
{
	u_int i;
	int ret;

	if (num == 0 || threads == NULL)
		return (0);

	for (i = 0; i < num; ++i, ++threads) {
		if ((ret = pthread_join(threads->handle, NULL)) != 0) {
			lprintf(wtperf, ret, 0, "Error joining thread");
			return (ret);
		}

		free(threads->key_buf);
		threads->key_buf = NULL;
		free(threads->value_buf);
		threads->value_buf = NULL;
	}

	/*
	 * We don't free the thread structures or any memory referenced, or NULL
	 * the reference when we stop the threads; the thread structure is still
	 * being read by the monitor thread (among others).  As a standalone
	 * program, leaking memory isn't a concern, and it's simpler that way.
	 */
	return (0);
}

static void
recreate_dir(const char *name)
{
	char *buf;
	size_t len;

	len = strlen(name) * 2 + 100;
	buf = dmalloc(len);
	(void)snprintf(buf, len, "rm -rf %s && mkdir %s", name, name);
	testutil_checkfmt(system(buf), "system: %s", buf);
	free(buf);
}

static int
drop_all_tables(WTPERF *wtperf)
{
	struct timespec start, stop;
	CONFIG_OPTS *opts;
	WT_SESSION *session;
	size_t i;
	uint64_t msecs;
	int ret, t_ret;

	opts = wtperf->opts;

	/* Drop any tables. */
	if ((ret = wtperf->conn->open_session(
	    wtperf->conn, NULL, opts->sess_config, &session)) != 0) {
		lprintf(wtperf, ret, 0,
		    "Error opening a session on %s", wtperf->home);
		return (ret);
	}
	__wt_epoch(NULL, &start);
	for (i = 0; i < opts->table_count; i++) {
		if ((ret =
		    session->drop(session, wtperf->uris[i], NULL)) != 0) {
			lprintf(wtperf, ret, 0,
			    "Error dropping table %s", wtperf->uris[i]);
			goto err;
		}
	}
	__wt_epoch(NULL, &stop);
	msecs = WT_TIMEDIFF_MS(stop, start);
	lprintf(wtperf, 0, 1,
	    "Executed %" PRIu32 " drop operations average time %" PRIu64 "ms",
	    opts->table_count, msecs / opts->table_count);

err:	if ((t_ret = session->close(session, NULL)) != 0 && ret == 0)
		ret = t_ret;
	return (ret);
}

static uint64_t
wtperf_value_range(WTPERF *wtperf)
{
	CONFIG_OPTS *opts;

	opts = wtperf->opts;

	if (opts->random_range)
		return (opts->icount + opts->random_range);
	/*
	 * It is legal to configure a zero size populate phase, hide that
	 * from other code by pretending the range is 1 in that case.
	 */
	if (opts->icount + wtperf->insert_key == 0)
		return (1);
	return (opts->icount +
	    wtperf->insert_key - (u_int)(wtperf->workers_cnt + 1));
}

static uint64_t
wtperf_rand(WTPERF_THREAD *thread)
{
	CONFIG_OPTS *opts;
	WTPERF *wtperf;
	double S1, S2, U;
	uint64_t rval;

	wtperf = thread->wtperf;
	opts = wtperf->opts;

	/*
	 * Use WiredTiger's random number routine: it's lock-free and fairly
	 * good.
	 */
	rval = __wt_random(&thread->rnd);

	/* Use Pareto distribution to give 80/20 hot/cold values. */
	if (opts->pareto != 0) {
#define	PARETO_SHAPE	1.5
		S1 = (-1 / PARETO_SHAPE);
		S2 = wtperf_value_range(wtperf) *
		    (opts->pareto / 100.0) * (PARETO_SHAPE - 1);
		U = 1 - (double)rval / (double)UINT32_MAX;
		rval = (uint64_t)((pow(U, S1) - 1) * S2);
		/*
		 * This Pareto calculation chooses out of range values about
		 * 2% of the time, from my testing. That will lead to the
		 * first item in the table being "hot".
		 */
		if (rval > wtperf_value_range(wtperf))
			rval = 0;
	}
	/*
	 * Wrap the key to within the expected range and avoid zero: we never
	 * insert that key.
	 */
	rval = (rval % wtperf_value_range(wtperf)) + 1;
	return (rval);
}
