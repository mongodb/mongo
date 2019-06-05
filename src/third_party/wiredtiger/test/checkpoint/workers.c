/*-
 * Public Domain 2014-2019 MongoDB, Inc.
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

#include "test_checkpoint.h"

static int real_worker(void);
static WT_THREAD_RET worker(void *);

/*
 * create_table --
 *     Create a WiredTiger table of the configured type for this cookie.
 */
static int
create_table(WT_SESSION *session, COOKIE *cookie)
{
	int ret;
	char config[256];

	/*
	 * If we're using timestamps, turn off logging for the table.
	 */
	if (g.use_timestamps)
		testutil_check(__wt_snprintf(config, sizeof(config),
		    "key_format=%s,value_format=S,allocation_size=512,"	\
		    "leaf_page_max=1KB,internal_page_max=1KB,"		\
		    "memory_page_max=64KB,log=(enabled=false),%s",
		    cookie->type == COL ? "r" : "q",
		    cookie->type == LSM ? ",type=lsm" : ""));
	else
		testutil_check(__wt_snprintf(config, sizeof(config),
		    "key_format=%s,value_format=S,%s",
		    cookie->type == COL ? "r" : "q",
		    cookie->type == LSM ? ",type=lsm" : ""));

	if ((ret = session->create(session, cookie->uri, config)) != 0)
		if (ret != EEXIST)
			return (log_print_err("session.create", ret, 1));
	++g.ntables_created;
	return (0);
}

/*
 * start_workers --
 *     Setup the configuration for the tables being populated, then start
 *     the worker thread(s) and wait for them to finish.
 */
int
start_workers(table_type type)
{
	struct timeval start, stop;
	WT_SESSION *session;
	wt_thread_t *tids;
	double seconds;
	int i, ret;

	ret = 0;

	/* Create statistics and thread structures. */
	if ((tids = calloc((size_t)(g.nworkers), sizeof(*tids))) == NULL)
		return (log_print_err("calloc", errno, 1));

	if ((ret = g.conn->open_session(g.conn, NULL, NULL, &session)) != 0) {
		(void)log_print_err("conn.open_session", ret, 1);
		goto err;
	}
	/* Setup the cookies */
	for (i = 0; i < g.ntables; ++i) {
		g.cookies[i].id = i;
		if (type == MIX)
			g.cookies[i].type =
			    (table_type)((i % MAX_TABLE_TYPE) + 1);
		else
			g.cookies[i].type = type;
		testutil_check(__wt_snprintf(
		    g.cookies[i].uri, sizeof(g.cookies[i].uri),
		    "%s%04d", URI_BASE, g.cookies[i].id));

		/* Should probably be atomic to avoid races. */
		if ((ret = create_table(session, &g.cookies[i])) != 0)
			goto err;
	}

	testutil_check(session->close(session, NULL));

	(void)gettimeofday(&start, NULL);

	/* Create threads. */
	for (i = 0; i < g.nworkers; ++i)
		testutil_check(__wt_thread_create(
		    NULL, &tids[i], worker, &g.cookies[i]));

	/* Wait for the threads. */
	for (i = 0; i < g.nworkers; ++i)
		testutil_check(__wt_thread_join(NULL, &tids[i]));

	(void)gettimeofday(&stop, NULL);
	seconds = (stop.tv_sec - start.tv_sec) +
	    (stop.tv_usec - start.tv_usec) * 1e-6;
	printf("Ran workers for: %f seconds\n", seconds);

err:	free(tids);

	return (ret);
}

/*
 * worker_op --
 *	Write operation.
 */
static inline int
worker_op(WT_CURSOR *cursor, uint64_t keyno, u_int new_val)
{
	int cmp, ret;
	char valuebuf[64];

	cursor->set_key(cursor, keyno);
	/* Roughly half inserts, then balanced inserts / range removes. */
	if (new_val > g.nops / 2 && new_val % 39 == 0) {
		if ((ret = cursor->search_near(cursor, &cmp)) != 0) {
			if (ret == WT_NOTFOUND)
				return (0);
			if (ret == WT_ROLLBACK)
				return (WT_ROLLBACK);
			return (log_print_err("cursor.search_near", ret, 1));
		}
		if (cmp < 0) {
			if ((ret = cursor->next(cursor)) != 0) {
				if (ret == WT_NOTFOUND)
					return (0);
				if (ret == WT_ROLLBACK)
					return (WT_ROLLBACK);
				return (log_print_err("cursor.next", ret, 1));
			}
		}
		for (int i = 10; i > 0; i--) {
			if ((ret = cursor->remove(cursor)) != 0) {
				if (ret == WT_ROLLBACK)
					return (WT_ROLLBACK);
				return (log_print_err("cursor.remove", ret, 1));
			}
			if ((ret = cursor->next(cursor)) != 0) {
				if (ret == WT_NOTFOUND)
					return (0);
				if (ret == WT_ROLLBACK)
					return (WT_ROLLBACK);
				return (log_print_err("cursor.next", ret, 1));
			}
		}
		if (g.sweep_stress)
			testutil_check(cursor->reset(cursor));
	} else if (new_val % 39 < 10) {
		if ((ret = cursor->search(cursor)) != 0 && ret != WT_NOTFOUND) {
			if (ret == WT_ROLLBACK)
				return (WT_ROLLBACK);
			return (log_print_err("cursor.search", ret, 1));
		}
		if (g.sweep_stress)
			testutil_check(cursor->reset(cursor));
	} else {
		testutil_check(__wt_snprintf(
		    valuebuf, sizeof(valuebuf), "%052u", new_val));
		cursor->set_value(cursor, valuebuf);
		if ((ret = cursor->insert(cursor)) != 0) {
			if (ret == WT_ROLLBACK)
				return (WT_ROLLBACK);
			return (log_print_err("cursor.insert", ret, 1));
		}
	}

	return (0);
}

/*
 * worker --
 *	Worker thread start function.
 */
static WT_THREAD_RET
worker(void *arg)
{
	char tid[128];

	WT_UNUSED(arg);

	testutil_check(__wt_thread_str(tid, sizeof(tid)));
	printf("worker thread starting: tid: %s\n", tid);

	(void)real_worker();
	return (WT_THREAD_RET_VALUE);
}

/*
 * real_worker --
 *     A single worker thread that transactionally updates all tables with
 *     consistent values.
 */
static int
real_worker(void)
{
	WT_CURSOR **cursors;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	u_int i, keyno;
	int j, ret, t_ret;
	const char *begin_cfg;
	char buf[128];
	bool has_cursors;

	ret = t_ret = 0;

	if ((cursors = calloc(
	    (size_t)(g.ntables), sizeof(WT_CURSOR *))) == NULL)
		return (log_print_err("malloc", ENOMEM, 1));

	if ((ret = g.conn->open_session(
	    g.conn, NULL, "isolation=snapshot", &session)) != 0) {
		(void)log_print_err("conn.open_session", ret, 1);
		goto err;
	}

	__wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

	for (j = 0; j < g.ntables; j++)
		if ((ret = session->open_cursor(session,
		    g.cookies[j].uri, NULL, NULL, &cursors[j])) != 0) {
			(void)log_print_err("session.open_cursor", ret, 1);
			goto err;
		}
	has_cursors = true;

	if (g.use_timestamps)
		begin_cfg = "read_timestamp=1,roundup_timestamps=(read=true)";
	else
		begin_cfg = NULL;

	for (i = 0; i < g.nops && g.running; ++i, __wt_yield()) {
		if ((ret =
		    session->begin_transaction(session, begin_cfg)) != 0) {
			(void)log_print_err(
			    "real_worker:begin_transaction", ret, 1);
			goto err;
		}
		keyno = __wt_random(&rnd) % g.nkeys + 1;
		if (g.use_timestamps && i % 23 == 0) {
			if (__wt_try_readlock(
			    (WT_SESSION_IMPL *)session, &g.clock_lock) != 0) {
				testutil_check(
				    session->commit_transaction(session, NULL));
				for (j = 0; j < g.ntables; j++)
					testutil_check(
					    cursors[j]->close(cursors[j]));
				has_cursors = false;
				__wt_readlock(
				    (WT_SESSION_IMPL *)session, &g.clock_lock);
				testutil_check(session->begin_transaction(
				    session, begin_cfg));
			}
			testutil_check(__wt_snprintf(
			    buf, sizeof(buf), "commit_timestamp=%x", g.ts + 1));
			testutil_check(
			    session->timestamp_transaction(session, buf));
			__wt_readunlock(
			    (WT_SESSION_IMPL *)session, &g.clock_lock);

			for (j = 0; !has_cursors && j < g.ntables; j++)
				if ((ret = session->open_cursor(
				    session, g.cookies[j].uri,
				    NULL, NULL, &cursors[j])) != 0) {
					(void)log_print_err(
					    "session.open_cursor", ret, 1);
					goto err;
				}
			has_cursors = true;
		}
		for (j = 0; ret == 0 && j < g.ntables; j++) {
			ret = worker_op(cursors[j], keyno, i);
		}
		if (ret != 0 && ret != WT_ROLLBACK) {
			(void)log_print_err("worker op failed", ret, 1);
			goto err;
		} else if (ret == 0 && __wt_random(&rnd) % 7 != 0) {
			if ((ret = session->commit_transaction(
			    session, NULL)) != 0) {
				(void)log_print_err(
				    "real_worker:commit_transaction", ret, 1);
				goto err;
			}
		} else {
			if ((ret = session->rollback_transaction(
			    session, NULL)) != 0) {
				(void)log_print_err(
				    "real_worker:rollback_transaction", ret, 1);
				goto err;
			}
		}
	}

err:	if ((t_ret = session->close(session, NULL)) != 0 && ret == 0) {
		ret = t_ret;
		(void)log_print_err("session.close", ret, 1);
	}
	free(cursors);

	return (ret);
}
