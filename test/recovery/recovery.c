/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <wiredtiger.h>

#include "test_util.i"

static char home[512];			/* Program working dir */
static const char *progname;		/* Program name */
static const char *uri = "table:main";

#define	RECORDS_FILE "records"

#define	ENV_CONFIG						\
    "create,log=(file_max=10M,archive=false,enabled),"		\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_REC "log=(recover=on)"
#define	MAX_VAL	4096

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
static void
fill_db()
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_ITEM data;
	WT_RAND_STATE rnd;
	WT_SESSION *session;
	uint64_t i;
	int fd, ret;
	uint8_t buf[MAX_VAL];

	__wt_random_init(&rnd);
	memset(buf, 0, sizeof(buf));
	/*
	 * Initialize the first 25% to random values.  Leave a bunch of data
	 * space at the end to emphasize zero data.
	 */
	for (i = 0; i < MAX_VAL/4; i++)
		buf[i] = (uint8_t)__wt_random(&rnd);

	/*
	 * Run in the home directory so that the records file is in there too.
	 */
	chdir(home);
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret = session->create(session,
	    uri, "key_format=Q,value_format=u")) != 0)
		testutil_die(ret, "WT_SESSION.create: %s", uri);
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	/*
	 * Keep a separate file with the records we wrote for checking.
	 */
	unlink(RECORDS_FILE);

	if ((fd = open(RECORDS_FILE, O_CREAT | O_WRONLY, 0777)) == -1)
		testutil_die(errno, "open");

	/*
	 * Write data into the table until we are killed by the parent.
	 * The data in the buffer is already set to random content.
	 */
	data.data = buf;
	for (i = 0;; ++i) {
		data.size = __wt_random(&rnd) % MAX_VAL;
		cursor->set_key(cursor, i);
		cursor->set_value(cursor, &data);
		if ((ret = cursor->insert(cursor)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");
		if (write(fd, &i, sizeof(i)) != sizeof(i))
			testutil_die(errno, "write");
	}
}

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	WT_RAND_STATE rnd;
	uint64_t key;
	uint32_t absent, count, sleep_cnt;
	size_t rd;
	int ch, fd, status, ret;
	pid_t pid;
	char *working_dir;

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;

	working_dir = NULL;
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
	__wt_random_init(&rnd);
	/*
	 * We want to sleep a random amount between 10-25 seconds and then
	 * kill the child.
	 */
	sleep_cnt = 10 + __wt_random(&rnd) % 15;
	printf("Parent: sleep %" PRIu32 " seconds, then kill child\n",
	    sleep_cnt);
	sleep(sleep_cnt);

	/*
	 * !!! It should be plenty long enough to make sure more than one
	 * log file exists.  If wanted, that check would be added here.
	 */
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);

	/*
	 * !!! If we wanted to take a copy of the directory before recovery,
	 * this is the place to do it.
	 */
	chdir(home);
	if ((ret = wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open");
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");
	if ((ret = session->log_flush(session, "sync=on")) != 0)
		testutil_die(ret, "WT_SESSION.log_flush");
	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	if ((fd = open(RECORDS_FILE, O_RDONLY)) == -1)
		testutil_die(errno, "open");

	for (absent = count = 0;; ++count) {
		rd = read(fd, &key, sizeof(key));
		if (rd != sizeof(key)) {
			if (rd == 0)
				break;
			testutil_die(errno, "read");
		}
		cursor->set_key(cursor, key);
		if ((ret = cursor->search(cursor)) != 0) {
			if (ret != WT_NOTFOUND)
				testutil_die(ret, "search");
			printf("no record with key %" PRIu64 "\n", key);
			++absent;
		}
	}
	close(fd);
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if (absent) {
		printf("%u record(s) absent from %u\n", absent, count);
		return (EXIT_FAILURE);
	}
	return (EXIT_SUCCESS);
}
