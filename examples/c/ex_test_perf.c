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
	uint32_t read_threads;	/* Number of read threads. */
	uint32_t verbose;
	uint32_t stat_thread;	/* Whether to create a stat thread. */
	WT_CONNECTION *conn;
	FILE *logf;
} CONFIG;

/* Forward function definitions. */
int execute_reads(CONFIG *);
int populate(CONFIG *);
void print_config(CONFIG *);
void *read_thread(void *);
int setup_log_file(CONFIG *);
void *stat_worker(void *);
void usage(void);

/* Default values - these are tiny, we want the basic run to be fast. */
CONFIG default_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=200MB", /* conn_config */
	"key_format=S,value_format=S",	/* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	5000,		/* icount */
	100,		/* data_sz */
	20,		/* key_sz */
	2,		/* report_interval */
	2,		/* read_time */
	0,		/* elapsed_time */
	2,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL		/* logf */
};
/* Small config values - these are small. */
CONFIG small_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=500MB", /* conn_config */
	"key_format=S,value_format=S,lsm_chunk_size=5MB,"
	    "leaf_page_max=16k,internal_page_max=16kb", /* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	500000,		/* icount 0.5 million */
	100,		/* data_sz */
	20,		/* key_sz */
	10,		/* report_interval */
	20,		/* read_time */
	0,		/* elapsed_time */
	8,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL		/* logf */
};
/* Default values - these are small, we want the basic run to be fast. */
CONFIG med_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=1GB", /* conn_config */
	"key_format=S,value_format=S,lsm_chunk_size=20MB,"
	    "leaf_page_max=16k,internal_page_max=16kb", /* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	50000000,	/* icount 50 million */
	100,		/* data_sz */
	20,		/* key_sz */
	20,		/* report_interval */
	100,		/* read_time */
	0,		/* elapsed_time */
	16,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL		/* logf */
};
/* Default values - these are small, we want the basic run to be fast. */
CONFIG large_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=2GB", /* conn_config */
	"key_format=S,value_format=S,lsm_chunk_size=50MB,"
	    "leaf_page_max=16k,internal_page_max=16kb", /* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	500000000,	/* icount 500 million */
	100,		/* data_sz */
	20,		/* key_sz */
	20,		/* report_interval */
	600,		/* read_time */
	0,		/* elapsed_time */
	16,		/* read_threads */
	0,		/* verbose */
	0,		/* stat_thread */
	NULL,		/* conn */
	NULL		/* logf */
};

const char *debug_cconfig = "verbose=[lsm]";
const char *debug_tconfig = "";

/* Global values shared by threads. */
uint64_t nops;
int running;

void *
read_thread(void *arg)
{
	CONFIG *cfg;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	char *key_buf;
	int ret;

	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	key_buf = calloc(cfg->key_sz, 1);
	if (key_buf == NULL)
		return (arg);

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr,
		    "open_session failed in read thread: %d\n", ret);
		return (NULL);
	}
	if ((ret = session->open_cursor(session, cfg->uri,
	    NULL, NULL, &cursor)) != 0) {
		fprintf(stderr, "open_cursor failed in read thread: %d\n", ret);
		return (NULL);
	}

	while (running) {
		++nops;
		sprintf(key_buf, "%d", rand() % cfg->icount);
		cursor->set_key(cursor, key_buf);
		cursor->search(cursor);
	}
	session->close(session, NULL);
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
	int ret;
	uint64_t value;

	cfg = (CONFIG *)arg;
	conn = cfg->conn;
	lsm_uri = NULL;

	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr,
		    "open_session failed in read thread: %d\n", ret);
		return (NULL);
	}

	if (strncmp(cfg->uri, "lsm:", strlen("lsm:")) == 0) {
		lsm_uri = calloc(strlen(cfg->uri) + strlen("statistics:"), 1);
		if (lsm_uri == NULL) {
			fprintf(stderr, "No memory in stat thread.\n");
			goto err;
		}
		sprintf(lsm_uri, "statistics:%s", cfg->uri);
	}

	while (running) {
		sleep(cfg->report_interval);
		/* Generic header. */
		fprintf(cfg->logf, "=======================================\n");
		fprintf(cfg->logf,
		    "reads completed: %" PRIu64", elapsed time: ~%d\n",
		    nops, cfg->elapsed_time);
		/* Report LSM tree stats, if using LSM. */
		if (lsm_uri != NULL) {
			if ((ret = session->open_cursor(session, lsm_uri,
			    NULL, NULL, &cursor)) != 0) {
				fprintf(stderr,
				    "open_cursor LSM statistics: %d\n", ret);
				goto err;
			}
			while (
			    (ret = cursor->next(cursor)) == 0 &&
			    (ret = cursor->get_value(
			    cursor, &desc, &pvalue, &value)) == 0)
				fprintf(cfg->logf,
				    "stat:lsm:%s=%s\n", desc, pvalue);
			cursor->close(cursor);
		}
	}
err:	session->close(session, NULL);
	if (lsm_uri != NULL)
		free(lsm_uri);
	return (arg);
}

int populate(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *data_buf, *key_buf;
	double secs;
	int ret;
	struct timeval e, s;
	uint32_t i;

	conn = cfg->conn;

	data_buf = calloc(cfg->data_sz, 1);
	if (data_buf == NULL)
		return (ENOMEM);
	key_buf = calloc(cfg->key_sz, 1);
	if (key_buf == NULL)
		return (ENOMEM);

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    cfg->home, wiredtiger_strerror(ret));
		return (ret);
	}

	if ((ret = session->create(
	    session, cfg->uri, cfg->table_config)) != 0) {
		fprintf(stderr, "Error creating table %s: %s\n",
		    cfg->uri, wiredtiger_strerror(ret));
		return (ret);
	}

	if ((ret = session->open_cursor(
	    session, cfg->uri, NULL, "bulk", &cursor)) != 0) {
		fprintf(stderr, "Error opening cursor %s: %s\n",
		    cfg->uri, wiredtiger_strerror(ret));
		return (ret);
	}

	memset(data_buf, 'a', cfg->data_sz - 1);
	cursor->set_value(cursor, data_buf);
	/* Populate the database. */
	gettimeofday(&s, NULL);
	for (i = 0; i < cfg->icount; i++) {
		if (cfg->verbose > 0) {
			if (i % 1000000 == 0)
				printf(".");
			if (i % 50000000 == 0)
				printf("\n");
		}
		sprintf(key_buf, "%d", i);
		cursor->set_key(cursor, key_buf);
		if ((ret = cursor->insert(cursor)) != 0) {
			fprintf(stderr, "Failed inserting with: %d\n", ret);
			return (ret);
		}
	}
	gettimeofday(&e, NULL);
	cursor->close(cursor);
	session->close(session, NULL);
	if (cfg->verbose > 0) {
		fprintf(cfg->logf,
		    "Finished bulk load of %d items\n", cfg->icount);
		secs = e.tv_sec + e.tv_usec / 1000000.0;
		secs -= (s.tv_sec + s.tv_usec / 1000000.0);
		if (secs == 0)
			++secs;
		fprintf(cfg->logf,
		    "Load time: %.2f\n" "load ops/sec: %.2f\n",
		    secs, cfg->icount / secs);
	}

	free(data_buf);
	free(key_buf);
	return (ret);
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
	/* Turn off buffering for the log file. */
	(void)setvbuf(cfg->logf, NULL, _IONBF, 0);
	if (fname != NULL)
		free(fname);
	return (0);
}

int main(int argc, char **argv)
{
	CONFIG cfg;
	WT_CONNECTION *conn;
	const char *user_cconfig, *user_tconfig;
	const char *opts = "C:R:T:d:eh:i:k:lr:s:u:v:SML";
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
			cfg.icount = atoi(optarg) * 1000;
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
		fprintf(stderr, "Error connecting to %s: %s\n",
		    cfg.home, wiredtiger_strerror(ret));
		goto err;
	}

	cfg.conn = conn;
	if (cfg.create != 0 && (ret = populate(&cfg)) != 0)
		goto err;

	if (cfg.stat_thread) {
		if ((ret = pthread_create(
		    &stat, NULL, stat_worker, &cfg)) != 0) {
			fprintf(stderr, "Error creating statistics thread.\n");
			goto err;
		}
		stat_created = 1;
	}

	if (cfg.read_time != 0 && cfg.read_threads != 0)
		if ((ret = execute_reads(&cfg)) != 0)
			goto err;

	if (cfg.verbose > 0) {
		fprintf(cfg.logf,
	    "Ran performance test example with %d threads for %d seconds.\n",
		    cfg.read_threads, cfg.read_time);
		fprintf(cfg.logf,
		    "Executed %" PRIu64 " read operations\n", nops);
	}

	/* Cleanup. */
err:	if (stat_created != 0 && (ret = pthread_join(stat, NULL)) != 0)
		fprintf(stderr, "Error joining stat thread: %d.\n", ret);
	if (conn != NULL && (ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    cfg.home, wiredtiger_strerror(ret));
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

int execute_reads(CONFIG *cfg)
{
	pthread_t *read_threads;
	int ret;
	uint32_t i;
	uint64_t last_ops;

	if (cfg->verbose > 0)
		fprintf(cfg->logf, "Starting read threads\n");

	running = 1;
	nops = 0;

	read_threads = calloc(cfg->read_threads, sizeof(pthread_t *));
	if (read_threads == NULL)
		return (ENOMEM);
	for (i = 0; i < cfg->read_threads; i++) {
		if ((ret = pthread_create(
		    &read_threads[i], NULL, read_thread, cfg)) != 0) {
			fprintf(stderr, "Error creating thread: %d\n", i);
			return (ret);
		}
	}

	/* Sanity check reporting interval. */
	if (cfg->report_interval > cfg->read_time)
		cfg->report_interval = cfg->read_time;

	for (cfg->elapsed_time = 0, last_ops = 0;
	    cfg->elapsed_time < cfg->read_time;
	    cfg->elapsed_time += cfg->report_interval) {
		sleep(cfg->report_interval);
		if (cfg->verbose > 0) {
			fprintf(cfg->logf, "%" PRIu64 " ops in %d secs\n",
			    nops - last_ops, cfg->report_interval);
		}
		last_ops = nops;
	}
	running = 0;

	for (i = 0; i < cfg->read_threads; i++) {
		if ((ret = pthread_join(read_threads[i], NULL)) != 0) {
			fprintf(stderr, "Error joining thread %d\n", i);
			return (ret);
		}
	}

	if (read_threads != NULL)
		free(read_threads);
	return (ret);
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
	if (cfg->create)
		printf("\t Insert count: %d\n", cfg->icount);
	printf("\t key size: %d data size: %d\n", cfg->key_sz, cfg->data_sz);
	printf("\t Reporting interval: %d\n", cfg->report_interval);
	printf("\t Read workload period: %d\n", cfg->read_time);
	printf("\t Number read threads: %d\n", cfg->read_threads);
	printf("\t Verbosity: %d\n", cfg->verbose);
}

void usage(void)
{
	printf("ex_perf_test [-CDLMRSTdehikrsuv]\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t-R <int> number of read threads\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t-d <int> data item size\n");
	printf("\t-e use existing database (skip population phase)\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST \n");
	printf("\t-i <int> number of records to insert in thousands\n");
	printf("\t-k <int> key item size\n");
	printf("\t-r <int> number of seconds to run read phase\n");
	printf("\t-s <int> seed for random number generator\n");
	printf("\t-u <string> table uri, default lsm:test\n");
	printf("\t-v <int> verbosity\n");
}
