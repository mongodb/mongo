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
static char home[HOME_SIZE];		/* Program working dir */
static char home_rd[HOME_SIZE];		/* Read-only dir */
static char home_rd2[HOME_SIZE];	/* Read-only dir */
static const char *progname;		/* Program name */
static const char *saved_argv0;		/* Program name */
static const char *uri = "table:main";

#define	ENV_CONFIG						\
    "create,log=(file_max=10M,archive=false,enabled),"		\
    "transaction_sync=(enabled,method=none)"
#define	ENV_CONFIG_RD "readonly=true"
#define	MAX_VAL	4096
#define	MAX_KV	10000

static void
usage(void)
{
	fprintf(stderr, "usage: %s [-h dir]\n", progname);
	exit(EXIT_FAILURE);
}

static int
run_child(const char *homedir)
{
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int i, ret;

	/*
	 * We expect the read-only database will allow the second read-only
	 * handle to succeed because no one can create or set the lock file.
	 */
	if ((ret = wiredtiger_open(homedir, NULL, ENV_CONFIG_RD, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open readonly");

	/*
	 * Make sure we can read the data.
	 */
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0)
		testutil_die(ret, "WT_CONNECTION:open_session");

	if ((ret =
	    session->open_cursor(session, uri, NULL, NULL, &cursor)) != 0)
		testutil_die(ret, "WT_SESSION.open_cursor: %s", uri);

	i = 0;
	while ((ret = cursor->next(cursor)) == 0)
		++i;
	if (i != MAX_KV)
		testutil_die(EPERM, "cursor walk");
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "conn_close");
	return (0);
}

/*
 * Child process opens both databases readonly.
 */
static void
open_dbs(const char *dir, const char *dir_rd, const char *dir_rd2)
{
	WT_CONNECTION *conn;
	int ret;

	/*
	 * The parent has an open connection to all directories.
	 * We expect opening the writeable home to return an error.
	 * It is a failure if the child successfully opens that.
	 */
	if ((ret = wiredtiger_open(dir, NULL, ENV_CONFIG_RD, &conn)) == 0)
		testutil_die(ret, "wiredtiger_open readonly allowed");

	if ((ret = run_child(dir_rd)) != 0)
		testutil_die(ret, "run child 1");
	if ((ret = run_child(dir_rd2)) != 0)
		testutil_die(ret, "run child 2");
	exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

int
main(int argc, char *argv[])
{
	WT_CONNECTION *conn, *conn2, *conn3;
	WT_CURSOR *cursor;
	WT_ITEM data;
	WT_SESSION *session;
	uint64_t i;
	int ch, status, ret;
	bool child;
	const char *working_dir;
	char cmd[512];
	uint8_t buf[MAX_VAL];

	if ((progname = strrchr(argv[0], DIR_DELIM)) == NULL)
		progname = argv[0];
	else
		++progname;
	/*
	 * Needed unaltered for system command later.
	 */
	saved_argv0 = argv[0];

	working_dir = "WT_RD";
	child = false;
	while ((ch = __wt_getopt(progname, argc, argv, "Ch:")) != EOF)
		switch (ch) {
		case 'C':
			child = true;
			break;
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

	memset(buf, 0, sizeof(buf));
	/*
	 * Set up all the directory names.
	 */
	testutil_work_dir_from_path(home, 512, working_dir);
	strncpy(home_rd, home, HOME_SIZE);
	strcat(home_rd, ".RD");
	strncpy(home_rd2, home, HOME_SIZE);
	strcat(home_rd2, ".NOLOCK");
	if (!child) {
		testutil_make_work_dir(home);
		testutil_make_work_dir(home_rd);
		testutil_make_work_dir(home_rd2);
	} else {
		/*
		 * We are a child process, we just want to call
		 * the open_dbs with the directories we have.
		 * The child function will exit.
		 */
		open_dbs(home, home_rd, home_rd2);
	}

	/*
	 * Parent creates a database and table.  Then cleanly shuts down.
	 * Then copy database to read-only directory and chmod.
	 * Also copy database to read-only directory and remove the lock
	 * file.  One read-only database will have a lock file in the
	 * file system and the other will not.
	 * Parent opens all databases with read-only configuration flag.
	 * Parent forks off child who tries to also open all databases
	 * with the read-only flag.  It should error on the writeable
	 * directory, but allow it on the read-only directories.
	 * The child then confirms it can read all the data.
	 */
	/*
	 * Run in the home directory and create the table.
	 */
	if ((ret = wiredtiger_open(home, NULL, ENV_CONFIG, &conn)) != 0)
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
	 * Write data into the table and then cleanly shut down connection.
	 */
	data.data = buf;
	data.size = MAX_VAL;
	for (i = 0; i < MAX_KV; ++i) {
		cursor->set_key(cursor, i);
		cursor->set_value(cursor, &data);
		if ((ret = cursor->insert(cursor)) != 0)
			testutil_die(ret, "WT_CURSOR.insert");
	}
	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");

	/*
	 * Copy the database.  Remove any lock file from one copy
	 * and chmod the copies to be read-only permissions.
	 */
	(void)snprintf(cmd, sizeof(cmd),
	    "cp -rp %s/* %s; chmod 0555 %s; chmod -R 0444 %s/*",
	    home, home_rd, home_rd, home_rd);
	(void)system(cmd);
	(void)snprintf(cmd, sizeof(cmd),
	    "cp -rp %s/* %s; rm -f %s/WiredTiger.lock; "
	    "chmod 0555 %s; chmod -R 0444 %s/*",
	    home, home_rd2, home_rd2, home_rd2, home_rd2);
	(void)system(cmd);

	/*
	 * Open a connection handle to all databases.
	 */
	fprintf(stderr, " *** Expect several error messages from WT ***\n");
	if ((ret = wiredtiger_open(home, NULL, ENV_CONFIG_RD, &conn)) != 0)
		testutil_die(ret, "wiredtiger_open readonly");
	if ((ret = wiredtiger_open(home_rd, NULL, ENV_CONFIG_RD, &conn2)) != 0)
		testutil_die(ret, "wiredtiger_open readonly2");
	if ((ret = wiredtiger_open(home_rd2, NULL, ENV_CONFIG_RD, &conn3)) != 0)
		testutil_die(ret, "wiredtiger_open readonly3");

	/*
	 * Create a child to also open a connection handle to the databases.
	 * We cannot use fork here because using fork the child inherits the
	 * same memory image.  Therefore the WT process structure is set in
	 * the child even though it should not be.  So use 'system' to spawn
	 * an entirely new process.
	 */
	(void)snprintf(cmd, sizeof(cmd), "%s -C", saved_argv0);
	if ((status = system(cmd)) < 0)
		testutil_die(status, "system");

	/*
	 * The child will exit with success if its test passes.
	 */
	if (WEXITSTATUS(status) != 0)
		testutil_die(WEXITSTATUS(status), "system");

	if ((ret = conn->close(conn, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if ((ret = conn2->close(conn2, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	if ((ret = conn3->close(conn3, NULL)) != 0)
		testutil_die(ret, "WT_CONNECTION:close");
	/*
	 * We need to chmod the read-only databases back so that they can
	 * be removed by scripts.
	 */
	(void)snprintf(cmd, sizeof(cmd), "chmod 0777 %s %s", home_rd, home_rd2);
	(void)system(cmd);
	(void)snprintf(cmd, sizeof(cmd), "chmod -R 0666 %s/* %s/*",
	    home_rd, home_rd2);
	(void)system(cmd);
	printf(" *** Readonly test successful ***\n");
	return (EXIT_SUCCESS);
}
