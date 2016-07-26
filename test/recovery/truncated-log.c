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

#ifdef _WIN32
/* snprintf is not supported on <= VS2013 */
#define	snprintf _snprintf
#endif

static char home[512];			/* Program working dir */
static const char *progname;		/* Program name */
static const char * const uri = "table:main";

#define	RECORDS_FILE "records"

#define	ENV_CONFIG						\
    "create,log=(file_max=100K,archive=false,enabled),"		\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_REC "log=(recover=on)"
#define	LOG_FILE_1 "WiredTigerLog.0000000001"

#define	K_SIZE	16
#define	V_SIZE	256

static void usage(void)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
usage(void)
{
	fprintf(stderr, "usage: %s [-h dir]\n", progname);
	exit(EXIT_FAILURE);
}

/*
 * Child process creates the database and table, and then writes data into
 * the table until it is killed by the parent.
 */
static void fill_db(void)WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
fill_db(void)
{
	FILE *fp;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor, *logc;
	WT_LSN lsn, save_lsn;
	WT_SESSION *session;
	uint32_t i, max_key, min_key, units, unused;
	int ret;
	bool first;
	char k[K_SIZE], v[V_SIZE];

	/*
	 * Run in the home directory so that the records file is in there too.
	 */
	if (chdir(home) != 0)
		testutil_die(errno, "chdir: %s", home);
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret = session->create(session,
	    uri, "key_format=S,value_format=S")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri);
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	/*
	 * Keep a separate file with the records we wrote for checking.
	 */
	(void)unlink(RECORDS_FILE);
	if ((fp = fopen(RECORDS_FILE, "w")) == NULL)
		testutil_die(errno, "fopen");
	/*
	 * Set to no buffering.
	 */
	__wt_stream_set_no_buffer(fp);
	save_lsn.l.file = 0;

	/*
	 * Write data into the table until we move to log file 2.
	 * We do the calculation below so that we don't have to walk the
	 * log for every record.
	 *
	 * Calculate about how many records should fit in the log file.
	 * Subtract a bunch for metadata and file creation records.
	 * Then subtract out a few more records to be conservative.
	 */
	units = (K_SIZE + V_SIZE) / 128 + 1;
	min_key = 90000 / (units * 128) - 15;
	max_key = min_key * 2;
	first = true;
	for (i = 0; i < max_key; ++i) {
		snprintf(k, sizeof(k), "key%03d", (int)i);
		snprintf(v, sizeof(v), "value%0*d",
		    (int)(V_SIZE - strlen("value")), (int)i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		if ((ret = cursor->insert(cursor)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");

		if (i > min_key) {
			if ((ret = session->open_cursor(
			    session, "log:", NULL, NULL, &logc)) != 0)
				testutil_die(ret, "open_cursor: log");
			if (save_lsn.l.file != 0) {
				logc->set_key(logc,
				    save_lsn.l.file, save_lsn.l.offset, 0);
				if ((ret = logc->search(logc)) != 0)
					testutil_die(errno, "search");
			}
			while ((ret = logc->next(logc)) == 0) {
				if ((ret = logc->get_key(logc,
				    &lsn.l.file, &lsn.l.offset, &unused)) != 0)
					testutil_die(errno, "get_key");
				if (lsn.l.file < 2)
					save_lsn = lsn;
				else {
					if (first)
						testutil_die(EINVAL,
						    "min_key too high");
					if (fprintf(fp,
					    "%" PRIu32 " %" PRIu32 "\n",
					    save_lsn.l.offset, i - 1) == -1)
						testutil_die(errno, "fprintf");
					break;
				}
			}
			first = false;
		}
	}
	if (fclose(fp) != 0)
		testutil_die(errno, "fclose");
	abort();
	/* NOTREACHED */
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
	uint64_t new_offset, offset;
	uint32_t count, max_key;
	int ch, status, ret;
	pid_t pid;
	const char *working_dir;

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	working_dir = "WT_TEST.truncated-log";
	while ((ch = __wt_getopt(progname, argc, argv, "h:")) != EOF)
		switch (ch) {
		case 'h':
			working_dir = __wt_optarg;
			break;
		default:
			usage();
		}
	argc -= __wt_optind;
	argv += __wt_optind;
	if (argc != 0)
		usage();

	testutil_work_dir_from_path(home, 512, working_dir);
	testutil_make_work_dir(home);

	/*
	 * Fork a child to insert as many items.  We will then randomly
	 * kill the child, run recovery and make sure all items we wrote
	 * exist after recovery runs.
	 */
	if ((pid = fork()) < 0)
		testutil_die(errno, "fork");

	if (pid == 0) { /* child */
		fill_db();
		return (EXIT_SUCCESS);
	}

	/* parent */
	/* Wait for child to kill itself. */
	if (waitpid(pid, &status, 0) == -1)
		testutil_die(errno, "waitpid");

	/*
	 * !!! If we wanted to take a copy of the directory before recovery,
	 * this is the place to do it.
	 */
	if (chdir(home) != 0)
		testutil_die(errno, "chdir: %s", home);

	printf("Open database, run recovery and verify content\n");
	if ((fp = fopen(RECORDS_FILE, "r")) == NULL)
		testutil_die(errno, "fopen");
	ret = fscanf(fp, "%" SCNu64 " %" SCNu32 "\n", &offset, &max_key);
	if (ret != 2)
		testutil_die(errno, "fscanf");
	if (fclose(fp) != 0)
		testutil_die(errno, "fclose");
	/*
	 * The offset is the beginning of the last record.  Truncate to
	 * the middle of that last record (i.e. ahead of that offset).
	 */
	if (offset > UINT64_MAX - V_SIZE)
		testutil_die(ERANGE, "offset");
	new_offset = offset + V_SIZE;
	printf("Parent: Truncate to %" PRIu64 "\n", new_offset);
	if ((ret = truncate(LOG_FILE_1, (wt_off_t)new_offset)) != 0)
		testutil_die(errno, "truncate");

	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	/*
	 * For every key in the saved file, verify that the key exists
	 * in the table after recovery.  Since we did write-no-sync, we
	 * expect every key to have been recovered.
	 */
	count = 0;
	while ((ret = cursor->next(cursor)) == 0)
		++count;
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if (count > max_key) {
		printf("expected %" PRIu32 " records found %" PRIu32 "\n",
		    max_key, count);
		return (EXIT_FAILURE);
	}
	printf("%" PRIu32 " records verified\n", count);
	return (EXIT_SUCCESS);
}
