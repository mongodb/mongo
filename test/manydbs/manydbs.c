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

#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <wiredtiger.h>

#include "test_util.i"

#define	HOME_SIZE	512
#define	HOME_BASE	"WT_HOME"
static char home[HOME_SIZE];		/* Base home directory */
static char hometmp[HOME_SIZE];		/* Each conn home directory */
static const char *progname;		/* Program name */
static const char * const uri = "table:main";

#define	WTOPEN_CFG_COMMON					\
    "create,log=(file_max=10M,archive=false,enabled),"		\
    "statistics=(fast),statistics_log=(wait=5),"
#define	WT_CONFIG0						\
    WTOPEN_CFG_COMMON						\
    "transaction_sync=(enabled=false)"
#define	WT_CONFIG1						\
    WTOPEN_CFG_COMMON						\
    "transaction_sync=(enabled,method=none)"
#define	WT_CONFIG2						\
    WTOPEN_CFG_COMMON						\
    "transaction_sync=(enabled,method=fsync)"

#define	MAX_DBS		10
#define	MAX_IDLE_TIME	30
#define	IDLE_INCR	5

#define	MAX_KV		100
#define	MAX_VAL		128

static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-I] [-C maxcpu%%] [-D maxdbs] [-h dir]\n", progname);
	exit(EXIT_FAILURE);
}

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

WT_CONNECTION **conn = NULL;
WT_CURSOR **cursor = NULL;
WT_RAND_STATE rnd;
WT_SESSION **session = NULL;

static int
run_ops(int dbs)
{
	WT_ITEM data;
	int db_set, i, key;
	uint32_t db;
	uint8_t buf[MAX_VAL];

	memset(buf, 0, sizeof(buf));
	/*
	 * First time through, set up sessions, create the tables and
	 * open cursors.
	 */
	if (session == NULL) {
		__wt_random_init(&rnd);
		if ((session =
		    calloc((size_t)dbs, sizeof(WT_SESSION *))) == NULL)
			testutil_die(ENOMEM, "session array malloc");
		if ((cursor = calloc((size_t)dbs, sizeof(WT_CURSOR *))) == NULL)
			testutil_die(ENOMEM, "cursor array malloc");
		for (i = 0; i < dbs; ++i) {
			testutil_check(conn[i]->open_session(conn[i],
			    NULL, NULL, &session[i]));
			testutil_check(session[i]->create(session[i],
			    uri, "key_format=Q,value_format=u"));
			testutil_check(session[i]->open_cursor(session[i],
			    uri, NULL, NULL, &cursor[i]));
		}
	}
	for (i = 0; i < MAX_VAL; ++i)
		buf[i] = (uint8_t)__wt_random(&rnd);
	data.data = buf;
	/*
	 * Write a small amount of data into a random subset of the databases.
	 */
	db_set = dbs / 4;
	for (i = 0; i < db_set; ++i) {
		db = __wt_random(&rnd) % (uint32_t)dbs;
		printf("Write to database %" PRIu32 "\n", db);
		for (key = 0; key < MAX_KV; ++key) {
			data.size = __wt_random(&rnd) % MAX_VAL;
			cursor[db]->set_key(cursor[db], key);
			cursor[db]->set_value(cursor[db], &data);
			testutil_check(cursor[db]->insert(cursor[db]));
		}
	}
	return (0);
}

int
main(int argc, char *argv[])
{
	FILE *fp;
	float cpu, max;
	int cfg, ch, dbs, i;
	const char *working_dir;
	bool idle, setmax;
	const char *wt_cfg;
	char cmd[128];

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;
	dbs = MAX_DBS;
	working_dir = HOME_BASE;
	max = (float)dbs;
	idle = setmax = false;
	while ((ch = __wt_getopt(progname, argc, argv, "C:D:h:I")) != EOF)
		switch (ch) {
		case 'C':
			max = (float)atof(__wt_optarg);
			setmax = true;
			break;
		case 'D':
			dbs = atoi(__wt_optarg);
			break;
		case 'h':
			working_dir = __wt_optarg;
			break;
		case 'I':
			idle = true;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
	/*
	 * Adjust the maxcpu in relation to the number of databases, unless
	 * the user set it explicitly.
	 */
	if (!setmax)
		max = (float)dbs;
	if (argc != 0)
		usage();

	if ((conn = calloc((size_t)dbs, sizeof(WT_CONNECTION *))) == NULL)
		testutil_die(ENOMEM, "connection array malloc");
	memset(cmd, 0, sizeof(cmd));
	/*
	 * Set up all the directory names.
	 */
	testutil_work_dir_from_path(home, HOME_SIZE, working_dir);
	testutil_make_work_dir(home);
	for (i = 0; i < dbs; ++i) {
		snprintf(hometmp, HOME_SIZE, "%s/%s.%d", home, HOME_BASE, i);
		testutil_make_work_dir(hometmp);
		/*
		 * Open each database.  Rotate different configurations
		 * among them.
		 */
		cfg = i % 3;
		if (cfg == 0)
			wt_cfg = WT_CONFIG0;
		else if (cfg == 1)
			wt_cfg = WT_CONFIG1;
		else
			wt_cfg = WT_CONFIG2;
		testutil_check(wiredtiger_open(
		    hometmp, NULL, wt_cfg, &conn[i]));
	}

	sleep(10);
	for (i = 0; i < MAX_IDLE_TIME; i += IDLE_INCR) {
		if (!idle)
			testutil_check(run_ops(dbs));
		printf("Sleep %d (%d of %d)\n", IDLE_INCR, i, MAX_IDLE_TIME);
		sleep(IDLE_INCR);
	}

	/*
	 * Check CPU after all idling or work is done.
	 */
	(void)snprintf(cmd, sizeof(cmd),
	    "ps -p %lu -o pcpu=", (unsigned long)getpid());
	if ((fp = popen(cmd, "r")) == NULL)
		testutil_die(errno, "popen");
	fscanf(fp, "%f", &cpu);
	printf("Final CPU %f, max %f\n", cpu, max);
	if (cpu > max) {
		fprintf(stderr, "ERROR: CPU usage: %f, max %f\n", cpu, max);
		testutil_die(ERANGE, "CPU");
	}
	if (pclose(fp) != 0)
		testutil_die(errno, "pclose");
	for (i = 0; i < dbs; ++i)
		testutil_check(conn[i]->close(conn[i], NULL));

	return (EXIT_SUCCESS);
}
