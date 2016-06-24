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

#include "test_util.h"

#include <sys/wait.h>
#include <signal.h>

static char home[512];			/* Program working dir */
static const char *progname;		/* Program name */
static const char * const uri = "table:main";

#define	MAX_TH	12
#define	MIN_TH	5
#define	MAX_TIME	40
#define	MIN_TIME	10
#define	RECORDS_FILE	"records-%" PRIu32

#define	ENV_CONFIG						\
    "create,log=(file_max=10M,archive=false,enabled),"		\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_REC "log=(recover=on)"
#define	MAX_VAL	4096

static void usage(void)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
	fprintf(stderr, "usage: %s [-h dir] [-T threads]\n", progname);
	exit(EXIT_FAILURE);
}

typedef struct {
	WT_CONNECTION *conn;
	uint64_t start;
	uint32_t id;
} WT_THREAD_DATA;

static void *
thread_run(void *arg)
{
	FILE *fp;
	WT_CURSOR *cursor;
	WT_ITEM data;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	uint64_t i;
	int ret;
	char buf[MAX_VAL], kname[64];

	__wt_random_init(&rnd);
	memset(buf, 0, sizeof(buf));
	memset(kname, 0, sizeof(kname));

	td = (WT_THREAD_DATA *)arg;
	/*
	 * The value is the name of the record file with our id appended.
	 */
	snprintf(buf, sizeof(buf), RECORDS_FILE, td->id);
	/*
	 * Keep a separate file with the records we wrote for checking.
	 */
	(void)unlink(buf);
	if ((fp = fopen(buf, "w")) == NULL)
		testutil_die(errno, "fopen");
	/*
	 * Set to no buffering.
	 */
	__wt_stream_set_line_buffer(fp);
	if ((ret = td->conn->open_session(td->conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);
	data.data = buf;
	data.size = sizeof(buf);
	/*
	 * Write our portion of the key space until we're killed.
	 */
	for (i = td->start; ; ++i) {
		snprintf(kname, sizeof(kname), "%" PRIu64, i);
		data.size = __wt_random(&rnd) % MAX_VAL;
		cursor->set_key(cursor, kname);
		cursor->set_value(cursor, &data);
		if ((ret = cursor->insert(cursor)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");
		/*
		 * Save the key separately for checking later.
		 */
		if (fprintf(fp, "%" PRIu64 "\n", i) == -1)
			testutil_die(errno, "fprintf");
	}
	/* NOTREACHED */
}

/*
 * Child process creates the database and table, and then creates worker
 * threads to add data until it is killed by the parent.
 */
static void fill_db(uint32_t)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
fill_db(uint32_t nth)
{
	pthread_t *thr;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	uint32_t i;
	int ret;

	thr = dcalloc(nth, sizeof(pthread_t));
	td = dcalloc(nth, sizeof(WT_THREAD_DATA));
	if (chdir(home) != 0)
		testutil_die(errno, "Child chdir: %s", home);
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret = session->create(session,
	    uri, "key_format=S,value_format=u")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri);
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "WT_SESSION:close");

	printf("Create %" PRIu32 " writer threads\n", nth);
	for (i = 0; i < nth; ++i) {
		td[i].conn = conn;
		td[i].start = (UINT64_MAX / nth) * i;
		td[i].id = i;
		if ((ret = pthread_create(
		    &thr[i], NULL, thread_run, &td[i])) != 0)
			testutil_die(ret, "pthread_create");
	}
	printf("Spawned %" PRIu32 " writer threads\n", nth);
	fflush(stdout);
	/*
	 * The threads never exit, so the child will just wait here until
	 * it is killed.
	 */
	for (i = 0; i < nth; ++i)
		testutil_assert(pthread_join(thr[i], NULL) == 0);
	/*
	 * NOTREACHED
	 */
	free(thr);
	free(td);
	exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

void (*custom_die)(void) = NULL;

int
main(int argc, char *argv[])
{
	FILE *fp;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	WT_RAND_STATE rnd;
	uint64_t key;
	uint32_t absent, count, i, nth, timeout;
	int ch, status, ret;
	pid_t pid;
	bool rand_th, rand_time, verify_only;
	const char *working_dir;
	char fname[64], kname[64];

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	nth = MIN_TH;
	rand_th = rand_time = true;
	timeout = MIN_TIME;
	verify_only = false;
	working_dir = "WT_TEST.random-abort";

	while ((ch = __wt_getopt(progname, argc, argv, "h:T:t:v")) != EOF)
		switch (ch) {
		case 'h':
			working_dir = __wt_optarg;
			break;
		case 'T':
			rand_th = false;
			nth = (uint32_t)atoi(__wt_optarg);
			break;
		case 't':
			rand_time = false;
			timeout = (uint32_t)atoi(__wt_optarg);
			break;
		case 'v':
			verify_only = true;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		usage();

	testutil_work_dir_from_path(home, 512, working_dir);
	/*
	 * If the user wants to verify they need to tell us how many threads
	 * there were so we can find the old record files.
	 */
	if (verify_only && rand_th) {
		fprintf(stderr,
		    "Verify option requires specifying number of threads\n");
		exit (EXIT_FAILURE);
	}
	if (!verify_only) {
		testutil_make_work_dir(home);

		testutil_assert(__wt_random_init_seed(NULL, &rnd) == 0);
		if (rand_time) {
			timeout = __wt_random(&rnd) % MAX_TIME;
			if (timeout < MIN_TIME)
				timeout = MIN_TIME;
		}
		if (rand_th) {
			nth = __wt_random(&rnd) % MAX_TH;
			if (nth < MIN_TH)
				nth = MIN_TH;
		}
		printf("Parent: Create %" PRIu32
		    " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
		/*
		 * Fork a child to insert as many items.  We will then randomly
		 * kill the child, run recovery and make sure all items we wrote
		 * exist after recovery runs.
		 */
		if ((pid = fork()) < 0)
			testutil_die(errno, "fork");

		if (pid == 0) { /* child */
			fill_db(nth);
			return (EXIT_SUCCESS);
		}

		/* parent */
		/*
		 * Sleep for the configured amount of time before killing
		 * the child.
		 */
		sleep(timeout);

		/*
		 * !!! It should be plenty long enough to make sure more than
		 * one log file exists.  If wanted, that check would be added
		 * here.
		 */
		printf("Kill child\n");
		if (kill(pid, SIGKILL) != 0)
			testutil_die(errno, "kill");
		if (waitpid(pid, &status, 0) == -1)
			testutil_die(errno, "waitpid");
	}
	/*
	 * !!! If we wanted to take a copy of the directory before recovery,
	 * this is the place to do it.
	 */
	if (chdir(home) != 0)
		testutil_die(errno, "parent chdir: %s", home);
	printf("Open database, run recovery and verify content\n");
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	absent = count = 0;
	for (i = 0; i < nth; ++i) {
		snprintf(fname, sizeof(fname), RECORDS_FILE, i);
		if ((fp = fopen(fname, "r")) == NULL) {
			fprintf(stderr,
			    "Failed to open %s. i %" PRIu32 "\n", fname, i);
			testutil_die(errno, "fopen");
		}

		/*
		 * For every key in the saved file, verify that the key exists
		 * in the table after recovery.  Since we did write-no-sync, we
		 * expect every key to have been recovered.
		 */
		for (;; ++count) {
			ret = fscanf(fp, "%" SCNu64 "\n", &key);
			if (ret != EOF && ret != 1)
				testutil_die(errno, "fscanf");
			if (ret == EOF)
				break;
			snprintf(kname, sizeof(kname), "%" PRIu64, key);
			cursor->set_key(cursor, kname);
			if ((ret = cursor->search(cursor)) != 0) {
				if (ret != WT_NOTFOUND)
					testutil_die(ret, "search");
				printf("%s: no record with key %" PRIu64 "\n",
				    fname, key);
				++absent;
			}
		}
		if (fclose(fp) != 0)
			testutil_die(errno, "fclose");
	}
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if (absent) {
		printf("%" PRIu32 " record(s) absent from %" PRIu32 "\n",
		    absent, count);
		return (EXIT_FAILURE);
	}
	printf("%" PRIu32 " records verified\n", count);
	return (EXIT_SUCCESS);
}
