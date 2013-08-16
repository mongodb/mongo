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
 *
 * wtperf.c
 *	This is an application that executes parallel random read workload.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>
#include <stddef.h>
#include <ctype.h>
#include <limits.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#define	ATOMIC_ADD(v, val)						\
	__sync_add_and_fetch(&(v), val)
#ifndef F_CLR
#define	F_CLR(p, mask)		((p)->flags &= ~((uint32_t)(mask)))
#endif
#ifndef F_ISSET
#define	F_ISSET(p, mask)	((p)->flags & ((uint32_t)(mask)))
#endif
#ifndef F_SET
#define	F_SET(p, mask)		((p)->flags |= ((uint32_t)(mask)))
#endif

typedef struct {
	const char *home;
	WT_CONNECTION *conn;
	FILE *logf;
#define	WT_PERF_INIT		0x00
#define	WT_PERF_POP		0x01
#define	WT_PERF_READ		0x02
	uint32_t phase;
#define PERF_INSERT_RMW		0x01
#define PERF_RAND_PARETO	0x02 /* Use the Pareto random distribution. */
#define PERF_RAND_WORKLOAD	0x04
	uint32_t flags;
	struct timeval phase_start_time;
	uint32_t rand_range; /* The range to use if doing random inserts. */
	uint32_t elapsed_time;

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT

} CONFIG;

typedef enum {
	UINT32_TYPE, STRING_TYPE, BOOL_TYPE, FLAG_TYPE
} CONFIG_OPT_TYPE;

typedef struct {
	const char *name;
	const char *description;
	const char *defaultval;
	CONFIG_OPT_TYPE type;
	size_t offset;
	uint32_t flagmask;
} CONFIG_OPT;

/* All options changeable on command line using -o or -O are listed here. */
CONFIG_OPT config_opts[] = {

#define OPT_DEFINE_DESC
#include "wtperf_opt.i"
#undef OPT_DEFINE_DESC

};

/* Forward function definitions. */
void *checkpoint_worker(void *);
void config_assign(CONFIG *, const CONFIG *);
void config_free(CONFIG *);
int config_opt(CONFIG *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *);
int config_opt_file(CONFIG *, WT_SESSION *, const char *);
int config_opt_int(CONFIG *, WT_SESSION *, const char *, const char *);
int config_opt_line(CONFIG *, WT_SESSION *, const char *);
int config_opt_str(CONFIG *, WT_SESSION *, const char *, const char *);
void config_opt_usage(void);
int connection_reconfigure(WT_CONNECTION *, const char *);
int execute_populate(CONFIG *);
int execute_workload(CONFIG *);
int find_table_count(CONFIG *);
int get_next_op(uint64_t *);
void indent_lines(const char *, const char *);
void *insert_thread(void *);
int lprintf(CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format (printf, 4, 5)))
#endif
;
void *populate_thread(void *);
void print_config(CONFIG *);
void *read_thread(void *);
int setup_log_file(CONFIG *);
int start_threads(CONFIG *, u_int, pthread_t **, void *(*func)(void *));
void *stat_worker(void *);
int stop_threads(CONFIG *, u_int, pthread_t *);
char *strstr_right(const char *str, const char *match, const char **rightp);
void *update_thread(void *);
void usage(void);
void worker(CONFIG *, uint32_t);
uint64_t wtperf_rand(CONFIG *);
void wtperf_srand(CONFIG *);
uint64_t wtperf_value_range(CONFIG *);

#define	DEFAULT_LSM_CONFIG						\
	"key_format=S,value_format=S,exclusive,"			\
	"leaf_page_max=4kb,internal_page_max=64kb,allocation_size=4kb,"

/* Worker thread types. */
#define WORKER_READ		0x01
#define WORKER_INSERT		0x02
#define WORKER_INSERT_RMW	0x03
#define WORKER_UPDATE		0x04

/* Default values. */
CONFIG default_cfg = {
	"WT_TEST",	/* home */
	NULL,		/* conn */
	NULL,		/* logf */
	WT_PERF_INIT, /* phase */
	0,		/* flags */
	{0, 0},		/* phase_start_time */
	0,		/* rand_range */
	0,		/* elapsed_time */

#define OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT

};

const char *small_config_str =
    "conn_config=\"create,cache_size=500MB\","
    "table_config=\"" DEFAULT_LSM_CONFIG "lsm_chunk_size=5MB,\","
    "icount=500000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=20,"
    "populate_threads=1,"
    "read_threads=8,";

const char *med_config_str =
    "conn_config=\"create,cache_size=1GB\","
    "table_config=\"" DEFAULT_LSM_CONFIG "lsm_chunk_size=20MB,\","
    "icount=50000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=100,"
    "populate_threads=1,"
    "read_threads=16,";

const char *large_config_str =
    "conn_config=\"create,cache_size=2GB\","
    "table_config=\"" DEFAULT_LSM_CONFIG "lsm_chunk_size=50MB,\","
    "icount=500000000,"
    "data_sz=100,"
    "key_sz=20,"
    "report_interval=5,"
    "run_time=600,"
    "populate_threads=1,"
    "read_threads=16,";


const char *debug_cconfig = "verbose=[lsm]";
const char *debug_tconfig = "";

/* Global values shared by threads. */
/*
 * g_nins_ops is used to track both insert count and assign keys, so use this
 * to track insert failures.
 */
uint64_t g_nfailedins_ops; 
uint64_t g_nins_ops;
uint64_t g_npop_ops;
uint64_t g_nread_ops;
uint64_t g_nupdate_ops;
uint64_t g_nworker_ops;
int g_running;
int g_util_running;
uint32_t g_threads_quit; /* For tracking threads that exit early. */

/* End global values shared by threads. */

void *
read_thread(void *arg)
{
	worker((CONFIG *)arg, WORKER_READ);
	return (NULL);
}

void *
insert_thread(void *arg)
{
	CONFIG *config;

	config = (CONFIG *)arg;
	worker(config, F_ISSET(config, PERF_INSERT_RMW) ?
	    WORKER_INSERT_RMW : WORKER_INSERT);
	return (NULL);
}

void *
update_thread(void *arg)
{
	worker((CONFIG *)arg, WORKER_UPDATE);
	return (NULL);
}

void
worker(CONFIG *cfg, uint32_t worker_type)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t next_incr, next_val;
	int ret, op_ret;
	const char *op_name = "search";
	char *data_buf, *key_buf, *value;

	session = NULL;
	data_buf = key_buf = NULL;
	op_ret = 0;

	conn = cfg->conn;
	key_buf = calloc(cfg->key_sz + 1, 1);
	if (key_buf == NULL) {
		lprintf(cfg, ret = ENOMEM, 0, "Populate key buffer");
		goto err;
	}
	if (worker_type == WORKER_INSERT || worker_type == WORKER_UPDATE) {
		data_buf = calloc(cfg->data_sz, 1);
		if (data_buf == NULL) {
			lprintf(cfg, ret = ENOMEM, 0, "Populate data buffer");
			goto err;
		}
		memset(data_buf, 'a', cfg->data_sz - 1);
	}

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in read thread");
		goto err;
	}
	if ((ret = session->open_cursor(session, cfg->uri,
	    NULL, NULL, &cursor)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_cursor failed in read thread");
		goto err;
	}

	while (g_running) {
		/* Get a value in range, avoid zero. */
		if (worker_type == WORKER_INSERT)
			next_incr = ATOMIC_ADD(g_nins_ops, 1);

		if (!F_ISSET(cfg, PERF_RAND_WORKLOAD) &&
		    worker_type == WORKER_INSERT)
			next_val = cfg->icount + next_incr;
		else
			next_val = wtperf_rand(cfg);
		/*
		 * If the workload is started without a populate phase we
		 * rely on at least one insert to get a valid item id.
		 */
		if (worker_type != WORKER_INSERT &&
		    wtperf_value_range(cfg) < next_val)
			continue;
		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, next_val);
		cursor->set_key(cursor, key_buf);
		switch(worker_type) {
		case WORKER_READ:
			op_name = "read";
			op_ret = cursor->search(cursor);
			if (F_ISSET(cfg, PERF_RAND_WORKLOAD) &&
			    op_ret == WT_NOTFOUND)
				op_ret = 0;
			if (op_ret == 0)
				++g_nread_ops;
			break;
		case WORKER_INSERT_RMW:
			op_name="insert_rmw";
			op_ret = cursor->search(cursor);
			if (op_ret != WT_NOTFOUND)
				break;
			/* Fall through */
		case WORKER_INSERT:
			op_name = "insert";
			cursor->set_value(cursor, data_buf);
			op_ret = cursor->insert(cursor);
			if (F_ISSET(cfg, PERF_RAND_WORKLOAD) &&
			    op_ret == WT_DUPLICATE_KEY)
				op_ret = 0;
			if (op_ret != 0)
				++g_nfailedins_ops;
			break;
		case WORKER_UPDATE:
			op_name = "update";
			op_ret = cursor->search(cursor);
			if (op_ret == 0) {
				cursor->get_value(cursor, &value);
				memcpy(data_buf, value, cfg->data_sz);
				if (data_buf[0] == 'a')
					data_buf[0] = 'b';
				else
					data_buf[0] = 'a';
				cursor->set_value(cursor, data_buf);
				op_ret = cursor->update(cursor);
			}
			if (F_ISSET(cfg, PERF_RAND_WORKLOAD) &&
			    op_ret == WT_NOTFOUND)
				op_ret = 0;
			if (op_ret == 0)
				++g_nupdate_ops;
			break;
		default:
			lprintf(cfg, EINVAL, 0, "Invalid worker type");
			goto err;
		}

		/* Report errors and continue. */
		if (op_ret != 0)
			lprintf(cfg, op_ret, 0,
			    "%s failed for: %s", op_name, key_buf);
		else
			++g_nworker_ops;
	}

err:	if (ret != 0)
		++g_threads_quit;
	if (session != NULL)
		session->close(session, NULL);
	if (data_buf != NULL)
		free(data_buf);
	if (key_buf != NULL)
		free(key_buf);
}

/* Retrieve an ID for the next insert operation. */
int get_next_op(uint64_t *op)
{
	*op = ATOMIC_ADD(g_npop_ops, 1);
	return (0);
}

void *
populate_thread(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t op;
	int ret;
	char *data_buf, *key_buf;

	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	session = NULL;
	data_buf = key_buf = NULL;
	ret = 0;

	cfg->phase = WT_PERF_POP;

	data_buf = calloc(cfg->data_sz, 1);
	if (data_buf == NULL) {
		lprintf(cfg, ENOMEM, 0, "Populate data buffer");
		goto err;
	}
	key_buf = calloc(cfg->key_sz + 1, 1);
	if (key_buf == NULL) {
		lprintf(cfg, ENOMEM, 0, "Populate key buffer");
		goto err;
	}

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "Error opening a session on %s", cfg->home);
		goto err;
	}

	/* Do a bulk load if populate is single-threaded. */
	if ((ret = session->open_cursor(session, cfg->uri, NULL,
	    cfg->populate_threads == 1 ? "bulk" : NULL, &cursor)) != 0) {
		lprintf(cfg, ret, 0, "Error opening cursor %s", cfg->uri);
		goto err;
	}

	memset(data_buf, 'a', cfg->data_sz - 1);
	cursor->set_value(cursor, data_buf);
	/* Populate the database. */
	while (1) {
		get_next_op(&op);
		if (op > cfg->icount)
			break;
		sprintf(key_buf, "%0*" PRIu64, cfg->key_sz, op);
		cursor->set_key(cursor, key_buf);
		if ((ret = cursor->insert(cursor)) != 0) {
			lprintf(cfg, ret, 0, "Failed inserting");
			goto err;
		}
	}
	/* To ensure managing thread knows if we exited early. */
err:	if (ret != 0)
		++g_threads_quit;
	if (session != NULL)
		session->close(session, NULL);
	if (data_buf)
		free(data_buf);
	if (key_buf)
		free(key_buf);
	return (arg);
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
		lprintf(cfg, ENOMEM, 0, "Statistics thread uri create.");
		goto err;
	}
	(void)snprintf(stat_uri, uri_len, "statistics:%s", cfg->uri);

	while (g_util_running) {
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < cfg->stat_interval; i++) {
			sleep(cfg->report_interval);
			if (!g_util_running)
				break;
		}
		/* Generic header. */
		lprintf(cfg, 0, cfg->verbose,
		    "=======================================");
		gettimeofday(&e, NULL);
		secs = e.tv_sec + e.tv_usec / 1000000.0;
		secs -= (cfg->phase_start_time.tv_sec +
		    cfg->phase_start_time.tv_usec / 1000000.0);
		if (secs == 0)
			++secs;
		if (cfg->phase == WT_PERF_POP)
			lprintf(cfg, 0, cfg->verbose,
			    "inserts: %" PRIu64 ", elapsed time: %.2f",
			    g_npop_ops, secs);
		else
			lprintf(cfg, 0, cfg->verbose,
			    "reads: %" PRIu64 " inserts: %" PRIu64
			    " updates: %" PRIu64 ", elapsed time: %.2f",
			    g_nread_ops, g_nins_ops, g_nupdate_ops, secs);

		/* Report data source stats. */
		if ((ret = session->open_cursor(session, stat_uri,
		    NULL, "statistics_fast", &cursor)) != 0) {
			lprintf(cfg, ret, 0,
			    "open_cursor failed for data source statistics");
			goto err;
		}
		while ((ret = cursor->next(cursor)) == 0 && (ret =
		    cursor->get_value(cursor, &desc, &pvalue, &value)) == 0 &&
		    value != 0)
			lprintf(cfg, 0, cfg->verbose,
			    "stat:lsm: %s=%s", desc, pvalue);
		cursor->close(cursor);
		lprintf(cfg, 0, cfg->verbose, "-----------------");

		/* Dump the connection statistics since last time. */
		if ((ret = session->open_cursor(session, "statistics:",
		    NULL, "statistics_clear", &cursor)) != 0) {
			lprintf(cfg, ret, 0,
			    "open_cursor failed in statistics");
			goto err;
		}
		while ((ret = cursor->next(cursor)) == 0 && (ret =
		    cursor->get_value(cursor, &desc, &pvalue, &value)) == 0 &&
		    value != 0)
			lprintf(cfg, 0, cfg->verbose,
			    "stat:conn: %s=%s", desc, pvalue);
		cursor->close(cursor);
	}
err:	if (session != NULL)
		session->close(session, NULL);
	if (stat_uri != NULL)
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

	session = NULL;
	cfg = (CONFIG *)arg;
	conn = cfg->conn;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in checkpoint thread.");
		goto err;
	}

	while (g_util_running) {
		/*
		 * TODO: do we care how long the checkpoint takes?
		 */
		/* Break the sleep up, so we notice interrupts faster. */
		for (i = 0; i < cfg->checkpoint_interval; i++) {
			sleep(cfg->report_interval);
			if (!g_util_running)
				break;
		}

		gettimeofday(&s, NULL);
		if ((ret = session->checkpoint(session, NULL)) != 0)
			/* Report errors and continue. */
			lprintf(cfg, ret, 0, "Checkpoint failed.");
		gettimeofday(&e, NULL);
		ms = (e.tv_sec * 1000) + (e.tv_usec / 1000.0);
		ms -= (s.tv_sec * 1000) + (s.tv_usec / 1000.0);
		lprintf(cfg, 0, 1,
		    "Finished checkpoint in %" PRIu64 " ms.", ms);
	}
err:	if (session != NULL)
		session->close(session, NULL);
	return (arg);
}

int execute_populate(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	pthread_t *threads;
	struct timeval e;
	double secs;
	int ret;
	uint64_t elapsed, last_ops;

	conn = cfg->conn;
	cfg->phase = WT_PERF_POP;
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
		session->close(session, NULL);
		return (ret);
	}
	session->close(session, NULL);

	if ((ret = start_threads(
	    cfg, cfg->populate_threads, &threads, populate_thread)) != 0)
		return (ret);

	gettimeofday(&cfg->phase_start_time, NULL);
	for (cfg->elapsed_time = 0, elapsed = last_ops = 0;
	    g_npop_ops < cfg->icount &&
	    g_threads_quit < cfg->populate_threads;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, so adjust accordingly.
		 */
		usleep(10000);
		elapsed += 1;
		if (elapsed % 100 == 0 &&
		    (elapsed / 100) % cfg->report_interval == 0) {
			lprintf(cfg, 0, 1, "%" PRIu64 " ops in %d secs",
			    g_npop_ops - last_ops, cfg->report_interval);
			last_ops = g_npop_ops;
		}
	}
	if (g_threads_quit == cfg->populate_threads) {
		lprintf(cfg, WT_ERROR, 0,
		    "Populate threads exited without finishing.");
		return (WT_ERROR);
	}
	gettimeofday(&e, NULL);

	if ((ret = stop_threads(cfg, cfg->populate_threads, threads)) != 0)
		return (ret);

	lprintf(cfg, 0, 1,
	    "Finished load of %d items", cfg->icount);
	secs = e.tv_sec + e.tv_usec / 1000000.0;
	secs -= (cfg->phase_start_time.tv_sec +
	    cfg->phase_start_time.tv_usec / 1000000.0);
	if (secs == 0)
		++secs;
	lprintf(cfg, 0, 1,
	    "Load time: %.2f\n" "load ops/sec: %.2f",
	    secs, cfg->icount / secs);

	return (0);
}

int execute_workload(CONFIG *cfg)
{
	pthread_t *ithreads, *rthreads, *uthreads;
	uint64_t last_inserts, last_reads, last_updates;
	int ret;

	cfg->phase = WT_PERF_READ;
	last_inserts = last_reads = last_updates = 0;
	lprintf(cfg, 0, 1, "Starting read threads");

	if (cfg->read_threads != 0 && (ret = start_threads(
	    cfg, cfg->read_threads, &rthreads, read_thread)) != 0)
		return (ret);

	if (cfg->insert_threads != 0 && (ret = start_threads(
	    cfg, cfg->insert_threads, &ithreads, insert_thread)) != 0)
		return (ret);

	if (cfg->update_threads != 0 && (ret = start_threads(
	    cfg, cfg->update_threads, &uthreads, update_thread)) != 0)
		return (ret);

	/* Sanity check reporting interval. */
	if (cfg->report_interval > cfg->run_time)
		cfg->report_interval = cfg->run_time;

	gettimeofday(&cfg->phase_start_time, NULL);
	for (cfg->elapsed_time = 0;
	    cfg->elapsed_time < cfg->run_time &&
	    g_threads_quit < cfg->read_threads;
	    cfg->elapsed_time += cfg->report_interval) {
		sleep(cfg->report_interval);
		lprintf(cfg, 0, 1,
		    "%" PRIu64 " reads, %" PRIu64 " inserts, %" PRIu64
		    " updates in %d secs",
		    g_nread_ops - last_reads,
		    g_nins_ops - last_inserts,
		    g_nupdate_ops - last_updates,
		    cfg->report_interval);
		last_reads = g_nread_ops;
		last_inserts = g_nins_ops;
		last_updates = g_nupdate_ops;
	}
	/* Report if any worker threads didn't finish. */
	if (g_threads_quit != 0)
		lprintf(cfg, WT_ERROR, 0,
		    "Worker thread(s) exited without finishing.");

	if (cfg->read_threads != 0 &&
	    (ret = stop_threads(cfg, cfg->read_threads, rthreads)) != 0)
		return (ret);

	if (cfg->insert_threads != 0 &&
	    (ret = stop_threads(cfg, cfg->insert_threads, ithreads)) != 0)
		return (ret);

	if (cfg->update_threads != 0 &&
	    (ret = stop_threads(cfg, cfg->update_threads, uthreads)) != 0)
		return (ret);

	return (0);
}

/*
 * Ensure that icount matches the number of records in the 
 * existing table.
 */
int find_table_count(CONFIG *cfg)
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
	cursor->get_key(cursor, &key);
	cfg->icount = (uint32_t)atoi(key);

err:	session->close(session, NULL);
	return (ret);
}

/* Same as strstr, but also returns the right boundary of the match if found */
char *strstr_right(const char *str, const char *match, const char **rightp)
{
	char *result;

	if ((result = strstr(str, match)) != NULL)
		*rightp = result + strlen(match);
	else
		*rightp = NULL;
	return result;
}

/* Strip out any create parameter before reconfiguring */
int connection_reconfigure(WT_CONNECTION *conn, const char *orig)
{
	char *alloced;
	const char *config, *left, *right;
	int ret;
	size_t alloclen, leftlen;

	alloced = NULL;
	if ((left = strstr_right(orig, ",create,", &right)) != NULL ||
	    (left = strstr_right(orig, "create,", &right)) == orig ||
	    ((left = strstr_right(orig, ",create", &right)) != NULL &&
	    right == &orig[strlen(orig)])) {

		leftlen = (size_t)(left - orig);
		alloclen = leftlen + strlen(right) + 1;
		alloced = malloc(alloclen);
		strncpy(alloced, orig, leftlen);
		strncpy(&alloced[leftlen], right, alloclen - leftlen);
		config = alloced;
	} else
		config = orig;

	ret = conn->reconfigure(conn, config);
	if (alloced != NULL)
		free(alloced);
	return (ret);
}


int main(int argc, char **argv)
{
	CONFIG cfg;
	WT_CONNECTION *conn;
	WT_SESSION *parse_session;
	pthread_t checkpoint, stat;
	uint64_t req_len;
	int ch, checkpoint_created, ret, stat_created;
	const char *user_cconfig, *user_tconfig;
	const char *opts = "C:O:T:h:o:SML";
	char *cc_buf, *tc_buf;

	/* Setup the default configuration values. */
	memset(&cfg, 0, sizeof(cfg));
	config_assign(&cfg, &default_cfg);
	cc_buf = tc_buf = NULL;
	user_cconfig = user_tconfig = NULL;
	conn = NULL;
	checkpoint_created = stat_created = 0;
	parse_session = NULL;

	/*
	 * First do a basic validation of options,
	 * and home is needed before open.
	 */
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'h':
			cfg.home = optarg;
			break;
		case '?':
			fprintf(stderr, "Invalid option\n");
			usage();
			return (EINVAL);
		}

	/*
	 * We do the open now, since we'll need a connection and
	 * session to use the extension config parser.  We will
	 * reconfigure later as needed.
	 */
	if ((ret = wiredtiger_open(
	    cfg.home, NULL, "create,cache_size=1M", &conn)) != 0) {
		lprintf(&cfg, ret, 0, "Error connecting to %s", cfg.home);
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
			if (config_opt_line(&cfg,
				parse_session, small_config_str) != 0)
				return (EINVAL);
			break;
		case 'M':
			if (config_opt_line(&cfg,
				parse_session, med_config_str) != 0)
				return (EINVAL);
			break;
		case 'L':
			if (config_opt_line(&cfg,
				parse_session, large_config_str) != 0)
				return (EINVAL);
			break;
		case 'O':
			if (config_opt_file(&cfg,
				parse_session, optarg) != 0)
				return (EINVAL);
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
				return (EINVAL);
			break;
		case 'C':
			user_cconfig = optarg;
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		}

	if (cfg.rand_range > 0)
		F_SET(&cfg, PERF_RAND_WORKLOAD);

	if ((ret = setup_log_file(&cfg)) != 0)
		goto err;

	/* Make stdout line buffered, so verbose output appears quickly. */
	(void)setvbuf(stdout, NULL, _IOLBF, 0);

	/* Concatenate non-default configuration strings. */
	if (cfg.verbose > 1 || user_cconfig != NULL) {
		req_len = strlen(cfg.conn_config) + strlen(debug_cconfig) + 3;
		if (user_cconfig != NULL)
			req_len += strlen(user_cconfig);
		cc_buf = calloc(req_len, 1);
		if (cc_buf == NULL) {
			ret = ENOMEM;
			goto err;
		}
		snprintf(cc_buf, req_len, "%s%s%s%s%s",
		    cfg.conn_config,
		    cfg.verbose > 1 ? "," : "",
		    cfg.verbose > 1 ? debug_cconfig : "",
		    user_cconfig ? "," : "", user_cconfig ? user_cconfig : "");
		config_opt_str(&cfg, parse_session,
		    "conn_config", cc_buf);
	}
	if (cfg.verbose > 1 || user_tconfig != NULL) {
		req_len = strlen(cfg.table_config) + strlen(debug_tconfig) + 3;
		if (user_tconfig != NULL)
			req_len += strlen(user_tconfig);
		tc_buf = calloc(req_len, 1);
		if (tc_buf == NULL) {
			ret = ENOMEM;
			goto err;
		}
		snprintf(tc_buf, req_len, "%s%s%s%s%s",
		    cfg.table_config,
		    cfg.verbose > 1 ? "," : "",
		    cfg.verbose > 1 ? debug_tconfig : "",
		    user_tconfig ? "," : "", user_tconfig ? user_tconfig : "");
		config_opt_str(&cfg, parse_session,
		    "table_config", tc_buf);
	}

	wtperf_srand(&cfg);

	parse_session->close(parse_session, NULL);
	parse_session = NULL;

	if (cfg.verbose > 1)
		print_config(&cfg);

	/* Reconfigure our connection to the database. */
	if ((ret = connection_reconfigure(conn, cfg.conn_config)) != 0) {
		lprintf(&cfg, ret, 0, "Error configuring using %s",
		    cfg.conn_config);
		goto err;
	}

	cfg.conn = conn;

	g_util_running = 1;
	if (cfg.stat_interval != 0) {
		if ((ret = pthread_create(
		    &stat, NULL, stat_worker, &cfg)) != 0) {
			lprintf(&cfg, ret, 0,
			    "Error creating statistics thread.");
			goto err;
		}
		stat_created = 1;
	}
	if (cfg.checkpoint_interval != 0) {
		if ((ret = pthread_create(
		    &checkpoint, NULL, checkpoint_worker, &cfg)) != 0) {
			lprintf(&cfg, ret, 0,
			    "Error creating checkpoint thread.");
			goto err;
		}
		checkpoint_created = 1;
	}
	if (cfg.create != 0 && execute_populate(&cfg) != 0)
		goto err;
	/* If we aren't populating, set the insert count. */
	if (cfg.create == 0 && find_table_count(&cfg) != 0)
		goto err;

	if (cfg.run_time != 0 &&
	    cfg.read_threads + cfg.insert_threads + cfg.update_threads != 0 &&
	    (ret = execute_workload(&cfg)) != 0)
		goto err;

	lprintf(&cfg, 0, 1,
	    "Ran performance test example with %d read threads, %d insert"
	    " threads and %d update threads for %d seconds.",
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

err:	g_util_running = 0;

	if (parse_session != NULL)
		parse_session->close(parse_session, NULL);
	if (checkpoint_created != 0 &&
	    (ret = pthread_join(checkpoint, NULL)) != 0)
		lprintf(&cfg, ret, 0, "Error joining checkpoint thread.");
	if (stat_created != 0 && (ret = pthread_join(stat, NULL)) != 0)
		lprintf(&cfg, ret, 0, "Error joining stat thread.");
	if (conn != NULL && (ret = conn->close(conn, NULL)) != 0)
		lprintf(&cfg, ret, 0,
		    "Error closing connection to %s", cfg.home);
	if (cc_buf != NULL)
		free(cc_buf);
	if (tc_buf != NULL)
		free(tc_buf);
	if (cfg.logf != NULL) {
		fflush(cfg.logf);
		fclose(cfg.logf);
	}
	config_free(&cfg);

	return (ret);
}

/*
 * Following are utility functions.
 */

/* Assign the src config to the dest.
 * Any storage allocated in dest is freed as a result.
 */
void config_assign(CONFIG *dest, const CONFIG *src)
{
	size_t i, len;
	const char *saved_home;
	char *newstr, **pstr;

	saved_home = dest->home;
	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE) {
			pstr = (char **)
			    ((unsigned char *)dest + config_opts[i].offset);
			if (*pstr != NULL) {
				len = strlen(*pstr) + 1;
				newstr = malloc(len);
				strncpy(newstr, *pstr, len);
				*pstr = newstr;
			}
		}
	dest->home = saved_home;
}

/* Free any storage allocated in the config struct.
 */
void
config_free(CONFIG *cfg)
{
	size_t i;
	char **pstr;

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE) {
			pstr = (char **)
			    ((unsigned char *)cfg + config_opts[i].offset);
			if (*pstr != NULL) {
				free(*pstr);
				*pstr = NULL;
			}
		}
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
	size_t i, nopt;
	char *newstr, **strp;
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
	if (popt->type == UINT32_TYPE) {
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad int value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		} else if (v->val < 0 || v->val > UINT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "uint32 value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(uint32_t *)valueloc = (uint32_t)v->val;
	} else if (popt->type == STRING_TYPE) {
		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		if (*strp != NULL)
			free(*strp);
		newstr = malloc(v->len + 1);
		strncpy(newstr, v->str, v->len);
		newstr[v->len] = '\0';
		*strp = newstr;
	} else if (popt->type == BOOL_TYPE || popt->type == FLAG_TYPE) {
		uint32_t *pconfigval;

		if (v->type != WT_CONFIG_ITEM_BOOL) {
			fprintf(stderr, "wtperf: Error: "
			    "bad bool value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		pconfigval = (uint32_t *)valueloc;
		if (popt->type == BOOL_TYPE)
			*pconfigval = (uint32_t)v->val;
		else if (v->val != 0)
			*pconfigval |= popt->flagmask;
		else
			*pconfigval &= ~popt->flagmask;
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
		return errno;
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

	optstr = malloc(strlen(name) + strlen(value) + 4);  /* name="value" */
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

	optstr = malloc(strlen(name) + strlen(value) + 2);  /* name=value */
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
	printf("String values must be enclosed by \" quotes,\n");
	printf("bool values must be true or false.\n\n");

	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++) {
		typestr = "?";
		defaultval = config_opts[i].defaultval;
		if (config_opts[i].type == UINT32_TYPE)
			typestr = "int";
		else if (config_opts[i].type == STRING_TYPE)
			typestr = "string";
		else if (config_opts[i].type == BOOL_TYPE ||
		    config_opts[i].type == FLAG_TYPE) {
			typestr = "bool";
			if (strcmp(defaultval, "0") == 0)
				defaultval = "true";
			else
				defaultval = "false";
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
    CONFIG *cfg, u_int num, pthread_t **threadsp, void *(*func)(void *))
{
	pthread_t *threads;
	u_int i;
	int ret;

	g_running = 1;
	g_npop_ops = g_nread_ops = g_nupdate_ops = 0;
	g_threads_quit = 0;
	threads = calloc(num, sizeof(pthread_t *));
	if (threads == NULL)
		return (ENOMEM);
	for (i = 0; i < num; i++)
		if ((ret = pthread_create(
		    &threads[i], NULL, func, cfg)) != 0) {
			g_running = 0;
			lprintf(cfg, ret, 0, "Error creating thread: %d", i);
			return (ret);
		}
	*threadsp = threads;
	return (0);
}

int
stop_threads(CONFIG *cfg, u_int num, pthread_t *threads)
{
	u_int i;
	int ret;

	g_running = 0;

	for (i = 0; i < num; i++)
		if ((ret = pthread_join(threads[i], NULL)) != 0) {
			lprintf(cfg, ret, 0, "Error joining thread %d", i);
			return (ret);
		}

	free(threads);
	return (0);
}

/*
 * Log printf - output a log message.
 */
int
lprintf(CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
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
		return (0);

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

	return (0);
}

/* Setup the logging output mechanism. */
int setup_log_file(CONFIG *cfg)
{
	char *fname;
	int offset;

	if (cfg->verbose < 1 && cfg->stat_interval == 0)
		return (0);

	if ((fname = calloc(strlen(cfg->home) +
	    strlen(cfg->uri) + strlen(".stat") + 1, 1)) == NULL) {
		fprintf(stderr, "No memory in stat thread\n");
		return (ENOMEM);
	}
	for (offset = 0;
	    cfg->uri[offset] != 0 && cfg->uri[offset] != ':';
	    offset++) {}
	if (cfg->uri[offset] == 0)
		offset = 0;
	else
		++offset;
	sprintf(fname, "%s/%s.stat", cfg->home, cfg->uri + offset);
	if ((cfg->logf = fopen(fname, "w")) == NULL) {
		fprintf(stderr, "Statistics failed to open log file.\n");
		return (EINVAL);
	}
	/* Use line buffering for the log file. */
	(void)setvbuf(cfg->logf, NULL, _IOLBF, 0);
	if (fname != NULL)
		free(fname);
	return (0);
}

void wtperf_srand(CONFIG *cfg) {
	srand(cfg->rand_seed);
}

uint64_t wtperf_value_range(CONFIG *cfg) {
	if (F_ISSET(cfg, PERF_RAND_WORKLOAD))
		return (cfg->icount + cfg->rand_range);
	else 
		return (cfg->icount + g_nins_ops - (cfg->insert_threads + 1));
}

uint64_t wtperf_rand(CONFIG *cfg) {
	double S1, S2, U;
	uint64_t rval = (uint64_t)rand();
	/* Use Pareto distribution to give 80/20 hot/cold values. */
	if (F_ISSET(cfg, PERF_RAND_PARETO)) {
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
	return rval;
}

void indent_lines(const char *lines, const char *indent)
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

void print_config(CONFIG *cfg)
{
	printf("Workload configuration:\n");
	printf("\t home: %s\n", cfg->home);
	printf("\t uri: %s\n", cfg->uri);
	printf("\t Connection configuration: %s\n", cfg->conn_config);
	printf("\t Table configuration: %s\n", cfg->table_config);
	printf("\t %s\n", cfg->create ? "Creating" : "Using existing");
	printf("\t Checkpoint interval: %d\n", cfg->checkpoint_interval);
	printf("\t Random seed: %d\n", cfg->rand_seed);
	if (cfg->create) {
		printf("\t Insert count: %d\n", cfg->icount);
		printf("\t Number populate threads: %d\n",
		    cfg->populate_threads);
	}
	printf("\t key size: %d data size: %d\n", cfg->key_sz, cfg->data_sz);
	printf("\t Reporting interval: %d\n", cfg->report_interval);
	printf("\t Workload period: %d\n", cfg->run_time);
	printf("\t Number read threads: %d\n", cfg->read_threads);
	printf("\t Number insert threads: %d\n", cfg->insert_threads);
	if (F_ISSET(cfg, PERF_INSERT_RMW))
		printf("\t Insert operations are RMW.\n");
	printf("\t Number update threads: %d\n", cfg->update_threads);
	printf("\t Verbosity: %d\n", cfg->verbose);
}

void usage(void)
{
	printf("wtperf [-CLMOSThov]\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\t-O <filename> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\n");
	config_opt_usage();
}
