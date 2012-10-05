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
	uint32_t read_threads;	/* Number of read threads. */
	uint32_t verbose;
	WT_CONNECTION *conn;
} CONFIG;

/* Forward function definitions. */
int populate(CONFIG *);
void print_config(CONFIG *cfg);
void *read_thread(void *arg);
void usage(void);

/* Default values - these are small, we want the basic run to be fast. */
CONFIG default_cfg = {
	"WT_TEST",	/* home */
	"lsm:test",	/* uri */
	"create,cache_size=2GB", /* conn_config */
	"key_format=S,value_format=S,lsm_chunk_size=20MB,"
	    "leaf_page_max=16k,internal_page_max=16kb", /* table_config */
	1,		/* create */
	14023954,	/* rand_seed */
	500000,		/* icount */
	100,		/* data_sz */
	20,		/* key_sz */
	20,		/* report_interval */
	10,		/* read_time */
	16,		/* read_threads */
	0,		/* verbose */
	NULL
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

int populate(CONFIG *cfg)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	char *data_buf, *key_buf;
	int ret;
	uint32_t i;

	conn = cfg->conn;

	data_buf = calloc(cfg->data_sz, 1);
	if (data_buf == NULL)
		return (ENOMEM);
	key_buf = calloc(cfg->key_sz, 1);
	if (key_buf == NULL)
		return (ENOMEM);

	/* Open a session for the current thread's work. */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		fprintf(stderr, "Error opening a session on %s: %s\n",
		    cfg->home, wiredtiger_strerror(ret));

	ret = session->create(session, cfg->uri, cfg->table_config);

	ret = session->open_cursor(
	    session, cfg->uri, NULL, "bulk", &cursor);

	memset(data_buf, 'a', cfg->data_sz - 1);
	cursor->set_value(cursor, data_buf);
	/* Populate the database. */
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
			return (1);
		}
	}
	cursor->close(cursor);
	session->close(session, NULL);
	if (cfg->verbose > 0)
		printf("Finished bulk load of %d items\n", cfg->icount);

	free(data_buf);
	free(key_buf);
	return (ret);
}

int main(int argc, char **argv)
{
	CONFIG cfg;
	WT_CONNECTION *conn;
	const char *user_cconfig, *user_tconfig;
	char *cc_buf, *tc_buf;
	int ch, debug, ret;
	pthread_t *read_threads;
	uint32_t i, slept;
	uint64_t last_ops, req_len;

	return (0);
	/* Setup the default configuration values. */
	memcpy(&cfg, &default_cfg, sizeof(cfg));
	debug = 0;
	cc_buf = tc_buf = NULL;
	while ((ch = getopt(argc, argv, "C:DR:T:d:eh:i:k:r:s:u:v:")) != EOF)
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
		case 'D':
			debug = 1;
			break;
		case 'R':
			cfg.read_threads = atoi(optarg);
			break;
		case 'T':
			user_tconfig = optarg;
			break;
		case '?':
		default:
			fprintf(stderr, "Invalid option\n");
			usage();
			return (EINVAL);
		}

	/* Handle non-const configuration strings. */
	if (debug || user_cconfig) {
		req_len = strlen(cfg.conn_config) + strlen(debug_cconfig) +
			strlen(user_cconfig);
		cc_buf = calloc(req_len, 1);
		if (cc_buf == NULL)
			exit(ENOMEM);
		snprintf(cc_buf, req_len, "%s%s%s%s%s",
		    cfg.conn_config,
		    debug ? "," : "", debug ? debug_cconfig : "",
		    user_cconfig ? "," : "", user_cconfig ? user_cconfig : "");
		cfg.conn_config = cc_buf;
	}
	if (debug || user_tconfig) {
		req_len = strlen(cfg.table_config) + strlen(debug_tconfig) +
			strlen(user_tconfig);
		tc_buf = calloc(req_len, 1);
		if (tc_buf == NULL)
			exit(ENOMEM);
		snprintf(tc_buf, req_len, "%s%s%s%s%s",
		    cfg.table_config,
		    debug ? "," : "", debug ? debug_tconfig : "",
		    user_tconfig ? "," : "", user_tconfig ? user_tconfig : "");
		cfg.table_config = tc_buf;
	}

	srand(cfg.rand_seed);

	if (cfg.verbose > 1)
		print_config(&cfg);

	/* Open a connection to the database, creating it if necessary. */
	if ((ret = wiredtiger_open(
	    cfg.home, NULL, cfg.conn_config, &conn)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    cfg.home, wiredtiger_strerror(ret));

	cfg.conn = conn;
	if (cfg.create)
		populate(&cfg);

	if (cfg.verbose > 0)
		printf("Starting read threads\n");
	running = 1;
	nops = 0;
	read_threads = calloc(cfg.read_threads, sizeof(pthread_t *));
	if (read_threads == NULL)
		exit(ENOMEM);
	for (i = 0; i < cfg.read_threads; i++)
		ret = pthread_create(&read_threads[i], NULL, read_thread, &cfg);

	if (cfg.report_interval > cfg.read_time)
		cfg.report_interval = cfg.read_time;
	for (slept = 0, last_ops = 0; slept < cfg.read_time;
	    slept += cfg.report_interval) {
		sleep(cfg.report_interval);
		if (cfg.verbose > 0) {
			printf("%" PRIu64 " ops in %d secs\n",
			    nops - last_ops, cfg.report_interval);
			fflush(stdout);
		}
		last_ops = nops;
	}
	running = 0;

	for (i = 0; i < cfg.read_threads; i++)
		ret = pthread_join(read_threads[i], NULL);

	/* Note: closing the connection implicitly closes open session(s). */
	if ((ret = conn->close(conn, NULL)) != 0)
		fprintf(stderr, "Error connecting to %s: %s\n",
		    cfg.home, wiredtiger_strerror(ret));

	printf("Ran performance test example with %d threads for %d seconds.\n",
	    cfg.read_threads, cfg.read_time);
	printf("Executed %" PRIu64 " read operations\n", nops);
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
		printf("\tInsert count: %d\n", cfg->icount);
	printf("\t key size: %d data size: %d\n", cfg->key_sz, cfg->data_sz);
	printf("\t Reporting interval: %d\n", cfg->report_interval);
	printf("\t Read workload period: %d\n", cfg->read_time);
	printf("\t Number read threads: %d\n", cfg->read_threads);
	printf("\t Verbosity: %d\n", cfg->verbose);
}

void usage(void)
{
	printf("ex_perf_test [-CDRTdehikrsuv]\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t-D debug configuration\n");
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
