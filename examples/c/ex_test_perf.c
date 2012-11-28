/*-
 * Public Domain 2008-2012 WiredTiger, Inc.
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
 * ex_test_perf.c
 *	This is an application that executes parallel random read workload.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>
#include <unistd.h>

#include <wiredtiger.h>

#define	ATOMIC_ADD(v, val)						\
	__sync_add_and_fetch(&(v), val)

typedef struct {
	const char *home;
	const char *uri;
	const char *conn_config;
	const char *table_config;
	uint32_t create;	/* Whether to populate for this run. */
	uint32_t rand_seed;
	uint32_t icount;	/* Items to insert. */
	uint32_t data_sz;
	uint32_t key_sz;
	uint32_t report_interval;
	uint32_t read_time;
	uint32_t elapsed_time;
	uint32_t populate_threads;/* Number of populate threads. */
	uint32_t read_threads;	/* Number of read threads. */
	uint32_t verbose;
	uint32_t stat_thread;	/* Whether to create a stat thread. */
	WT_CONNECTION *conn;
	FILE *logf;
#define	LSM_TEST_PERF_INIT	0x00
#define	LSM_TEST_PERF_POP	0x01
#define	LSM_TEST_PERF_READ	0x02
	uint32_t phase;
	struct timeval phase_start_time;
} CONFIG;

/* Forward function definitions. */
int execute_populate(CONFIG *);
int execute_reads(CONFIG *);
int get_next_op(uint64_t *);
int lprintf(CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format (printf, 4, 5)))
#endif
;
void *populate_thread(void *);
void print_config(CONFIG *);
void *read_thread(void *);
int setup_log_file(CONFIG *);
int start_threads(CONFIG *, int, pthread_t **, void *(*func)(void *));
void *stat_worker(void *);
int stop_threads(CONFIG *, int, pthread_t *);
void usage(void);

#define	DEFAULT_LSM_CONFIG						\
	"key_format=S,value_format=S,exclusive,"			\
	"leaf_page_max=4kb,internal_page_max=64kb,allocation_size=4kb,"

/* Default values - these are tiny, we want the basic run to be fast. */
CONFIG default_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=200MB", /* conn_config */
	DEFAULT_LSM_CONFIG, /* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	5000,		/* icount */
	100,		/* data_sz */
	20,		/* key_sz */
	2,		/* report_interval */
	2,		/* read_time */
	0,		/* elapsed_time */
	1,		/* populate_threads */
	2,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL,		/* logf */
	LSM_TEST_PERF_INIT, /* phase */
	{0, 0}		/* phase_start_time */
};
/* Small config values - these are small. */
CONFIG small_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=500MB", /* conn_config */
	DEFAULT_LSM_CONFIG /* table_config */
	    "lsm_chunk_size=5MB,",
	1,		/* create */
	14023954,	/* rand_seed */
	500000,		/* icount 0.5 million */
	100,		/* data_sz */
	20,		/* key_sz */
	10,		/* report_interval */
	20,		/* read_time */
	0,		/* elapsed_time */
	1,		/* populate_threads */
	8,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL,		/* logf */
	LSM_TEST_PERF_INIT, /* phase */
	{0, 0}		/* phase_start_time */
};
/* Default values - these are small, we want the basic run to be fast. */
CONFIG med_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=1GB", /* conn_config */
	DEFAULT_LSM_CONFIG /* table_config */
	    "lsm_chunk_size=20MB,",
	1,		/* create */
	14023954,	/* rand_seed */
	50000000,	/* icount 50 million */
	100,		/* data_sz */
	20,		/* key_sz */
	20,		/* report_interval */
	100,		/* read_time */
	0,		/* elapsed_time */
	1,		/* populate_threads */
	16,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL,		/* logf */
	LSM_TEST_PERF_INIT, /* phase */
	{0, 0}		/* phase_start_time */
};
/* Default values - these are small, we want the basic run to be fast. */
CONFIG large_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=2GB", /* conn_config */
	DEFAULT_LSM_CONFIG /* table_config */
	    "lsm_chunk_size=50MB,",
	1,		/* create */
	14023954,	/* rand_seed */
	500000000,	/* icount 500 million */
	100,		/* data_sz */
	20,		/* key_sz */
	20,		/* report_interval */
	600,		/* read_time */
	0,		/* elapsed_time */
	1,		/* populate_threads */
	16,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL,		/* logf */
	LSM_TEST_PERF_INIT, /* phase */
	{0, 0}		/* phase_start_time */
};

const char *debug_cconfig = "verbose=[lsm]";
const char *debug_tconfig = "";

/* Global values shared by threads. */
uint64_t g_nops;
int g_running;
int g_stat_running;
uint32_t g_threads_quit; /* For tracking threads that exit early. */

void *
read_thread(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	char *key_buf;
	int ret, search_ret;

	session = NULL;
	key_buf = NULL;

	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	key_buf = calloc(cfg->key_sz, 1);
	if (key_buf == NULL) {
		ret = ENOMEM;
		goto err;
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
		++g_nops;
		/* Get a value in range, avoid zero. */
		sprintf(key_buf, "%d", (rand() % (cfg->icount - 1)) + 1);
		cursor->set_key(cursor, key_buf);
		/* Report errors and continue. */
		if ((search_ret = cursor->search(cursor)) != 0)
			lprintf(cfg, search_ret, 0,
			    "Search failed for: %s", key_buf);
	}

err:	if (ret != 0)
		++g_threads_quit;
	if (session != NULL)
		session->close(session, NULL);
	if (key_buf != NULL)
		free(key_buf);
	return (arg);
}

/* Retrieve an ID for the next insert operation. */
int get_next_op(uint64_t *op)
{
	*op = ATOMIC_ADD(g_nops, 1);
	return (0);
}

void *
populate_thread(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *data_buf, *key_buf;
	int ret;
	uint64_t op;

	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	session = NULL;
	data_buf = key_buf = NULL;

	cfg->phase = LSM_TEST_PERF_POP;

	data_buf = calloc(cfg->data_sz, 1);
	if (data_buf == NULL) {
		lprintf(cfg, ENOMEM, 0, "Populate data buffer");
		goto err;
	}
	key_buf = calloc(cfg->key_sz, 1);
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

	/* Can only use bulk load single threaded. */
	if ((ret = session->open_cursor(
	    session, cfg->uri, NULL,
	    cfg->populate_threads == 1 ? "bulk" : "", &cursor)) != 0) {
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
		sprintf(key_buf, "%"PRIu64, op);
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
	WT_SESSION *session;
	WT_CURSOR *cursor;
	const char *desc, *pvalue;
	char *lsm_uri;
	double secs;
	int ret;
	struct timeval e;
	uint64_t value;

	session = NULL;
	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	lsm_uri = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0,
		    "open_session failed in statistics thread.");
		goto err;
	}

	if (strncmp(cfg->uri, "lsm:", strlen("lsm:")) == 0) {
		lsm_uri = calloc(
		    strlen(cfg->uri) + strlen("statistics:") + 1, 1);
		if (lsm_uri == NULL) {
			lprintf(
			    cfg, ENOMEM, 0, "Statistics thread uri create.");
			goto err;
		}
		sprintf(lsm_uri, "statistics:%s", cfg->uri);
	}

	while (g_stat_running) {
		sleep(cfg->report_interval);
		/* Generic header. */
		lprintf(cfg, 0, cfg->verbose,
		    "=======================================");
		gettimeofday(&e, NULL);
		secs = e.tv_sec + e.tv_usec / 1000000.0;
		secs -= (cfg->phase_start_time.tv_sec +
		    cfg->phase_start_time.tv_usec / 1000000.0);
		if (secs == 0)
			++secs;
		lprintf(cfg, 0, cfg->verbose,
		    "%s completed: %" PRIu64", elapsed time: %.2f",
		    cfg->phase == LSM_TEST_PERF_READ ? "reads" : "inserts",
		    g_nops, secs);
		/* Report LSM tree stats, if using LSM. */
		if (lsm_uri != NULL) {
			if ((ret = session->open_cursor(session, lsm_uri,
			    NULL, NULL, &cursor)) != 0) {
				lprintf(cfg, ret, 0,
				    "open_cursor failed in LSM statistics");
				goto err;
			}
			while (
			    (ret = cursor->next(cursor)) == 0 &&
			    (ret = cursor->get_value(
			    cursor, &desc, &pvalue, &value)) == 0)
				lprintf(cfg, 0, cfg->verbose,
				    "stat:lsm: %s=%s", desc, pvalue);
			cursor->close(cursor);
			lprintf(cfg, 0, cfg->verbose, "-----------------");
		}

		/* Dump the connection statistics since last time. */
		if ((ret = session->open_cursor(session, "statistics:",
		    NULL, "statistics_clear", &cursor)) != 0) {
			lprintf(cfg, ret, 0,
			    "open_cursor failed in statistics");
			goto err;
		}
		while (
		    (ret = cursor->next(cursor)) == 0 &&
		    (ret = cursor->get_value(
		    cursor, &desc, &pvalue, &value)) == 0)
			lprintf(cfg, 0, cfg->verbose,
			    "stat:conn: %s=%s", desc, pvalue);
		cursor->close(cursor);

	}
err:	if (session != NULL)
		session->close(session, NULL);
	if (lsm_uri != NULL)
		free(lsm_uri);
	return (arg);
}

int execute_populate(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	pthread_t *threads;
	double secs;
	int ret;
	uint64_t elapsed, last_ops;
	struct timeval e;

	conn = cfg->conn;
	cfg->phase = LSM_TEST_PERF_POP;
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
	    g_nops < cfg->icount && g_threads_quit < cfg->populate_threads;) {
		/*
		 * Sleep for 100th of a second, report_interval is in second
		 * granularity, so adjust accordingly.
		 */
		usleep(10000);
		elapsed += 1;
		if (elapsed % 100 == 0 &&
		    (elapsed / 100) % cfg->report_interval == 0) {
			lprintf(cfg, 0, 1, "%" PRIu64 " ops in %d secs",
			    g_nops - last_ops, cfg->report_interval);
			last_ops = g_nops;
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
	    "Finished bulk load of %d items", cfg->icount);
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

int execute_reads(CONFIG *cfg)
{
	pthread_t *threads;
	int ret;
	uint64_t last_ops;

	cfg->phase = LSM_TEST_PERF_READ;
	lprintf(cfg, 0, 1, "Starting read threads");

	if ((ret = start_threads(
	    cfg, cfg->read_threads, &threads, read_thread)) != 0)
		return (ret);

	/* Sanity check reporting interval. */
	if (cfg->report_interval > cfg->read_time)
		cfg->report_interval = cfg->read_time;

	gettimeofday(&cfg->phase_start_time, NULL);
	for (cfg->elapsed_time = 0, last_ops = 0;
	    cfg->elapsed_time < cfg->read_time &&
	    g_threads_quit < cfg->read_threads;
	    cfg->elapsed_time += cfg->report_interval) {
		sleep(cfg->report_interval);
		lprintf(cfg, 0, 1, "%" PRIu64 " ops in %d secs",
		    g_nops - last_ops, cfg->report_interval);
		last_ops = g_nops;
	}
	if (g_threads_quit == cfg->populate_threads) {
		lprintf(cfg, WT_ERROR, 0,
		    "Populate threads exited without finishing.");
		return (WT_ERROR);
	}

	if ((ret = stop_threads(cfg, cfg->read_threads, threads)) != 0)
		return (ret);

	return (0);
}

int main(int argc, char **argv)
{
	CONFIG cfg;
	WT_CONNECTION *conn;
	const char *user_cconfig, *user_tconfig;
	const char *opts = "C:P:R:T:d:eh:i:k:lr:s:u:v:SML";
	char *cc_buf, *tc_buf;
	int ch, ret, stat_created;
	pthread_t stat;
	uint64_t req_len;

	/* Setup the default configuration values. */
	memcpy(&cfg, &default_cfg, sizeof(cfg));
	cc_buf = tc_buf = NULL;
	user_cconfig = user_tconfig = NULL;
	conn = NULL;
	stat_created = 0;

	/*
	 * First parse different config structures - other options override
	 * fields within the structure.
	 */
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'S':
			memcpy(&cfg, &small_cfg, sizeof(cfg));
			break;
		case 'M':
			memcpy(&cfg, &med_cfg, sizeof(cfg));
			break;
		case 'L':
			memcpy(&cfg, &large_cfg, sizeof(cfg));
			break;
		default:
			/* Validation is provided on the next parse. */
			break;
		}

	/* Parse other options */
	optind = 1;
	while ((ch = getopt(argc, argv, opts)) != EOF)
		switch (ch) {
		case 'd':
			cfg.data_sz = atoi(optarg);
			break;
		case 'e':
			cfg.create = 0;
			break;
		case 'h':
			cfg.home = optarg;
			break;
		case 'i':
			cfg.icount = atoi(optarg);
			break;
		case 'k':
			cfg.key_sz = atoi(optarg);
			break;
		case 'l':
			cfg.stat_thread = 1;
			break;
		case 'r':
			cfg.read_time = atoi(optarg);
			break;
		case 's':
			cfg.rand_seed = atoi(optarg);
			break;
		case 'u':
			cfg.uri = optarg;
			break;
		case 'v':
			cfg.verbose = atoi(optarg);
			break;
		case 'C':
			user_cconfig = optarg;
			break;
		case 'P':
			cfg.populate_threads = atoi(optarg);
			break;
		case 'R':
			cfg.read_threads = atoi(optarg);
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		case 'L':
		case 'M':
		case 'S':
			break;
		case '?':
		default:
			fprintf(stderr, "Invalid option\n");
			usage();
			return (EINVAL);
		}

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
		cfg.conn_config = cc_buf;
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
		cfg.table_config = tc_buf;
	}

	srand(cfg.rand_seed);

	if (cfg.verbose > 1)
		print_config(&cfg);

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(
	    cfg.home, NULL, cfg.conn_config, &conn)) != 0) {
		lprintf(&cfg, ret, 0, "Error connecting to %s", cfg.home);
		goto err;
	}

	cfg.conn = conn;

	if (cfg.stat_thread) {
		g_stat_running = 1;
		if ((ret = pthread_create(
		    &stat, NULL, stat_worker, &cfg)) != 0) {
			lprintf(&cfg, ret, 0,
			    "Error creating statistics thread.");
			goto err;
		}
		stat_created = 1;
	}
	if (cfg.create != 0 && execute_populate(&cfg) != 0)
		goto err;

	if (cfg.read_time != 0 && cfg.read_threads != 0 &&
	    (ret = execute_reads(&cfg)) != 0)
			goto err;

	lprintf(&cfg, 0, 1,
	    "Ran performance test example with %d threads for %d seconds.",
	    cfg.read_threads, cfg.read_time);
	lprintf(&cfg, 0, 1,
	    "Executed %" PRIu64 " read operations", g_nops);

err:	if (cfg.stat_thread)
		g_stat_running = 0;

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

	return (ret);
}

/*
 * Following are utility functions.
 */
int
start_threads(CONFIG *cfg, int num, pthread_t **threadsp, void *(*func)(void *))
{
	pthread_t *threads;
	int i, ret;

	g_running = 1;
	g_nops = 0;
	g_threads_quit = 0;
	threads = calloc(num, sizeof(pthread_t *));
	if (threads == NULL)
		return (ENOMEM);
	for (i = 0; i < num; i++) {
		if ((ret = pthread_create(
		    &threads[i], NULL, func, cfg)) != 0) {
			g_running = 0;
			lprintf(cfg, ret, 0, "Error creating thread: %d", i);
			return (ret);
		}
	}
	*threadsp = threads;
	return (0);
}

int
stop_threads(CONFIG *cfg, int num, pthread_t *threads)
{
	int i, ret;
	g_running = 0;

	for (i = 0; i < num; i++) {
		if ((ret = pthread_join(threads[i], NULL)) != 0) {
			lprintf(cfg, ret, 0, "Error joining thread %d", i);
			return (ret);
		}
	}

	free(threads);
	return (0);
}

/*
 * Log printf - output a log message.
 */
int lprintf(CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
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
	vfprintf(cfg->logf, fmt, ap);
	va_end(ap);
	fprintf(cfg->logf, " Error: %s\n", wiredtiger_strerror(err));
	if (cfg->logf != stderr) {
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fprintf(stderr, " Error: %s\n", wiredtiger_strerror(err));
	}

	return (0);
}

/* Setup the logging output mechanism. */
int setup_log_file(CONFIG *cfg)
{
	char *fname;
	int offset;

	if (cfg->verbose < 1 && cfg->stat_thread == 0)
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

void print_config(CONFIG *cfg)
{
	printf("Workload configuration:\n");
	printf("\t home: %s\n", cfg->home);
	printf("\t uri: %s\n", cfg->uri);
	printf("\t Connection configuration: %s\n", cfg->conn_config);
	printf("\t Table configuration: %s\n", cfg->table_config);
	printf("\t %s\n", cfg->create ? "Creating" : "Using existing");
	printf("\t Random seed: %d\n", cfg->rand_seed);
	if (cfg->create) {
		printf("\t Insert count: %d\n", cfg->icount);
		printf("\t Number populate threads: %d\n",
		    cfg->populate_threads);
	}
	printf("\t key size: %d data size: %d\n", cfg->key_sz, cfg->data_sz);
	printf("\t Reporting interval: %d\n", cfg->report_interval);
	printf("\t Read workload period: %d\n", cfg->read_time);
	printf("\t Number read threads: %d\n", cfg->read_threads);
	printf("\t Verbosity: %d\n", cfg->verbose);
}

void usage(void)
{
	printf("ex_perf_test [-CLMPRSTdehikrsuv]\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t-P <int> number of populate threads\n");
	printf("\t-R <int> number of read threads\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t-d <int> data item size\n");
	printf("\t-e use existing database (skip population phase)\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST \n");
	printf("\t-i <int> number of records to insert\n");
	printf("\t-k <int> key item size\n");
	printf("\t-r <int> number of seconds to run read phase\n");
	printf("\t-s <int> seed for random number generator\n");
	printf("\t-u <string> table uri, default lsm:test\n");
	printf("\t-v <int> verbosity\n");
}
