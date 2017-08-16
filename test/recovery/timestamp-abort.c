/*-
 * Public Domain 2014-2017 MongoDB, Inc.
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
 * Create three tables that we will write the same data to and verify that
 * all the types of usage have the expected data in them after a crash and
 * recovery.  We want:
 * 1. A table that is logged and is not involved in timestamps.  This table
 * simulates a user local table.
 * 2. A table that is logged and involved in timestamps.  This simulates
 * the oplog.
 * 3. A table that is not logged and involved in timestamps.  This simulates
 * a typical collection file.
 *
 * We also create a fourth table that is not logged and not involved directly
 * in timestamps to store the stable timestamp.  That way we can know what the
 * latest stable timestamp is on checkpoint.
 *
 * We also create several files that are not WiredTiger tables.  The checkpoint
 * thread creates a file indicating that a checkpoint has completed.  The parent
 * process uses this to know when at least one checkpoint is done and it can
 * start the timer to abort.
 *
 * Each worker thread creates its own records file that records the data it
 * inserted and it records the timestamp that was used for that insertion.
 */
static const char * const uri_local = "table:local";
static const char * const uri_oplog = "table:oplog";
static const char * const uri_collection = "table:collection";

static const char * const stable_store = "table:stable";
static const char * const ckpt_file = "checkpoint_done";
static bool compat, inmem, use_ts;
static uint64_t global_ts = 1;

#define	MAX_TH		12
#define	MAX_TIME	40
#define	MIN_TH		5
#define	MIN_TIME	10
#define	RECORDS_FILE	"records-%" PRIu32
#define	STABLE_PERIOD	100

#define	ENV_CONFIG_COMPAT	",compatibility=(release=\"2.9\")"
#define	ENV_CONFIG_DEF						\
    "create,log=(archive=false,file_max=10M,enabled)"
#define	ENV_CONFIG_TXNSYNC					\
    "create,log=(archive=false,file_max=10M,enabled),"			\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_REC "log=(archive=false,recover=on)"

#define	MAX_CKPT_INTERVAL 5	/* Maximum interval between checkpoints */
#define	MAX_VAL	1024

static void usage(void)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-h dir] [-T threads] [-t time] [-Cmvz]\n", progname);
	exit(EXIT_FAILURE);
}

typedef struct {
	WT_CONNECTION *conn;
	uint64_t start;
	uint32_t id;
} WT_THREAD_DATA;

/*
 * thread_ckpt_run --
 *	Runner function for the checkpoint thread.
 */
static WT_THREAD_RET
thread_ckpt_run(void *arg)
{
	FILE *fp;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	uint64_t ts;
	uint32_t sleep_time;
	int i, ret;
	bool first_ckpt;

	__wt_random_init(&rnd);

	td = (WT_THREAD_DATA *)arg;
	/*
	 * Keep a separate file with the records we wrote for checking.
	 */
	(void)unlink(ckpt_file);
	if ((ret = td->conn->open_session(td->conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	first_ckpt = true;
	ts = 0;
	for (i = 0; ;++i) {
		sleep_time = __wt_random(&rnd) % MAX_CKPT_INTERVAL;
		sleep(sleep_time);
		if (use_ts)
			ts = global_ts;
		/*
		 * Since this is the default, send in this string even if
		 * running without timestamps.
		 */
		testutil_check(session->checkpoint(
		    session, "use_timestamp=true"));
		printf("Checkpoint %d complete.  Minimum ts %" PRIu64 "\n",
		    i, ts);
		fflush(stdout);
		/*
		 * Create the checkpoint file so that the parent process knows
		 * at least one checkpoint has finished and can start its
		 * timer.
		 */
		if (first_ckpt) {
			testutil_checksys((fp = fopen(ckpt_file, "w")) == NULL);
			first_ckpt = false;
			testutil_checksys(fclose(fp) != 0);
		}
	}
	/* NOTREACHED */
}

/*
 * thread_run --
 *	Runner function for the worker threads.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
	FILE *fp;
	WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_stable;
	WT_ITEM data;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	uint64_t i, stable_ts;
	int ret;
	char cbuf[MAX_VAL], lbuf[MAX_VAL], obuf[MAX_VAL];
	char kname[64], tscfg[64];

	__wt_random_init(&rnd);
	memset(cbuf, 0, sizeof(cbuf));
	memset(lbuf, 0, sizeof(lbuf));
	memset(obuf, 0, sizeof(obuf));
	memset(kname, 0, sizeof(kname));

	td = (WT_THREAD_DATA *)arg;
	/*
	 * Set up the separate file for checking.
	 */
	testutil_check(__wt_snprintf(cbuf, sizeof(cbuf), RECORDS_FILE, td->id));
	(void)unlink(cbuf);
	testutil_checksys((fp = fopen(cbuf, "w")) == NULL);
	/*
	 * Set to line buffering.  But that is advisory only.  We've seen
	 * cases where the result files end up with partial lines.
	 */
	__wt_stream_set_line_buffer(fp);
	if ((ret = td->conn->open_session(td->conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	/*
	 * Open a cursor to each table.
	 */
	if ((ret = session->open_cursor(session,
	    uri_collection, NULL, NULL, &cur_coll)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_collection);
	if ((ret = session->open_cursor(session,
	    uri_local, NULL, NULL, &cur_local)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_local);
	if ((ret = session->open_cursor(session,
	    uri_oplog, NULL, NULL, &cur_oplog)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_oplog);

	if ((ret = session->open_cursor(
	    session, stable_store, NULL, NULL, &cur_stable)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", stable_store);

	/*
	 * Write our portion of the key space until we're killed.
	 */
	printf("Thread %" PRIu32 " starts at %" PRIu64 "\n", td->id, td->start);
	for (i = td->start; ; ++i) {
		if (use_ts)
			stable_ts = global_ts++;
		else
			stable_ts = 0;
		testutil_check(__wt_snprintf(
		    kname, sizeof(kname), "%" PRIu64, i));

		testutil_check(session->begin_transaction(session, NULL));
		cur_coll->set_key(cur_coll, kname);
		cur_local->set_key(cur_local, kname);
		cur_oplog->set_key(cur_oplog, kname);
		/*
		 * Put an informative string into the value so that it
		 * can be viewed well in a binary dump.
		 */
		testutil_check(__wt_snprintf(cbuf, sizeof(cbuf),
		    "COLL: thread:%" PRIu64 " ts:%" PRIu64 " key: %" PRIu64,
		    td->id, stable_ts, i));
		testutil_check(__wt_snprintf(lbuf, sizeof(lbuf),
		    "LOCAL: thread:%" PRIu64 " ts:%" PRIu64 " key: %" PRIu64,
		    td->id, stable_ts, i));
		testutil_check(__wt_snprintf(obuf, sizeof(obuf),
		    "OPLOG: thread:%" PRIu64 " ts:%" PRIu64 " key: %" PRIu64,
		    td->id, stable_ts, i));
		data.size = __wt_random(&rnd) % MAX_VAL;
		data.data = cbuf;
		cur_coll->set_value(cur_coll, &data);
		if ((ret = cur_coll->insert(cur_coll)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");
		data.size = __wt_random(&rnd) % MAX_VAL;
		data.data = obuf;
		cur_oplog->set_value(cur_oplog, &data);
		if ((ret = cur_oplog->insert(cur_oplog)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");
		if (use_ts) {
			testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
			    "commit_timestamp=%" PRIx64, stable_ts));
			testutil_check(
			    session->commit_transaction(session, tscfg));
		} else
			testutil_check(
			    session->commit_transaction(session, NULL));
		/*
		 * Insert into the local table outside the timestamp txn.
		 */
		data.size = __wt_random(&rnd) % MAX_VAL;
		data.data = lbuf;
		cur_local->set_value(cur_local, &data);
		if ((ret = cur_local->insert(cur_local)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");

		/*
		 * Every N records we will record our stable timestamp into the
		 * stable table.  That will define our threshold where we
		 * expect to find records after recovery.
		 */
		if (i % STABLE_PERIOD == 0) {
			if (use_ts) {
				/*
				 * Set both the oldest and stable timestamp
				 * so that we don't need to maintain read
				 * availability at older timestamps.
				 */
				testutil_check(__wt_snprintf(
				    tscfg, sizeof(tscfg),
				    "oldest_timestamp=%" PRIx64
				    ",stable_timestamp=%" PRIx64,
				    stable_ts, stable_ts));
				testutil_check(
				    td->conn->set_timestamp(td->conn, tscfg));
			}
			cur_stable->set_key(cur_stable, td->id);
			cur_stable->set_value(cur_stable, stable_ts);
			testutil_check(cur_stable->insert(cur_stable));
		}
		/*
		 * Save the timestamp and key separately for checking later.
		 */
		if (fprintf(fp,
		    "%" PRIu64 " %" PRIu64 "\n", stable_ts, i) < 0)
			testutil_die(EIO, "fprintf");
	}
	/* NOTREACHED */
}

/*
 * Child process creates the database and table, and then creates worker
 * threads to add data until it is killed by the parent.
 */
static void run_workload(uint32_t)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
run_workload(uint32_t nth)
{
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_THREAD_DATA *td;
	wt_thread_t *thr;
	uint32_t i;
	int ret;
	char envconf[512];

	thr = dcalloc(nth+1, sizeof(*thr));
	td = dcalloc(nth+1, sizeof(WT_THREAD_DATA));
	if (chdir(home) != 0)
		testutil_die(errno, "Child chdir: %s", home);
	if (inmem)
		strcpy(envconf, ENV_CONFIG_DEF);
	else
		strcpy(envconf, ENV_CONFIG_TXNSYNC);
	if (compat)
		strcat(envconf, ENV_CONFIG_COMPAT);

	if ((ret = wiredtiger_open(NULL, NULL, envconf, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	/*
	 * Create all the tables.
	 */
	if ((ret = session->create(session, uri_collection,
		"key_format=S,value_format=u,log=(enabled=false)")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri_collection);
	if ((ret = session->create(session,
	    uri_local, "key_format=S,value_format=u")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri_local);
	if ((ret = session->create(session,
	    uri_oplog, "key_format=S,value_format=u")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri_oplog);
	/*
	 * Don't log the stable timestamp table so that we know what timestamp
	 * was stored at the checkpoint.
	 */
	if ((ret = session->create(session, stable_store,
	    "key_format=Q,value_format=Q,log=(enabled=false)")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", stable_store);
	if ((ret = session->close(session, NULL)) != 0)
		testutil_die(ret, "WT_SESSION:close");

	/*
	 * Thread 0 is the checkpoint thread.
	 */
	td[0].conn = conn;
	td[0].id = 0;
	printf("Create checkpoint thread\n");
	testutil_check(__wt_thread_create(
	    NULL, &thr[0], thread_ckpt_run, &td[0]));
	for (i = 1; i <= nth; ++i) {
		td[i].conn = conn;
		td[i].start = (UINT64_MAX / nth) * (i - 1);
		td[i].id = i;
		testutil_check(__wt_thread_create(
		    NULL, &thr[i], thread_run, &td[i]));
	}
	/*
	 * The threads never exit, so the child will just wait here until
	 * it is killed.
	 */
	printf("Create %" PRIu32 " writer threads\n", nth);
	fflush(stdout);
	for (i = 0; i <= nth; ++i)
		testutil_check(__wt_thread_join(NULL, thr[i]));
	/*
	 * NOTREACHED
	 */
	free(thr);
	free(td);
	exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
	struct stat sb;
	FILE *fp;
	WT_CONNECTION *conn;
	WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_stable;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	pid_t pid;
	uint64_t absent_coll, absent_local, absent_oplog, count, key, last_key;
	uint64_t first_miss, middle_coll, middle_local, middle_oplog;
	uint64_t stable_fp, stable_val, val[MAX_TH+1];
	uint32_t i, nth, timeout;
	int ch, status, ret;
	const char *working_dir;
	char buf[128], fname[64], kname[64], statname[1024];
	bool fatal, rand_th, rand_time, verify_only;

	(void)testutil_set_progname(argv);

	compat = inmem = false;
	use_ts = true;
	nth = MIN_TH;
	rand_th = rand_time = true;
	timeout = MIN_TIME;
	verify_only = false;
	working_dir = "WT_TEST.timestamp-abort";

	while ((ch = __wt_getopt(progname, argc, argv, "Ch:mT:t:vz")) != EOF)
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
		case 'z':
			use_ts = false;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
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
		printf("Parent: compatibility: %s, "
		    "in-mem log sync: %s, timestamp in use: %s\n",
		    compat ? "true" : "false",
		    inmem ? "true" : "false",
		    use_ts ? "true" : "false");
		printf("Parent: Create %" PRIu32
		    " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
		/*
		 * Fork a child to insert as many items.  We will then randomly
		 * kill the child, run recovery and make sure all items we wrote
		 * exist after recovery runs.
		 */
		testutil_checksys((pid = fork()) < 0);

		if (pid == 0) { /* child */
			run_workload(nth);
			return (EXIT_SUCCESS);
		}

		/* parent */
		/*
		 * Sleep for the configured amount of time before killing
		 * the child.  Start the timeout from the time we notice that
		 * the file has been created.  That allows the test to run
		 * correctly on really slow machines.  Verify the process ID
		 * still exists in case the child aborts for some reason we
		 * don't stay in this loop forever.
		 */
		testutil_check(__wt_snprintf(
		    statname, sizeof(statname), "%s/%s", home, ckpt_file));
		while (stat(statname, &sb) != 0 && kill(pid, 0) == 0)
			sleep(1);
		sleep(timeout);

		/*
		 * !!! It should be plenty long enough to make sure more than
		 * one log file exists.  If wanted, that check would be added
		 * here.
		 */
		printf("Kill child\n");
		testutil_checksys(kill(pid, SIGKILL) != 0);
		testutil_checksys(waitpid(pid, &status, 0) == -1);
	}
	/*
	 * !!! If we wanted to take a copy of the directory before recovery,
	 * this is the place to do it.
	 */
	if (chdir(home) != 0)
		testutil_die(errno, "parent chdir: %s", home);
	testutil_check(__wt_snprintf(buf, sizeof(buf),
	    "rm -rf ../%s.SAVE && mkdir ../%s.SAVE && cp -rp * ../%s.SAVE",
	     home, home, home));
	(void)system(buf);
	printf("Open database, run recovery and verify content\n");

	/*
	 * Open the connection which forces recovery to be run.
	 */
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	/*
	 * Open a cursor on all the tables.
	 */
	if ((ret = session->open_cursor(session,
	    uri_collection, NULL, NULL, &cur_coll)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_collection);
	if ((ret = session->open_cursor(session,
	    uri_local, NULL, NULL, &cur_local)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_local);
	if ((ret = session->open_cursor(session,
	    uri_oplog, NULL, NULL, &cur_oplog)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri_oplog);
	if ((ret = session->open_cursor(session,
	    stable_store, NULL, NULL, &cur_stable)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", stable_store);

	/*
	 * Find the biggest stable timestamp value that was saved.
	 */
	stable_val = 0;
	memset(val, 0, sizeof(val));
	while (cur_stable->next(cur_stable) == 0) {
		cur_stable->get_key(cur_stable, &key);
		cur_stable->get_value(cur_stable, &val[key]);
		if (val[key] > stable_val)
			stable_val = val[key];

		if (use_ts)
			printf("Stable: key %" PRIu64 " value %" PRIu64 "\n",
			    key, val[key]);
	}
	if (use_ts)
		printf("Got stable_val %" PRIu64 "\n", stable_val);

	count = 0;
	absent_coll = absent_local = absent_oplog = 0;
	fatal = false;
	for (i = 1; i <= nth; ++i) {
		first_miss = middle_coll = middle_local = middle_oplog = 0;
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
			ret = fscanf(fp, "%" SCNu64 "%" SCNu64 "\n",
			    &stable_fp, &key);
			if (ret != EOF && ret != 2) {
				/*
				 * If we find a partial line, consider it
				 * like an EOF.
				 */
				if (ret == 1 || ret == 0)
					break;
				testutil_die(errno, "fscanf");
			}
			if (ret == EOF)
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
			cur_coll->set_key(cur_coll, kname);
			cur_local->set_key(cur_local, kname);
			cur_oplog->set_key(cur_oplog, kname);
			/*
			 * The collection table should always only have the
			 * data as of the checkpoint.
			 */
			if ((ret = cur_coll->search(cur_coll)) != 0) {
				if (ret != WT_NOTFOUND)
					testutil_die(ret, "search");
				/*
				 * If we don't find a record, the stable
				 * timestamp written to our file better be
				 * larger than the saved one.
				 */
				if (!inmem &&
				    stable_fp != 0 && stable_fp <= val[i]) {
					printf("%s: COLLECTION no record with "
					    "key %" PRIu64 " record ts %" PRIu64
					    " <= stable ts %" PRIu64 "\n",
					    fname, key, stable_fp, val[i]);
					absent_coll++;
				}
				if (middle_coll == 0)
					first_miss = key;
				middle_coll = key;
			} else if (middle_coll != 0) {
				/*
				 * We should never find an existing key after
				 * we have detected one missing.
				 */
				printf("%s: COLLECTION after absent records %"
				    PRIu64 "-%" PRIu64 " key %" PRIu64
				    " exists\n",
				    fname, first_miss, middle_coll, key);
				fatal = true;
			}
			/*
			 * The local table should always have all data.
			 */
			if ((ret = cur_local->search(cur_local)) != 0) {
				if (ret != WT_NOTFOUND)
					testutil_die(ret, "search");
				if (!inmem)
					printf("%s: LOCAL no record with key %"
					    PRIu64 "\n", fname, key);
				absent_local++;
				middle_local = key;
			} else if (middle_local != 0) {
				/*
				 * We should never find an existing key after
				 * we have detected one missing.
				 */
				printf("%s: LOCAL after absent record at %"
				    PRIu64 " key %" PRIu64 " exists\n",
				    fname, middle_local, key);
				fatal = true;
			}
			/*
			 * The oplog table should always have all data.
			 */
			if ((ret = cur_oplog->search(cur_oplog)) != 0) {
				if (ret != WT_NOTFOUND)
					testutil_die(ret, "search");
				if (!inmem)
					printf("%s: OPLOG no record with key %"
					    PRIu64 "\n", fname, key);
				absent_oplog++;
				middle_oplog = key;
			} else if (middle_oplog != 0) {
				/*
				 * We should never find an existing key after
				 * we have detected one missing.
				 */
				printf("%s: OPLOG after absent record at %"
				    PRIu64 " key %" PRIu64 " exists\n",
				    fname, middle_oplog, key);
				fatal = true;
			}
		}
		testutil_checksys(fclose(fp) != 0);
	}
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if (fatal)
		return (EXIT_FAILURE);
	if (!inmem && absent_coll) {
		printf("COLLECTION: %" PRIu64
		    " record(s) absent from %" PRIu64 "\n",
		    absent_coll, count);
		fatal = true;
	}
	if (!inmem && absent_local) {
		printf("LOCAL: %" PRIu64 " record(s) absent from %" PRIu64 "\n",
		    absent_local, count);
		fatal = true;
	}
	if (!inmem && absent_oplog) {
		printf("OPLOG: %" PRIu64 " record(s) absent from %" PRIu64 "\n",
		    absent_oplog, count);
		fatal = true;
	}
	if (fatal)
		return (EXIT_FAILURE);
	printf("%" PRIu64 " records verified\n", count);
	return (EXIT_SUCCESS);
}
