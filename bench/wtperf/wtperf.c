/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

#define	IS_INSERT_WORKER(w)						\
    ((w) == WORKER_INSERT || (w) == WORKER_INSERT_RMW)

/* Default values. */
static const CONFIG default_cfg = {
	"WT_TEST",			/* home */
	NULL,				/* uri */
	NULL,				/* conn */
	NULL,				/* logf */
	NULL, NULL, NULL, NULL, NULL,	/* threads */
	WT_PERF_INIT,			/* phase */
	{0, 0},				/* phase_start_time */

#define	OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT
};

static const char * const small_config_str =
    "conn_config=\"cache_size=500MB\","
    "table_config=\"lsm_chunk_size=5MB\","
    "icount=500000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=20,"
    "populate_threads=1,"
    "read_threads=8,";

static const char * const med_config_str =
    "conn_config=\"cache_size=1GB\","
    "table_config=\"lsm_chunk_size=20MB\","
    "icount=50000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=100,"
    "populate_threads=1,"
    "read_threads=16,";

static const char * const large_config_str =
    "conn_config=\"cache_size=2GB\","
    "table_config=\"lsm_chunk_size=50MB\","
    "icount=500000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=600,"
    "populate_threads=1,"
    "read_threads=16,";

static const char * const debug_cconfig = "verbose=[lsm]";
static const char * const debug_tconfig = "";

static uint64_t g_ckpt_ops;		/* checkpoint operations */
static uint64_t g_insert_ops;		/* insert operations */
static uint64_t g_read_ops;		/* read operations */
static uint64_t g_update_ops;		/* update operations */

static uint64_t g_insert_key;		/* insert key */

static int g_running;			/* threads are running */
static int g_threads_quit;		/* threads that exited early */
static int g_util_running;		/* utility threads are running */

/*
 * Atomic update where needed.
 */
#if defined(_lint)
#define	ATOMIC_ADD(v, val)	((v) += (val), (v))
#else
#define	ATOMIC_ADD(v, val)	__sync_add_and_fetch(&(v), val)
#endif

static void	*checkpoint_worker(void *);
static int	 execute_populate(CONFIG *);
static int	 execute_workload(CONFIG *);
static int	 find_table_count(CONFIG *);
static void	*insert_thread(void *);
static void	*populate_thread(void *);
static void	*read_thread(void *);
static int	 start_threads(
		    CONFIG *, u_int, CONFIG_THREAD **, void *(*func)(void *));
static void	*stat_worker(void *);
static int	 stop_threads(CONFIG *, u_int, CONFIG_THREAD **);
static void	*update_thread(void *);
static void	 worker(CONFIG_THREAD *, worker_type);
static uint64_t	 wtperf_rand(CONFIG *);
static uint64_t	 wtperf_value_range(CONFIG *);

/* Retrieve an ID for the next insert operation. */
static inline uint64_t
get_next_incr(void)
{
	return (ATOMIC_ADD(g_insert_key, 1));
}

/*
 * Return total ops for a group of threads.
 */
#define	WTPERF_SUM_OPS(field)						\
static inline uint64_t							\
sum_##field##_ops(CONFIG_THREAD *threads, u_int num)			\
{									\
	uint64_t total;							\
	u_int i;							\
									\
	if (threads == NULL)						\
		return (0);						\
									\
	for (i = 0, total = 0; i < num; ++i, ++threads)			\
		total += threads->field##_ops;				\
	return (total);							\
}
WTPERF_SUM_OPS(ckpt)
WTPERF_SUM_OPS(insert)
WTPERF_SUM_OPS(read)
WTPERF_SUM_OPS(update)

static void
worker(CONFIG_THREAD *thread, worker_type wtype)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t next_val;
	int op_ret, ret;
	char *data_buf, *key_buf, *value;
	const char *op_name;

	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;
	data_buf = key_buf = NULL;
	op_ret = 0;

	if ((key_buf = calloc(cfg->key_sz + 1, 1)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	if (IS_INSERT_WORKER(wtype) || wtype == WORKER_UPDATE) {
		if ((data_buf = calloc(cfg->data_sz, 1)) == NULL) {
			ret = enomem(cfg);
			goto err;
		}
		memset(data_buf, 'a', cfg->data_sz - 1);
	}

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "worker: WT_CONNECTION.open_session");
		goto err;
	}
	if ((ret = session->open_cursor(
	    session, cfg->uri, NULL, NULL, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "worker: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	while (g_running) {
		if (cfg->random_range == 0 && IS_INSERT_WORKER(wtype))
			next_val = cfg->icount + get_next_incr();
		else
			next_val = wtperf_rand(cfg);

		/*
		 * If the workload is started without a populate phase we
		 * rely on at least one insert to get a valid item id.
		 */
		if (!IS_INSERT_WORKER(wtype) &&
		    wtperf_value_range(cfg) < next_val)
			continue;
		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, next_val);
		cursor->set_key(cursor, key_buf);
		switch (wtype) {
		case WORKER_READ:
			if ((op_ret = cursor->search(cursor)) == 0) {
				++thread->read_ops;
				continue;
			}
			op_name = "read";
			break;
		case WORKER_INSERT_RMW:
			if ((op_ret = cursor->search(cursor)) != WT_NOTFOUND) {
				op_name="insert_rmw";
				break;
			}
			/* All error returns reset the cursor buffers. */
			cursor->set_key(cursor, key_buf);
			/* FALLTHROUGH */
		case WORKER_INSERT:
			cursor->set_value(cursor, data_buf);
			if ((op_ret = cursor->insert(cursor)) == 0) {
				++thread->insert_ops;
				continue;
			}
			op_name = "insert";
			break;
		case WORKER_UPDATE:
			if ((op_ret = cursor->search(cursor)) == 0) {
				assert(cursor->get_value(cursor, &value) == 0);
				memcpy(data_buf, value, cfg->data_sz);
				if (data_buf[0] == 'a')
					data_buf[0] = 'b';
				else
					data_buf[0] = 'a';
				cursor->set_value(cursor, data_buf);
				if ((op_ret = cursor->update(cursor)) == 0) {
					++thread->update_ops;
					continue;
				}
			}
			op_name = "update";
			break;
		default:
			lprintf(cfg, EINVAL, 0, "Invalid worker type");
			goto err;
		}

		/* Report errors and continue. */
		if (op_ret == WT_NOTFOUND && cfg->random_range != 0)
			continue;

		/*
		 * Emit a warning instead of an error if there are insert
		 * threads, and the failed op was for an old item.
		 */
		if (op_ret == WT_NOTFOUND && cfg->insert_threads > 0 &&
		    (wtype == WORKER_READ || wtype == WORKER_UPDATE)) {
			if (next_val < cfg->icount)
				lprintf(cfg, WT_PANIC, 0,
				    "%s not found for: %s. Value was"
				    " inserted during populate",
				    op_name, key_buf);
			if (next_val * 1.1 > wtperf_value_range(cfg))
				continue;
			lprintf(cfg, op_ret, 1,
			    "%s not found for: %s, range: %" PRIu64
			    " an insert is likely more than ten "
			    "percent behind reads",
			    op_name, key_buf, wtperf_value_range(cfg));
		} else
			lprintf(cfg, op_ret, 0,
			    "%s failed for: %s, range: %"PRIu64,
			    op_name, key_buf, wtperf_value_range(cfg));
	}

	/* To ensure managing thread knows if we exited early. */
err:	if (ret != 0)
		++g_threads_quit;
	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	free(data_buf);
	free(key_buf);
}

static void *
read_thread(void *arg)
{
	worker((CONFIG_THREAD *)arg, WORKER_READ);
	return (NULL);
}

static void *
insert_thread(void *arg)
{
	CONFIG *cfg;

	cfg = ((CONFIG_THREAD *)arg)->cfg;
	worker((CONFIG_THREAD *)arg,
	    cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT);
	return (NULL);
}

static void *
update_thread(void *arg)
{
	worker((CONFIG_THREAD *)arg, WORKER_UPDATE);
	return (NULL);
}

static void *
populate_thread(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint32_t opcount;
	uint64_t op;
	int intxn, ret;
	char *data_buf, *key_buf;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;
	data_buf = key_buf = NULL;
	ret = 0;

	if ((key_buf = calloc(cfg->key_sz + 1, 1)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	if ((data_buf = calloc(cfg->data_sz, 1)) == NULL) {
		ret = enomem(cfg);
		goto err;
	}
	memset(data_buf, 'a', cfg->data_sz - 1);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "populate: WT_CONNECTION.open_session");
		goto err;
	}
	/* Do a bulk load if populate is single-threaded. */
	if ((ret = session->open_cursor(session, cfg->uri, NULL,
	    cfg->populate_threads == 1 ? "bulk" : NULL, &cursor)) != 0) {
		lprintf(cfg,
		    ret, 0, "populate: WT_SESSION.open_cursor: %s", cfg->uri);
		goto err;
	}

	/* Populate the database. */
	if (cfg->populate_ops_per_txn == 0)
		for (;;) {
			op = get_next_incr();
			if (op > cfg->icount)
				break;

			sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
			cursor->set_key(cursor, key_buf);
			cursor->set_value(cursor, data_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
			++thread->insert_ops;
		}
	else {
		for (intxn = 0, opcount = 0;;) {
			op = get_next_incr();
			if (op > cfg->icount)
				break;

			if (!intxn) {
				assert(session->begin_transaction(
				    session, cfg->transaction_config) == 0);
				intxn = 1;
			}
			sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
			cursor->set_key(cursor, key_buf);
			cursor->set_value(cursor, data_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
			++thread->insert_ops;

			if (++opcount < cfg->populate_ops_per_txn)
				continue;
			opcount = 0;

			if ((ret =
			    session->commit_transaction(session, NULL)) != 0)
				lprintf(cfg, ret, 0,
				    "Fail committing, transaction was aborted");
			intxn = 0;
		}
		if (intxn &&
		    (ret = session->commit_transaction(session, NULL)) != 0)
			lprintf(cfg, ret, 0,
			    "Fail committing, transaction was aborted");
	}

	/* To ensure managing thread knows if we exited early. */
err:	if (ret != 0)
		++g_threads_quit;
	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	free(data_buf);
	free(key_buf);
	return (NULL);
}

static void *
stat_worker(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	struct timeval e;
	double secs;
	size_t uri_len;
	uint64_t value;
	uint32_t i;
	int ret;
	const char *desc, *pvalue;
	char *stat_uri;

	session = NULL;
	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	stat_uri = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in statistics thread.");
		goto err;
	}

	uri_len = strlen("statistics:") + strlen(cfg->uri) + 1;
	if ((stat_uri = malloc(uri_len)) == NULL) {
		(void)enomem(cfg);
		goto err;
	}
	(void)snprintf(stat_uri, uri_len, "statistics:%s", cfg->uri);

	while (g_util_running) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < cfg->stat_interval; i++) {
			sleep(1);
			if (!g_util_running)
				break;
		}

		/* Generic header. */
		lprintf(cfg, 0, cfg->verbose,
		    "=======================================");
		assert(gettimeofday(&e, NULL) == 0);
		secs = e.tv_sec + e.tv_usec / 1000000.0;
		secs -= (cfg->phase_start_time.tv_sec +
		    cfg->phase_start_time.tv_usec / 1000000.0);
		if (secs == 0)
			++secs;

		switch (cfg->phase) {
		case WT_PERF_POPULATE:
			g_insert_ops = sum_insert_ops(
			    cfg->popthreads, cfg->populate_threads);
			lprintf(cfg, 0, cfg->verbose,
			    "inserts: %" PRIu64 ", elapsed time: %.2f",
			    g_insert_ops, secs);
			break;
		case WT_PERF_WORKER:
			g_ckpt_ops = sum_ckpt_ops(
			    cfg->ckptthreads, cfg->checkpoint_threads);
			g_insert_ops =
			    sum_insert_ops(cfg->ithreads, cfg->insert_threads);
			g_read_ops =
			    sum_read_ops(cfg->rthreads, cfg->read_threads);
			g_update_ops =
			    sum_update_ops(cfg->uthreads, cfg->update_threads);
			lprintf(cfg, 0, cfg->verbose,
			    "reads: %" PRIu64 " inserts: %" PRIu64
			    " updates: %" PRIu64 ", checkpoints: %" PRIu64
			    ", elapsed time: %.2f",
			    g_read_ops,
			    g_insert_ops, g_update_ops, g_ckpt_ops, secs);
			break;
		case WT_PERF_INIT:
		default:
			break;
		}

		/* Report data-source statistics. */
		if ((ret = session->open_cursor(session, stat_uri,
		    NULL, "statistics=(clear)", &cursor)) != 0) {
			/*
			 * It is possible the data source is exclusively
			 * locked at this moment.  Ignore it and try again.
			 */
			if (ret == EBUSY)
				continue;
			lprintf(cfg, ret, 0,
			    "open_cursor failed for data source statistics");
			goto err;
		}
		while ((ret = cursor->next(cursor)) == 0) {
			assert(cursor->get_value(
			    cursor, &desc, &pvalue, &value) == 0);
			if (value != 0)
				lprintf(cfg, 0, cfg->verbose,
				    "stat:table: %s=%s", desc, pvalue);
		}
		assert(ret == WT_NOTFOUND);
		assert(cursor->close(cursor) == 0);
		lprintf(cfg, 0, cfg->verbose, "-----------------");

		/* Report connection statistics. */
		if ((ret = session->open_cursor(session, "statistics:",
		    NULL, "statistics=(clear)", &cursor)) != 0) {
			lprintf(cfg, ret, 0,
			    "open_cursor failed in statistics");
			goto err;
		}
		while ((ret = cursor->next(cursor)) == 0) {
			assert(cursor->get_value(
			    cursor, &desc, &pvalue, &value) == 0);
			if (value != 0)
				lprintf(cfg, 0, cfg->verbose,
				    "stat:conn: %s=%s", desc, pvalue);
		}
		assert(ret == WT_NOTFOUND);
		assert(cursor->close(cursor) == 0);
	}
err:	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	free(stat_uri);
	return (arg);
}

static void *
checkpoint_worker(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *thread;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timeval e, s;
	uint64_t ms;
	uint32_t i;
	int ret;

	thread = (CONFIG_THREAD *)arg;
	cfg = thread->cfg;
	conn = cfg->conn;
	session = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in checkpoint thread.");
		goto err;
	}

	while (g_util_running) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0;
		    i < cfg->checkpoint_interval && g_util_running; i++)
			sleep(1);
		if (!g_util_running)
			break;

		assert(gettimeofday(&s, NULL) == 0);
		if ((ret = session->checkpoint(session, NULL)) != 0) {
			/* Report errors and continue. */
			lprintf(cfg, ret, 0, "Checkpoint failed.");
			continue;
		}
		++thread->ckpt_ops;

		assert(gettimeofday(&e, NULL) == 0);
		ms = (e.tv_sec * 1000) + (e.tv_usec / 1000.0);
		ms -= (s.tv_sec * 1000) + (s.tv_usec / 1000.0);
	}

err:	if (session != NULL)
		assert(session->close(session, NULL) == 0);

	return (NULL);
}

static int
execute_populate(CONFIG *cfg)
{
	struct timeval e;
	double secs;
	uint64_t last_ops;
	uint32_t interval;
	u_int sleepsec;
	int elapsed, ret;

	cfg->phase = WT_PERF_POPULATE;
	lprintf(cfg, 0, 1, "Starting populate threads");

	g_insert_key = 0;
	g_running = 1;
	g_threads_quit = 0;
	if ((ret = start_threads(cfg,
	    cfg->populate_threads, &cfg->popthreads, populate_thread)) != 0)
		return (ret);

	assert(gettimeofday(&cfg->phase_start_time, NULL) == 0);
	for (elapsed = 0, interval = 0, last_ops = 0;
	    g_insert_key < cfg->icount && g_threads_quit == 0;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, each 100th increment of elapsed is a single
		 * increment of interval.
		 */
		(void)usleep(10000);
		if (cfg->report_interval == 0 || ++elapsed < 100)
			continue;
		elapsed = 0;
		if (++interval < cfg->report_interval)
			continue;
		interval = 0;
		g_insert_ops =
		    sum_insert_ops(cfg->popthreads, cfg->populate_threads);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " populate inserts in %" PRIu32 " secs",
		    g_insert_ops - last_ops, cfg->report_interval);
		last_ops = g_insert_ops;
	}
	assert(gettimeofday(&e, NULL) == 0);

	g_running = 0;
	if ((ret =
	    stop_threads(cfg, cfg->populate_threads, &cfg->popthreads)) != 0)
		return (ret);

	/* Report if any worker threads didn't finish. */
	if (g_threads_quit != 0) {
		lprintf(cfg, WT_ERROR, 0,
		    "Populate thread(s) exited without finishing.");
		return (WT_ERROR);
	}

	lprintf(cfg, 0, 1, "Finished load of %" PRIu32 " items", cfg->icount);
	secs = e.tv_sec + e.tv_usec / 1000000.0;
	secs -= cfg->phase_start_time.tv_sec +
	    cfg->phase_start_time.tv_usec / 1000000.0;
	if (secs == 0)
		++secs;
	lprintf(cfg, 0, 1,
	    "Load time: %.2f\n" "load ops/sec: %.2f", secs, cfg->icount / secs);

	/*
	 * If configured, sleep for awhile to allow LSM merging to complete in
	 * the background.  If user specifies -1, then sleep for as long as it
	 * took to load.
	 */
	if (cfg->merge_sleep) {
		if (cfg->merge_sleep < 0)
			sleepsec =
			    (u_int)(e.tv_sec - cfg->phase_start_time.tv_sec);
		else
			sleepsec = (u_int)cfg->merge_sleep;
		lprintf(cfg, 0, 1, "Sleep %d seconds for merging", sleepsec);
		(void)sleep(sleepsec);
	}
	return (0);
}

static int
execute_workload(CONFIG *cfg)
{
	uint64_t last_ckpts, last_inserts, last_reads, last_updates;
	uint32_t interval, run_time;
	int ret, tret;

	lprintf(cfg, 0, 1, "Starting worker threads");
	cfg->phase = WT_PERF_WORKER;

	last_ckpts = last_inserts = last_reads = last_updates = 0;
	ret = 0;

	lprintf(cfg, 0, 1,
	    "Starting workload threads: read %" PRIu32
	    ", insert %" PRIu32 ", update %" PRIu32,
	    cfg->read_threads, cfg->insert_threads, cfg->update_threads);

	g_insert_key = 0;
	g_insert_ops = g_read_ops = g_update_ops = 0;
	g_running = 1;
	g_threads_quit = 0;

	if (cfg->read_threads != 0 && (tret = start_threads(
	    cfg, cfg->read_threads, &cfg->rthreads, read_thread)) != 0 &&
	    ret == 0) {
		ret = tret;
		goto err;
	}
	if (cfg->insert_threads != 0 && (tret = start_threads(
	    cfg, cfg->insert_threads, &cfg->ithreads, insert_thread)) != 0 &&
	    ret == 0) {
		ret = tret;
		goto err;
	}
	if (cfg->update_threads != 0 && (tret = start_threads(
	    cfg, cfg->update_threads, &cfg->uthreads, update_thread)) != 0 &&
	    ret == 0) {
		ret = tret;
		goto err;
	}

	assert(gettimeofday(&cfg->phase_start_time, NULL) == 0);
	for (run_time = cfg->run_time; g_threads_quit == 0;) {
		if (cfg->report_interval == 0 ||
		    run_time < cfg->report_interval)
			interval = run_time;
		else
			interval = cfg->report_interval;
		(void)sleep(interval);

		run_time -= interval;
		if (run_time == 0)
			break;

		g_ckpt_ops =
		    sum_ckpt_ops(cfg->ckptthreads, cfg->checkpoint_threads);
		g_insert_ops =
		    sum_insert_ops(cfg->ithreads, cfg->insert_threads);
		g_read_ops = sum_read_ops(cfg->rthreads, cfg->read_threads);
		g_update_ops =
		    sum_update_ops(cfg->uthreads, cfg->update_threads);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates, %" PRIu64 " checkpoints in %" PRIu32 " secs",
		    g_read_ops - last_reads,
		    g_insert_ops - last_inserts,
		    g_update_ops - last_updates,
		    g_ckpt_ops - last_ckpts,
		    cfg->report_interval);
		last_reads = g_read_ops;
		last_inserts = g_insert_ops;
		last_updates = g_update_ops;
		last_ckpts = g_ckpt_ops;
	}

	/* One final summation of the operations we've completed. */
	g_ckpt_ops = sum_ckpt_ops(cfg->ckptthreads, cfg->checkpoint_threads);
	g_insert_ops = sum_insert_ops(cfg->ithreads, cfg->insert_threads);
	g_read_ops = sum_read_ops(cfg->rthreads, cfg->read_threads);
	g_update_ops = sum_update_ops(cfg->uthreads, cfg->update_threads);

err:	g_running = 0;
	if (cfg->read_threads != 0 && (tret =
	    stop_threads(cfg, cfg->read_threads, &cfg->rthreads)) != 0 &&
	    ret == 0)
		ret = tret;
	if (cfg->insert_threads != 0 && (tret =
	    stop_threads(cfg, cfg->insert_threads, &cfg->ithreads)) != 0 &&
	    ret == 0)
		ret = tret;

	if (cfg->update_threads != 0 && (tret =
	    stop_threads(cfg, cfg->update_threads, &cfg->uthreads)) != 0 &&
	    ret == 0)
		ret = tret;

	/* Report if any worker threads didn't finish. */
	if (g_threads_quit != 0) {
		lprintf(cfg, WT_ERROR, 0,
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
find_table_count(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *key;
	int ret;

	conn = cfg->conn;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed finding existing table count");
		goto err;
	}
	if ((ret = session->open_cursor(session, cfg->uri,
	    NULL, NULL, &cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_cursor failed finding existing table count");
		goto err;
	}
	if ((ret = cursor->prev(cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "cursor prev failed finding existing table count");
		goto err;
	}
	assert(cursor->get_key(cursor, &key) == 0);
	cfg->icount = (uint32_t)atoi(key);

err:	assert(session->close(session, NULL) == 0);
	return (ret);
}

int
main(int argc, char *argv[])
{
	CONFIG cfg;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	pthread_t stat_thread;
	size_t len;
	uint64_t req_len;
	int ch, ret, stat_created, tret;
	const char *opts = "C:O:T:h:o:SML";
	const char *wtperftmp_subdir = "wtperftmp";
	const char *user_cconfig, *user_tconfig;
	char *cmd, *cc_buf, *tc_buf, *tmphome;

	conn = NULL;
	session = NULL;
	ret = stat_created = 0;
	user_cconfig = user_tconfig = NULL;
	cmd = cc_buf = tc_buf = tmphome = NULL;

	/* Setup the default configuration values. */
	memset(&cfg, 0, sizeof(cfg));
	if (config_assign(&cfg, &default_cfg))
		goto err;

	/* Do a basic validation of options, and home is needed before open. */
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'h':
			cfg.home = optarg;
			break;
		case '?':
			fprintf(stderr, "Invalid option\n");
			usage();
			goto einval;
		}

	/*
	 * Create a temporary directory underneath the test directory in which
	 * we do an initial WiredTiger open, because we need a connection and
	 * session in order to use the extension configuration parser.  We will
	 * open the real WiredTiger database after parsing the options.
	 */
	len = strlen(cfg.home) + strlen(wtperftmp_subdir) + 2;
	if ((tmphome = malloc(len)) == NULL) {
		ret = enomem(&cfg);
		goto err;
	}
	snprintf(tmphome, len, "%s/%s", cfg.home, wtperftmp_subdir);
	len = len * 2 + 100;
	if ((cmd = malloc(len)) == NULL) {
		ret = enomem(&cfg);
		goto err;
	}
	snprintf(cmd, len, "rm -rf %s && mkdir %s", tmphome, tmphome);
	if (system(cmd) != 0) {
		fprintf(stderr, "%s: failed\n", cmd);
		goto einval;
	}
	if ((ret = wiredtiger_open(tmphome, NULL, "create", &conn)) != 0) {
		lprintf(&cfg, ret, 0, "wiredtiger_open: %s", tmphome);
		goto err;
	}
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(&cfg, ret, 0, "Error creating session");
		goto err;
	}

	/*
	 * Then parse different config structures - other options override
	 * fields within the structure.
	 */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'S':
			if (config_opt_line(
			    &cfg, session, small_config_str) != 0)
				goto einval;
			break;
		case 'M':
			if (config_opt_line(
			    &cfg, session, med_config_str) != 0)
				goto einval;
			break;
		case 'L':
			if (config_opt_line(
			    &cfg, session, large_config_str) != 0)
				goto einval;
			break;
		case 'O':
			if (config_opt_file(
			    &cfg, session, optarg) != 0)
				goto einval;
			break;
		default:
			/* Validation done previously. */
			break;
		}

	/* Parse other options */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'o':
			/* Allow -o key=value */
			if (config_opt_line(&cfg, session, optarg) != 0)
				goto einval;
			break;
		case 'C':
			user_cconfig = optarg;
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		}

	/* Build the URI from the table name. */
	req_len = strlen("table:") + strlen(cfg.table_name) + 1;
	if ((cfg.uri = calloc(req_len, 1)) == NULL) {
		ret = enomem(&cfg);
		goto err;
	}
	snprintf(cfg.uri, req_len, "table:%s", cfg.table_name);
	
	if ((ret = setup_log_file(&cfg)) != 0)
		goto err;

	/* Make stdout line buffered, so verbose output appears quickly. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

	/* Concatenate non-default configuration strings. */
	if (cfg.verbose > 1 || user_cconfig != NULL) {
		req_len = strlen(cfg.conn_config) + strlen(debug_cconfig) + 3;
		if (user_cconfig != NULL)
			req_len += strlen(user_cconfig);
		if ((cc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(&cfg);
			goto err;
		}
		snprintf(cc_buf, req_len, "%s%s%s%s%s",
		    cfg.conn_config,
		    cfg.verbose > 1 ? "," : "",
		    cfg.verbose > 1 ? debug_cconfig : "",
		    user_cconfig ? "," : "", user_cconfig ? user_cconfig : "");
		if ((ret = config_opt_str(
		    &cfg, session, "conn_config", cc_buf)) != 0)
			goto err;
	}
	if (cfg.verbose > 1 || user_tconfig != NULL) {
		req_len = strlen(cfg.table_config) + strlen(debug_tconfig) + 3;
		if (user_tconfig != NULL)
			req_len += strlen(user_tconfig);
		if ((tc_buf = calloc(req_len, 1)) == NULL) {
			ret = enomem(&cfg);
			goto err;
		}
		snprintf(tc_buf, req_len, "%s%s%s%s%s",
		    cfg.table_config,
		    cfg.verbose > 1 ? "," : "",
		    cfg.verbose > 1 ? debug_tconfig : "",
		    user_tconfig ? "," : "", user_tconfig ? user_tconfig : "");
		if ((ret = config_opt_str(
		    &cfg, session, "table_config", tc_buf)) != 0)
			goto err;
	}

	ret = session->close(session, NULL);
	session = NULL;
	if (ret != 0) {
		lprintf(&cfg, ret, 0, "WT_SESSION.close");
		goto err;
	}
	ret = conn->close(conn, NULL);
	conn = NULL;
	if (ret != 0) {
		lprintf(&cfg, ret, 0, "WT_CONNECTION.close: %s", tmphome);
		goto err;
	}

	/* Sanity check reporting interval. */
	if (cfg.run_time > 0 && cfg.report_interval > cfg.run_time) {
		fprintf(stderr, "report-interval larger than the run-time.n");
		ret = EINVAL;
		goto err;
	}

	if (cfg.verbose > 1)		/* Display the configuration. */
		print_config(&cfg);

					/* Open the real connection. */
	if ((ret = wiredtiger_open(
	    cfg.home, NULL, cfg.conn_config, &conn)) != 0) {
		lprintf(&cfg, ret, 0, "Error connecting to %s", cfg.home);
		goto err;
	}
	cfg.conn = conn;
					/* Create the table. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(&cfg, ret, 0,
		    "Error opening a session on %s", cfg.home);
		goto err;
	}
	if ((ret = session->create(
	    session, cfg.uri, cfg.table_config)) != 0) {
		lprintf(&cfg, ret, 0, "Error creating table %s", cfg.uri);
		goto err;
	}
	assert(session->close(session, NULL) == 0);
	session = NULL;

	g_util_running = 1;		/* Start the statistics thread. */
	if (cfg.stat_interval != 0) {
		if ((ret = pthread_create(
		    &stat_thread, NULL, stat_worker, &cfg)) != 0) {
			lprintf(
			    &cfg, ret, 0, "Error creating statistics thread.");
			goto err;
		}
		stat_created = 1;
	}
					/* If creating, populate the table. */
	if (cfg.create != 0 && execute_populate(&cfg) != 0)
		goto err;
					/* Not creating, set insert count. */
	if (cfg.create == 0 && find_table_count(&cfg) != 0)
		goto err;
					/* Start the checkpoint thread. */
	if (cfg.checkpoint_threads != 0 &&
	    start_threads(&cfg,
	    cfg.checkpoint_threads, &cfg.ckptthreads, checkpoint_worker) != 0)
		goto err;
					/* Execute the workload. */
	if (cfg.run_time != 0 &&
	    cfg.read_threads + cfg.insert_threads + cfg.update_threads != 0 &&
	    (ret = execute_workload(&cfg)) != 0)
		goto err;

	lprintf(&cfg, 0, 1,
	    "Run completed: %" PRIu32 " read threads, %"
	    PRIu32 " insert threads, %" PRIu32 " update threads and %"
	    PRIu32 " checkpoint threads for %"
	    PRIu32 " seconds.",
	    cfg.read_threads, cfg.insert_threads,
	    cfg.update_threads, cfg.checkpoint_threads, cfg.run_time);

	if (cfg.read_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " read operations", g_read_ops);
	if (cfg.insert_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " insert operations", g_insert_ops);
	if (cfg.update_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " update operations", g_update_ops);
	if (cfg.checkpoint_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " checkpoint operations", g_ckpt_ops);

	if (0) {
einval:		ret = EINVAL;
	}
err:	g_util_running = 0;

	if (cfg.checkpoint_threads != 0 &&
	    (tret = stop_threads(&cfg, 1, &cfg.ckptthreads)) != 0)
		if (ret == 0)
			ret = tret;

	if (stat_created != 0 &&
	    (tret = pthread_join(stat_thread, NULL)) != 0) {
		lprintf(&cfg, ret, 0, "Error joining stat thread.");
		if (ret == 0)
			ret = tret;
	}

	if (conn != NULL && (tret = conn->close(conn, NULL)) != 0) {
		lprintf(&cfg, ret, 0,
		    "Error closing connection to %s", cfg.home);
		if (ret == 0)
			ret = tret;
	}

	if (cfg.logf != NULL) {
		assert(fflush(cfg.logf) == 0);
		assert(fclose(cfg.logf) == 0);
	}
	config_free(&cfg);

	free(cc_buf);
	free(cmd);
	free(tc_buf);
	free(tmphome);

	return (ret);
}

static int
start_threads(
    CONFIG *cfg, u_int num, CONFIG_THREAD **threadsp, void *(*func)(void *))
{
	CONFIG_THREAD *threads;
	u_int i;
	int ret;

	if ((*threadsp = calloc(num, sizeof(CONFIG_THREAD))) == NULL)
		return (enomem(cfg));

	for (i = 0, threads = *threadsp; i < num; ++i, ++threads) {
		threads->cfg = cfg;

		if ((ret = pthread_create(
		    &threads->handle, NULL, func, threads)) != 0) {
			lprintf(cfg, ret, 0, "Error creating thread");
			return (ret);
		}
	}
	return (0);
}

static int
stop_threads(CONFIG *cfg, u_int num, CONFIG_THREAD **threadsp)
{
	CONFIG_THREAD *threads;
	u_int i;
	int ret;

	if ((threads = *threadsp) == NULL)
		return (0);

	for (i = 0; i < num; ++i, ++threads)
		if ((ret = pthread_join(threads->handle, NULL)) != 0) {
			lprintf(cfg, ret, 0, "Error joining thread");
			return (ret);
		}

	free(*threadsp);
	*threadsp = NULL;

	return (0);
}

static uint64_t
wtperf_value_range(CONFIG *cfg)
{
	if (cfg->random_range == 0)
		return (cfg->icount + g_insert_key - (cfg->insert_threads + 1));
	else
		return (cfg->icount + cfg->random_range);
}

extern uint32_t __wt_random(void);

static uint64_t
wtperf_rand(CONFIG *cfg)
{
	double S1, S2, U;
	uint64_t rval;

	/*
	 * Use WiredTiger's random number routine: it's lock-free and fairly
	 * good.
	 */
	rval = (uint64_t)__wt_random();

	/* Use Pareto distribution to give 80/20 hot/cold values. */
	if (cfg->pareto) {
#define	PARETO_SHAPE	1.5
		S1 = (-1 / PARETO_SHAPE);
		S2 = wtperf_value_range(cfg) * 0.2 * (PARETO_SHAPE - 1);
		U = 1 - (double)rval / (double)RAND_MAX;
		rval = (pow(U, S1) - 1) * S2;
		/*
		 * This Pareto calculation chooses out of range values about
		 * about 2% of the time, from my testing. That will lead to the
		 * last item in the table being "hot".
		 */
		if (rval > wtperf_value_range(cfg))
			rval = wtperf_value_range(cfg);
	}
	/* Avoid zero - LSM doesn't like it. */
	rval = (rval % wtperf_value_range(cfg)) + 1;
	return (rval);
}
