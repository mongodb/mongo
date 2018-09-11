/*-
 * Public Domain 2014-2018 MongoDB, Inc.
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

static char home[1024];			/* Program working dir */

/*
 * These two names for the URI and file system must be maintained in tandem.
 */
static const char * const uri = "table:main";
static bool compat;
static bool inmem;

#define	MAX_TH	12
#define	MIN_TH	5
#define	MAX_TIME	40
#define	MIN_TIME	10
#define	RECORDS_FILE	"records-%" PRIu32

#define	ENV_CONFIG_COMPAT	",compatibility=(release=\"2.9\")"
#define	ENV_CONFIG_DEF						\
    "create,log=(file_max=10M,enabled)"
#define	ENV_CONFIG_TXNSYNC					\
    "create,log=(file_max=10M,enabled),"		\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_REC "log=(recover=on)"
#define	MAX_VAL	4096

static void handler(int)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
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

static WT_THREAD_RET
thread_run(void *arg)
{
	FILE *fp;
	WT_CURSOR *cursor;
	WT_ITEM data;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	uint64_t i;
	size_t lsize;
	char buf[MAX_VAL], kname[64], lgbuf[8];
	char large[128*1024];

	__wt_random_init(&rnd);
	memset(buf, 0, sizeof(buf));
	memset(kname, 0, sizeof(kname));
	lsize = sizeof(large);
	memset(large, 0, lsize);

	td = (WT_THREAD_DATA *)arg;
	/*
	 * The value is the name of the record file with our id appended.
	 */
	testutil_check(__wt_snprintf(buf, sizeof(buf), RECORDS_FILE, td->id));
	/*
	 * Set up a large value putting our id in it.  Write it in there a
	 * bunch of times, but the rest of the buffer can just be zero.
	 */
	testutil_check(__wt_snprintf(
	    lgbuf, sizeof(lgbuf), "th-%" PRIu32, td->id));
	for (i = 0; i < 128; i += strlen(lgbuf))
		testutil_check(__wt_snprintf(
		    &large[i], lsize - i, "%s", lgbuf));
	/*
	 * Keep a separate file with the records we wrote for checking.
	 */
	(void)unlink(buf);
	if ((fp = fopen(buf, "w")) == NULL)
		testutil_die(errno, "fopen");
	/*
	 * Set to line buffering.  But that is advisory only.  We've seen
	 * cases where the result files end up with partial lines.
	 */
	__wt_stream_set_line_buffer(fp);
	testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
	testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
	data.data = buf;
	data.size = sizeof(buf);
	/*
	 * Write our portion of the key space until we're killed.
	 */
	printf("Thread %" PRIu32 " starts at %" PRIu64 "\n",
	    td->id, td->start);
	for (i = td->start; ; ++i) {
		testutil_check(__wt_snprintf(
		    kname, sizeof(kname), "%" PRIu64, i));
		cursor->set_key(cursor, kname);
		/*
		 * Every 30th record write a very large record that exceeds the
		 * log buffer size.  This forces us to use the unbuffered path.
		 */
		if (i % 30 == 0) {
			data.size = 128 * 1024;
			data.data = large;
		} else {
			data.size = __wt_random(&rnd) % MAX_VAL;
			data.data = buf;
		}
		cursor->set_value(cursor, &data);
		testutil_check(cursor->insert(cursor));
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
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	wt_thread_t *thr;
	uint32_t i;
	char envconf[512];

	thr = dcalloc(nth, sizeof(*thr));
	td = dcalloc(nth, sizeof(WT_THREAD_DATA));
	if (chdir(home) != 0)
		testutil_die(errno, "Child chdir: %s", home);
	if (inmem)
		strcpy(envconf, ENV_CONFIG_DEF);
	else
		strcpy(envconf, ENV_CONFIG_TXNSYNC);
	if (compat)
		strcat(envconf, ENV_CONFIG_COMPAT);

	testutil_check(wiredtiger_open(NULL, NULL, envconf, &conn));
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(session->create(
	    session, uri, "key_format=S,value_format=u"));
	testutil_check(session->close(session, NULL));

	printf("Create %" PRIu32 " writer threads\n", nth);
	for (i = 0; i < nth; ++i) {
		td[i].conn = conn;
		td[i].start = WT_BILLION * (uint64_t)i;
		td[i].id = i;
		testutil_check(__wt_thread_create(
		    NULL, &thr[i], thread_run, &td[i]));
	}
	printf("Spawned %" PRIu32 " writer threads\n", nth);
	fflush(stdout);
	/*
	 * The threads never exit, so the child will just wait here until
	 * it is killed.
	 */
	for (i = 0; i < nth; ++i)
		testutil_check(__wt_thread_join(NULL, &thr[i]));
	/*
	 * NOTREACHED
	 */
	free(thr);
	free(td);
	exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

static void
handler(int sig)
{
	pid_t pid;

	WT_UNUSED(sig);
	pid = wait(NULL);
	/*
	 * The core file will indicate why the child exited. Choose EINVAL here.
	 */
	testutil_die(EINVAL,
	    "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

int
main(int argc, char *argv[])
{
	struct sigaction sa;
	struct stat sb;
	FILE *fp;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	pid_t pid;
	uint64_t absent, count, key, last_key, middle;
	uint32_t i, nth, timeout;
	int ch, status, ret;
	const char *working_dir;
	char buf[1024], fname[64], kname[64];
	bool fatal, rand_th, rand_time, verify_only;

	(void)testutil_set_progname(argv);

	compat = inmem = false;
	nth = MIN_TH;
	rand_th = rand_time = true;
	timeout = MIN_TIME;
	verify_only = false;
	working_dir = "WT_TEST.random-abort";

	while ((ch = __wt_getopt(progname, argc, argv, "Ch:mT:t:v")) != EOF)
		switch (ch) {
		case 'C':
			compat = true;
			break;
		case 'h':
			working_dir = __wt_optarg;
			break;
		case 'm':
			inmem = true;
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
	if (argc != 0)
		usage();

	testutil_work_dir_from_path(home, sizeof(home), working_dir);
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

		__wt_random_init_seed(NULL, &rnd);
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
		printf("Parent: Compatibility %s in-mem log %s\n",
		    compat ? "true" : "false", inmem ? "true" : "false");
		printf("Parent: Create %" PRIu32
		    " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
		printf("CONFIG: %s%s%s -h %s -T %" PRIu32 " -t %" PRIu32 "\n",
		    progname,
		    compat ? " -C" : "",
		    inmem ? " -m" : "",
		    working_dir, nth, timeout);
		/*
		 * Fork a child to insert as many items.  We will then randomly
		 * kill the child, run recovery and make sure all items we wrote
		 * exist after recovery runs.
		 */
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = handler;
		testutil_checksys(sigaction(SIGCHLD, &sa, NULL));
		if ((pid = fork()) < 0)
			testutil_die(errno, "fork");

		if (pid == 0) { /* child */
			fill_db(nth);
			return (EXIT_SUCCESS);
		}

		/* parent */
		/*
		 * Sleep for the configured amount of time before killing
		 * the child.  Start the timeout from the time we notice that
		 * the child workers have created their record files. That
		 * allows the test to run correctly on really slow machines.
		 */
		i = 0;
		while (i < nth) {
			/*
			 * Wait for each record file to exist.
			 */
			testutil_check(__wt_snprintf(
			    fname, sizeof(fname), RECORDS_FILE, i));
			testutil_check(__wt_snprintf(
			    buf, sizeof(buf),"%s/%s", home, fname));
			while (stat(buf, &sb) != 0)
				sleep(1);
			++i;
		}
		sleep(timeout);
		sa.sa_handler = SIG_DFL;
		testutil_checksys(sigaction(SIGCHLD, &sa, NULL));

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

	testutil_check(__wt_snprintf(buf, sizeof(buf),
	    "rm -rf ../%s.SAVE; mkdir ../%s.SAVE; "
	    "cp -p WiredTigerLog.* ../%s.SAVE;",
	    home, home, home));
	if ((status = system(buf)) < 0)
		testutil_die(status, "system: %s", buf);

	printf("Open database, run recovery and verify content\n");
	testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn));
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

	absent = count = 0;
	fatal = false;
	for (i = 0; i < nth; ++i) {
		middle = 0;
		testutil_check(__wt_snprintf(
		    fname, sizeof(fname), RECORDS_FILE, i));
		if ((fp = fopen(fname, "r")) == NULL)
			testutil_die(errno, "fopen: %s", fname);

		/*
		 * For every key in the saved file, verify that the key exists
		 * in the table after recovery.  If we're doing in-memory
		 * log buffering we never expect a record missing in the middle,
		 * but records may be missing at the end.  If we did
		 * write-no-sync, we expect every key to have been recovered.
		 */
		for (last_key = UINT64_MAX;; ++count, last_key = key) {
			ret = fscanf(fp, "%" SCNu64 "\n", &key);
			/*
			 * Consider anything other than clear success in
			 * getting the key to be EOF. We've seen file system
			 * issues where the file ends with zeroes on a 4K
			 * boundary and does not return EOF but a ret of zero.
			 */
			if (ret != 1)
				break;
			/*
			 * If we're unlucky, the last line may be a partially
			 * written key at the end that can result in a false
			 * negative error for a missing record.  Detect it.
			 */
			if (last_key != UINT64_MAX && key != last_key + 1) {
				printf("%s: Ignore partial record %" PRIu64
				    " last valid key %" PRIu64 "\n",
				    fname, key, last_key);
				break;
			}
			testutil_check(__wt_snprintf(
			    kname, sizeof(kname), "%" PRIu64, key));
			cursor->set_key(cursor, kname);
			if ((ret = cursor->search(cursor)) != 0) {
				if (ret != WT_NOTFOUND)
					testutil_die(ret, "search");
				if (!inmem)
					printf("%s: no record with key %"
					    PRIu64 "\n", fname, key);
				absent++;
				middle = key;
			} else if (middle != 0) {
				/*
				 * We should never find an existing key after
				 * we have detected one missing.
				 */
				printf("%s: after absent record at %" PRIu64
				    " key %" PRIu64 " exists\n",
				    fname, middle, key);
				fatal = true;
			}
		}
		if (fclose(fp) != 0)
			testutil_die(errno, "fclose");
	}
	testutil_check(conn->close(conn, NULL));
	if (fatal)
		return (EXIT_FAILURE);
	if (!inmem && absent) {
		printf("%" PRIu64 " record(s) absent from %" PRIu64 "\n",
		    absent, count);
		return (EXIT_FAILURE);
	}
	printf("%" PRIu64 " records verified\n", count);
	return (EXIT_SUCCESS);
}
