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
#include <sys/time.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include <gcc.h>				/* WiredTiger internal */

typedef struct {		/* Per-thread structure */
	void *cfg;		/* Enclosing handle */

	pthread_t  handle;	/* Handle */
	uint64_t   read_ops;	/* Read ops */
	uint64_t   update_ops;	/* Update ops */
} CONFIG_THREAD;

typedef struct {
	const char *home;	/* WiredTiger home */
	char *uri;		/* Object URI */

	WT_CONNECTION *conn;	/* Database connection */

	FILE *logf;		/* Logging handle */

	CONFIG_THREAD *rthreads, *ithreads, *popthreads, *uthreads;

	enum { WT_PERF_INIT, WT_PERF_POPULATE, WT_PERF_WORKER } phase;

	struct timeval phase_start_time;

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define	OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT

} CONFIG;

typedef enum {
	BOOL_TYPE, CONFIG_STRING_TYPE, INT_TYPE, STRING_TYPE, UINT32_TYPE
} CONFIG_OPT_TYPE;

typedef struct {
	const char *name;
	const char *description;
	const char *defaultval;
	CONFIG_OPT_TYPE type;
	size_t offset;
} CONFIG_OPT;

/* All options changeable on command line using -o or -O are listed here. */
CONFIG_OPT config_opts[] = {

#define	OPT_DEFINE_DESC
#include "wtperf_opt.i"
#undef OPT_DEFINE_DESC

};

/* Worker thread types. */
typedef enum {
    WORKER_READ, WORKER_INSERT, WORKER_INSERT_RMW, WORKER_UPDATE } worker_type;
#define	IS_INSERT_WORKER(w)						\
    ((w) == WORKER_INSERT || (w) == WORKER_INSERT_RMW)

/* Forward function definitions. */
void *checkpoint_worker(void *);
int config_assign(CONFIG *, const CONFIG *);
void config_free(CONFIG *);
int config_opt(CONFIG *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *);
int config_opt_file(CONFIG *, WT_SESSION *, const char *);
int config_opt_int(CONFIG *, WT_SESSION *, const char *, const char *);
int config_opt_line(CONFIG *, WT_SESSION *, const char *);
int config_opt_str(CONFIG *, WT_SESSION *, const char *, const char *);
void config_opt_usage(void);
int execute_populate(CONFIG *);
int execute_workload(CONFIG *);
int find_table_count(CONFIG *);
void indent_lines(const char *, const char *);
void *insert_thread(void *);
void lprintf(const CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
    WT_GCC_ATTRIBUTE((format (printf, 4, 5)));
void *populate_thread(void *);
void print_config(CONFIG *);
void *read_thread(void *);
int setup_log_file(CONFIG *);
int start_threads(CONFIG *, u_int, CONFIG_THREAD **, void *(*func)(void *));
void *stat_worker(void *);
int stop_threads(CONFIG *, u_int, CONFIG_THREAD **);
void *update_thread(void *);
void usage(void);
void worker(CONFIG_THREAD *, worker_type);
uint64_t wtperf_rand(CONFIG *);
uint64_t wtperf_value_range(CONFIG *);

#define	DEFAULT_LSM_CONFIG						\
	"key_format=S,value_format=S,type=lsm,exclusive=true,"		\
	"leaf_page_max=4kb,internal_page_max=64kb,allocation_size=4kb,"

/* Default values. */
CONFIG default_cfg = {
	"WT_TEST",		/* home */
	NULL,			/* uri */
	NULL,			/* conn */
	NULL,			/* logf */
	NULL, NULL, NULL, NULL,	/* threads */
	WT_PERF_INIT,		/* phase */
	{0, 0},			/* phase_start_time */

#define	OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT

};

const char *small_config_str =
    "conn_config=\"cache_size=500MB\","
    "table_config=\"lsm_chunk_size=5MB\","
    "icount=500000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=20,"
    "populate_threads=1,"
    "read_threads=8,";

const char *med_config_str =
    "conn_config=\"cache_size=1GB\","
    "table_config=\"lsm_chunk_size=20MB\","
    "icount=50000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=100,"
    "populate_threads=1,"
    "read_threads=16,";

const char *large_config_str =
    "conn_config=\"cache_size=2GB\","
    "table_config=\"lsm_chunk_size=50MB\","
    "icount=500000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=600,"
    "populate_threads=1,"
    "read_threads=16,";


const char *debug_cconfig = "verbose=[lsm]";
const char *debug_tconfig = "";

uint64_t g_nins_ops;		/* insert count and key assignment */
uint64_t g_npop_ops;		/* population count and key assignment */
uint64_t g_nread_ops;		/* read operations */
uint64_t g_nupdate_ops;		/* update operations */
uint32_t g_threads_quit; 	/* threads that exited early */

int g_running;			/* threads are running */
int g_util_running;		/* utility threads are running */

/*
 * Atomic update where needed.
 */
#if defined(_lint)
#define	ATOMIC_ADD(v, val)	((v) += (val), (v))
#else
#define	ATOMIC_ADD(v, val)	__sync_add_and_fetch(&(v), val)
#endif

/* Retrieve an ID for the next populate operation. */
static inline uint64_t
get_next_populate(void)
{
	return (ATOMIC_ADD(g_npop_ops, 1));
}

/* Retrieve an ID for the next insert operation. */
static inline uint64_t
get_next_incr(void)
{
	return (ATOMIC_ADD(g_nins_ops, 1));
}

/* Return the total thread read operations. */
static inline uint64_t
sum_read_ops(CONFIG_THREAD *threads, u_int num)
{
	uint64_t total;
	u_int i;

	if (threads == NULL)
		return (0);

	for (i = 0, total = 0; i < num; ++i, ++threads)
		total += threads->read_ops;
	return (total);
}

/* Return the total thread update operations. */
static inline uint64_t
sum_update_ops(CONFIG_THREAD *threads, u_int num)
{
	uint64_t total;
	u_int i;

	if (threads == NULL)
		return (0);

	for (i = 0, total = 0; i < num; ++i, ++threads)
		total += threads->update_ops;
	return (total);
}

static int
enomem(const CONFIG *cfg)
{
	const char *msg;

	msg = "Unable to allocate memory";
	if (cfg->logf == NULL)
		fprintf(stderr, "%s\n", msg);
	else
		lprintf(cfg, ENOMEM, 0, "%s", msg);
	return (ENOMEM);
}

void
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
			if ((op_ret = cursor->insert(cursor)) == 0)
				continue;
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

void *
read_thread(void *arg)
{
	worker((CONFIG_THREAD *)arg, WORKER_READ);
	return (NULL);
}

void *
insert_thread(void *arg)
{
	CONFIG *cfg;

	cfg = ((CONFIG_THREAD *)arg)->cfg;
	worker((CONFIG_THREAD *)arg,
	    cfg->insert_rmw ? WORKER_INSERT_RMW : WORKER_INSERT);
	return (NULL);
}

void *
update_thread(void *arg)
{
	worker((CONFIG_THREAD *)arg, WORKER_UPDATE);
	return (NULL);
}

void *
populate_thread(void *arg)
{
	CONFIG *cfg;
	CONFIG_THREAD *threads;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint32_t opcount;
	uint64_t op;
	int intxn, ret;
	char *data_buf, *key_buf;

	threads = (CONFIG_THREAD *)arg;
	cfg = threads->cfg;
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
			op = get_next_populate();
			if (op > cfg->icount)
				break;

			sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
			cursor->set_key(cursor, key_buf);
			cursor->set_value(cursor, data_buf);
			if ((ret = cursor->insert(cursor)) != 0) {
				lprintf(cfg, ret, 0, "Failed inserting");
				goto err;
			}
		}
	else {
		for (intxn = 0, opcount = 0;;) {
			op = get_next_populate();
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

void *
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
			lprintf(cfg, 0, cfg->verbose,
			    "inserts: %" PRIu64 ", elapsed time: %.2f",
			    g_npop_ops, secs);
			break;
		case WT_PERF_WORKER:
			g_nread_ops =
			    sum_read_ops(cfg->rthreads, cfg->read_threads);
			g_nupdate_ops =
			    sum_update_ops(cfg->uthreads, cfg->update_threads);
			lprintf(cfg, 0, cfg->verbose,
			    "reads: %" PRIu64 " inserts: %" PRIu64
			    " updates: %" PRIu64 ", elapsed time: %.2f",
			    g_nread_ops, g_nins_ops, g_nupdate_ops, secs);
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

void *
checkpoint_worker(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timeval e, s;
	uint64_t ms;
	uint32_t i;
	int ret;

	cfg = (CONFIG *)arg;
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
		assert(gettimeofday(&e, NULL) == 0);
		ms = (e.tv_sec * 1000) + (e.tv_usec / 1000.0);
		ms -= (s.tv_sec * 1000) + (s.tv_usec / 1000.0);
		lprintf(cfg, 0, 1,
		    "Finished checkpoint in %" PRIu64 " ms.", ms);
	}
err:	if (session != NULL)
		assert(session->close(session, NULL) == 0);
	return (arg);
}

int
execute_populate(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	struct timeval e;
	double secs;
	uint64_t last_ops;
	uint32_t interval;
	u_int sleepsec;
	int elapsed, ret;

	conn = cfg->conn;
	cfg->phase = WT_PERF_POPULATE;
	lprintf(cfg, 0, 1, "Starting populate threads");

	/* First create the table. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error opening a session on %s", cfg->home);
		return (ret);
	}

	if ((ret = session->create(
	    session, cfg->uri, cfg->table_config)) != 0) {
		lprintf(cfg, ret, 0, "Error creating table %s", cfg->uri);
		assert(session->close(session, NULL) == 0);
		return (ret);
	}
	assert(session->close(session, NULL) == 0);

	g_npop_ops = 0;
	g_running = 1;
	g_threads_quit = 0;
	if ((ret = start_threads(cfg,
	    cfg->populate_threads, &cfg->popthreads, populate_thread)) != 0)
		return (ret);

	assert(gettimeofday(&cfg->phase_start_time, NULL) == 0);
	for (elapsed = 0, interval = 0, last_ops = 0;
	    g_npop_ops < cfg->icount && g_threads_quit == 0;) {
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
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " ops in %" PRIu32 " secs",
		    g_npop_ops - last_ops, cfg->report_interval);
		last_ops = g_npop_ops;
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

int
execute_workload(CONFIG *cfg)
{
	uint64_t last_inserts, last_reads, last_updates;
	uint32_t interval, run_time;
	int ret, tret;

	lprintf(cfg, 0, 1, "Starting worker threads");
	cfg->phase = WT_PERF_WORKER;

	last_inserts = last_reads = last_updates = 0;
	ret = 0;

	lprintf(cfg, 0, 1,
	    "Starting workload threads: read %" PRIu32
	    ", insert %" PRIu32 ", update %" PRIu32,
	    cfg->read_threads, cfg->insert_threads, cfg->update_threads);

	g_nins_ops = g_nread_ops = g_nupdate_ops = 0;
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

		g_nread_ops = sum_read_ops(cfg->rthreads, cfg->read_threads);
		g_nupdate_ops =
		    sum_update_ops(cfg->uthreads, cfg->update_threads);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates in %" PRIu32 " secs",
		    g_nread_ops - last_reads,
		    g_nins_ops - last_inserts,
		    g_nupdate_ops - last_updates,
		    cfg->report_interval);
		last_reads = g_nread_ops;
		last_inserts = g_nins_ops;
		last_updates = g_nupdate_ops;
	}

	/* One final summation of the operations we've completed. */
	g_nread_ops = sum_read_ops(cfg->rthreads, cfg->read_threads);
	g_nupdate_ops = sum_update_ops(cfg->uthreads, cfg->update_threads);

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
int
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
	WT_SESSION *parse_session;
	pthread_t checkpoint_thread, stat_thread;
	size_t len;
	uint64_t req_len;
	int ch, checkpoint_created, ret, stat_created;
	const char *opts = "C:O:T:h:o:SML";
	const char *wtperftmp_subdir = "wtperftmp";
	const char *user_cconfig, *user_tconfig;
	char *cmd, *cc_buf, *tc_buf, *tmphome;

	conn = NULL;
	parse_session = NULL;
	checkpoint_created = ret = stat_created = 0;
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
	if ((ret = conn->open_session(conn, NULL, NULL, &parse_session)) != 0) {
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
			    &cfg, parse_session, small_config_str) != 0)
				goto einval;
			break;
		case 'M':
			if (config_opt_line(
			    &cfg, parse_session, med_config_str) != 0)
				goto einval;
			break;
		case 'L':
			if (config_opt_line(
			    &cfg, parse_session, large_config_str) != 0)
				goto einval;
			break;
		case 'O':
			if (config_opt_file(
			    &cfg, parse_session, optarg) != 0)
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
			if (config_opt_line(&cfg, parse_session, optarg) != 0)
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
		    &cfg, parse_session, "conn_config", cc_buf)) != 0)
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
		    &cfg, parse_session, "table_config", tc_buf)) != 0)
			goto err;
	}

	ret = parse_session->close(parse_session, NULL);
	parse_session = NULL;
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

	g_util_running = 1;		/* Start the statistics thread. */
	if (cfg.stat_interval != 0) {
		if ((ret = pthread_create(
		    &stat_thread, NULL, stat_worker, &cfg)) != 0) {
			lprintf(
			    &cfg, ret, 0, "Error creating statistics thread.");
			goto err;
		}
		stat_created = 1;
	}				/* Start the checkpoint thread. */
	if (cfg.checkpoint_interval != 0) {
		if ((ret = pthread_create(
		    &checkpoint_thread, NULL, checkpoint_worker, &cfg)) != 0) {
			lprintf(
			    &cfg, ret, 0, "Error creating checkpoint thread.");
			goto err;
		}
		checkpoint_created = 1;
	}
					/* If creating, populate the table. */
	if (cfg.create != 0 && execute_populate(&cfg) != 0)
		goto err;
					/* Not creating, set insert count. */
	if (cfg.create == 0 && find_table_count(&cfg) != 0)
		goto err;
					/* Execute the workload. */
	if (cfg.run_time != 0 &&
	    cfg.read_threads + cfg.insert_threads + cfg.update_threads != 0 &&
	    (ret = execute_workload(&cfg)) != 0)
		goto err;

	lprintf(&cfg, 0, 1,
	    "Ran performance test example with %" PRIu32 " read threads, %"
	    PRIu32 " insert threads and %" PRIu32 " update threads for %"
	    PRIu32 " seconds.",
	    cfg.read_threads, cfg.insert_threads,
	    cfg.update_threads, cfg.run_time);

	if (cfg.read_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " read operations", g_nread_ops);
	if (cfg.insert_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " insert operations", g_nins_ops);
	if (cfg.update_threads != 0)
		lprintf(&cfg, 0, 1,
		    "Executed %" PRIu64 " update operations", g_nupdate_ops);

	if (0) {
einval:		ret = EINVAL;
	}
err:	g_util_running = 0;

	if (checkpoint_created != 0 &&
	    (ret = pthread_join(checkpoint_thread, NULL)) != 0)
		lprintf(&cfg, ret, 0, "Error joining checkpoint thread.");
	if (stat_created != 0 &&
	    (ret = pthread_join(stat_thread, NULL)) != 0)
		lprintf(&cfg, ret, 0, "Error joining stat thread.");

	if (parse_session != NULL)
		assert(parse_session->close(parse_session, NULL) == 0);
	if (conn != NULL && (ret = conn->close(conn, NULL)) != 0)
		lprintf(&cfg, ret, 0,
		    "Error closing connection to %s", cfg.home);

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

/*
 * Following are utility functions.
 */

/* Assign the src config to the dest.
 * Any storage allocated in dest is freed as a result.
 */
int
config_assign(CONFIG *dest, const CONFIG *src)
{
	size_t i, len;
	char *newstr, **pstr;

	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((u_char *)dest + config_opts[i].offset);
			if (*pstr != NULL) {
				len = strlen(*pstr) + 1;
				if ((newstr = malloc(len)) == NULL)
					return (enomem(src));
				strncpy(newstr, *pstr, len);
				*pstr = newstr;
			}
		}
	return (0);
}

/* Free any storage allocated in the config struct.
 */
void
config_free(CONFIG *cfg)
{
	size_t i;
	char **pstr;

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((unsigned char *)cfg + config_opts[i].offset);
			if (*pstr != NULL) {
				free(*pstr);
				*pstr = NULL;
			}
		}

	free(cfg->uri);
}

/*
 * Check a single key=value returned by the config parser
 * against our table of valid keys, along with the expected type.
 * If everything is okay, set the value.
 */
int
config_opt(CONFIG *cfg, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	CONFIG_OPT *popt;
	char *newstr, **strp;
	size_t i, nopt;
	uint64_t newlen;
	void *valueloc;

	popt = NULL;
	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++)
		if (strlen(config_opts[i].name) == k->len &&
		    strncmp(config_opts[i].name, k->str, k->len) == 0) {
			popt = &config_opts[i];
			break;
		}
	if (popt == NULL) {
		fprintf(stderr, "wtperf: Error: "
		    "unknown option \'%.*s\'\n", (int)k->len, k->str);
		fprintf(stderr, "Options:\n");
		for (i = 0; i < nopt; i++)
			fprintf(stderr, "\t%s\n", config_opts[i].name);
		return (EINVAL);
	}
	valueloc = ((unsigned char *)cfg + popt->offset);
	switch (popt->type) {
	case BOOL_TYPE:
		if (v->type != WT_CONFIG_ITEM_BOOL) {
			fprintf(stderr, "wtperf: Error: "
			    "bad bool value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case INT_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad int value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val > INT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "int value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case UINT32_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad uint32 value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val < 0 || v->val > UINT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "uint32 value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(uint32_t *)valueloc = (uint32_t)v->val;
		break;
	case CONFIG_STRING_TYPE:
		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		newlen = v->len + 1;
		if (*strp == NULL) {
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
			strncpy(newstr, v->str, v->len);
		} else {
			newlen += (strlen(*strp) + 1);
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
			snprintf(newstr, newlen,
			    "%s,%*s", *strp, (int)v->len, v->str);
			/* Free the old value now we've copied it. */
			free(*strp);
		}
		*strp = newstr;
		break;
	case STRING_TYPE:
		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		free(*strp);
		if ((newstr = malloc(v->len + 1)) == NULL)
			return (enomem(cfg));
		strncpy(newstr, v->str, v->len);
		newstr[v->len] = '\0';
		*strp = newstr;
		break;
	}
	return (0);
}

/* Parse a configuration file.
 * We recognize comments '#' and continuation via lines ending in '\'.
 */
int
config_opt_file(CONFIG *cfg, WT_SESSION *parse_session, const char *filename)
{
	FILE *fp;
	size_t linelen, optionpos;
	int contline, linenum, ret;
	char line[256], option[1024];
	char *comment, *ltrim, *rtrim;

	if ((fp = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "wtperf: %s: %s\n", filename, strerror(errno));
		return (errno);
	}

	ret = 0;
	optionpos = 0;
	linenum = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		linenum++;
		/* trim the line */
		for (ltrim = line; *ltrim && isspace(*ltrim); ltrim++)
			;
		rtrim = &ltrim[strlen(ltrim)];
		if (rtrim > ltrim && rtrim[-1] == '\n')
			rtrim--;

		contline = (rtrim > ltrim && rtrim[-1] == '\\');
		if (contline)
			rtrim--;

		comment = strchr(ltrim, '#');
		if (comment != NULL && comment < rtrim)
			rtrim = comment;
		while (rtrim > ltrim && isspace(rtrim[-1]))
			rtrim--;

		linelen = (size_t)(rtrim - ltrim);
		if (linelen == 0)
			continue;

		if (linelen + optionpos + 1 > sizeof(option)) {
			fprintf(stderr, "wtperf: %s: %d: line overflow\n",
			    filename, linenum);
			ret = EINVAL;
			break;
		}
		*rtrim = '\0';
		strncpy(&option[optionpos], ltrim, linelen);
		option[optionpos + linelen] = '\0';
		if (contline)
			optionpos += linelen;
		else {
			if ((ret = config_opt_line(cfg,
				    parse_session, option)) != 0) {
				fprintf(stderr, "wtperf: %s: %d: parse error\n",
				    filename, linenum);
				break;
			}
			optionpos = 0;
		}
	}
	if (ret == 0 && optionpos > 0) {
		fprintf(stderr, "wtperf: %s: %d: last line continues\n",
		    filename, linenum);
		ret = EINVAL;
	}

	(void)fclose(fp);
	return (ret);
}

/* Parse a single line of config options.
 * Continued lines have already been joined.
 */
int
config_opt_line(CONFIG *cfg, WT_SESSION *parse_session, const char *optstr)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;
	WT_CONNECTION *conn;
	WT_EXTENSION_API *wt_api;
	int ret, t_ret;

	conn = parse_session->connection;
	wt_api = conn->get_extension_api(conn);

	if ((ret = wt_api->config_scan_begin(wt_api, parse_session, optstr,
	    strlen(optstr), &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

	while (ret == 0) {
		if ((ret =
		    wt_api->config_scan_next(wt_api, scan, &k, &v)) != 0) {
			/* Any parse error has already been reported. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			break;
		}
		ret = config_opt(cfg, &k, &v);
	}
	if ((t_ret = wt_api->config_scan_end(wt_api, scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_end");
		if (ret == 0)
			ret = t_ret;
	}

	return (ret);
}

/* Set a single string config option */
int
config_opt_str(CONFIG *cfg, WT_SESSION *parse_session,
    const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name="value" */
	if ((optstr = malloc(strlen(name) + strlen(value) + 4)) == NULL)
		return (enomem(cfg));
	sprintf(optstr, "%s=\"%s\"", name, value);
	ret = config_opt_line(cfg, parse_session, optstr);
	free(optstr);
	return (ret);
}

/* Set a single int config option */
int
config_opt_int(CONFIG *cfg, WT_SESSION *parse_session,
    const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name=value */
	if ((optstr = malloc(strlen(name) + strlen(value) + 2)) == NULL)
		return (enomem(cfg));
	sprintf(optstr, "%s=%s", name, value);
	ret = config_opt_line(cfg, parse_session, optstr);
	free(optstr);
	return (ret);
}

void
config_opt_usage(void)
{
	size_t i, linelen, nopt;
	const char *defaultval, *typestr;

	printf("Following are options settable using -o or -O, "
	    "showing [default value].\n");
	printf("String values must be enclosed by \" quotes, ");
	printf("boolean values must be either true or false.\n\n");

	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++) {
		typestr = "?";
		defaultval = config_opts[i].defaultval;
		switch (config_opts[i].type) {
		case BOOL_TYPE:
			typestr = "bool";
			if (strcmp(defaultval, "0") == 0)
				defaultval = "true";
			else
				defaultval = "false";
			break;
		case CONFIG_STRING_TYPE:
		case STRING_TYPE:
			typestr = "string";
			break;
		case INT_TYPE:
			typestr = "int";
			break;
		case UINT32_TYPE:
			typestr = "unsigned int";
			break;
		}
		linelen = (size_t)printf("  %s=<%s> [%s]",
		    config_opts[i].name, typestr, defaultval);
		if (linelen + 2 + strlen(config_opts[i].description) < 80)
			printf("  %s\n", config_opts[i].description);
		else {
			printf("\n");
			indent_lines(config_opts[i].description, "        ");
		}
	}
}

int
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

int
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

/*
 * Log printf - output a log message.
 */
void
lprintf(const CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
{
	va_list ap;

	if (err == 0 && level <= cfg->verbose) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, "\n");

		if (level < cfg->verbose) {
			va_start(ap, fmt);
			vprintf(fmt, ap);
			va_end(ap);
			printf("\n");
		}
	}
	if (err == 0)
		return;

	/* We are dealing with an error. */
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " Error: %s\n", wiredtiger_strerror(err));
	if (cfg->logf != NULL) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, " Error: %s\n", wiredtiger_strerror(err));
	}

	/* Never attempt to continue if we got a panic from WiredTiger. */
	if (err == WT_PANIC)
		exit(1);
}

/* Setup the logging output mechanism. */
int
setup_log_file(CONFIG *cfg)
{
	char *fname;
	int ret;

	ret = 0;

	if (cfg->verbose < 1 && cfg->stat_interval == 0)
		return (0);

	if ((fname = calloc(strlen(cfg->home) +
	    strlen(cfg->table_name) + strlen(".stat") + 2, 1)) == NULL)
		return (enomem(cfg));

	sprintf(fname, "%s/%s.stat", cfg->home, cfg->table_name);
	if ((cfg->logf = fopen(fname, "w")) == NULL) {
		fprintf(stderr, "Statistics failed to open log file.\n");
		ret = EINVAL;
	} else {
		/* Use line buffering for the log file. */
		(void)setvbuf(cfg->logf, NULL, _IOLBF, 0);
	}
	free(fname);
	return (ret);
}

uint64_t
wtperf_value_range(CONFIG *cfg)
{
	if (cfg->random_range == 0)
		return (cfg->icount + g_nins_ops - (cfg->insert_threads + 1));
	else
		return (cfg->icount + cfg->random_range);
}

extern uint32_t __wt_random(void);

uint64_t
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

void
indent_lines(const char *lines, const char *indent)
{
	const char *bol, *eol;
	int len;

	bol = lines;
	while (bol != NULL) {
		eol = strchr(bol, '\n');
		if (eol == NULL)
			len = (int)strlen(bol);
		else
			len = (int)(eol++ - bol);
		printf("%s%.*s\n", indent, len, bol);
		bol = eol;
	}
}

void
print_config(CONFIG *cfg)
{
	printf("Workload configuration:\n");
	printf("\thome: %s\n", cfg->home);
	printf("\ttable_name: %s\n", cfg->table_name);
	printf("\tConnection configuration: %s\n", cfg->conn_config);
	printf("\tTable configuration: %s\n", cfg->table_config);
	printf("\t%s\n", cfg->create ? "Creating" : "Using existing");
	printf("\tWorkload period: %" PRIu32 "\n", cfg->run_time);
	printf(
	    "\tCheckpoint interval: %" PRIu32 "\n", cfg->checkpoint_interval);
	printf("\tReporting interval: %" PRIu32 "\n", cfg->report_interval);
	printf("\tStatistics interval: %" PRIu32 "\n", cfg->stat_interval);
	if (cfg->create) {
		printf("\tInsert count: %" PRIu32 "\n", cfg->icount);
		printf("\tNumber populate threads: %" PRIu32 "\n",
		    cfg->populate_threads);
	}
	printf("\tNumber read threads: %" PRIu32 "\n", cfg->read_threads);
	printf("\tNumber insert threads: %" PRIu32 "\n", cfg->insert_threads);
	if (cfg->insert_rmw)
		printf("\tInsert operations are RMW.\n");
	printf("\tNumber update threads: %" PRIu32 "\n", cfg->update_threads);
	printf("\tkey size: %" PRIu32 " data size: %" PRIu32 "\n",
	    cfg->key_sz, cfg->data_sz);
	printf("\tVerbosity: %" PRIu32 "\n", cfg->verbose);
}

void
usage(void)
{
	printf("wtperf [-LMSv] [-C config] "
	    "[-h home] [-O file] [-o option] [-T config]\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-O <file> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\n");
	config_opt_usage();
}
