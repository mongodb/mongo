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
 *
 * ex_backup.c
 * 	demonstrates how to use incremental backup and log files.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
/* snprintf is not supported on <= VS2013 */
#define	snprintf _snprintf
#endif

#include <wiredtiger.h>

static const char * const home = "WT_HOME_LOG";
static const char * const home_full = "WT_HOME_LOG_FULL";
static const char * const home_incr = "WT_HOME_LOG_INCR";

static const char * const full_out = "./backup_full";
static const char * const incr_out = "./backup_incr";

static const char * const uri = "table:logtest";

#define	CONN_CONFIG \
    "create,cache_size=100MB,log=(archive=false,enabled=true,file_max=100K)"
#define	MAX_ITERATIONS	5
#define	MAX_KEYS	10000

static int
compare_backups(int i)
{
	int ret;
	char buf[1024], msg[8];

	/*
	 * We run 'wt dump' on both the full backup directory and the
	 * incremental backup directory for this iteration.  Since running
	 * 'wt' runs recovery and makes both directories "live", we need
	 * a new directory for each iteration.
	 *
	 * If i == 0, we're comparing against the main, original directory
	 * with the final incremental directory.
	 */
	if (i == 0)
		(void)snprintf(buf, sizeof(buf),
		    "../../wt -R -h %s dump logtest > %s.%d",
		    home, full_out, i);
	else
		(void)snprintf(buf, sizeof(buf),
		    "../../wt -R -h %s.%d dump logtest > %s.%d",
		    home_full, i, full_out, i);
	ret = system(buf);
	/*
	 * Now run dump on the incremental directory.
	 */
	(void)snprintf(buf, sizeof(buf),
	    "../../wt -R -h %s.%d dump logtest > %s.%d",
	    home_incr, i, incr_out, i);
	ret = system(buf);

	/*
	 * Compare the files.
	 */
	(void)snprintf(buf, sizeof(buf), "cmp %s.%d %s.%d",
	    full_out, i, incr_out, i);
	ret = system(buf);
	if (i == 0)
		(void)strncpy(msg, "MAIN", sizeof(msg));
	else
		snprintf(msg, sizeof(msg), "%d", i);
	printf(
	    "Iteration %s: Tables %s.%d and %s.%d %s\n",
	    msg, full_out, i, incr_out, i, ret == 0 ? "identical" : "differ");
	if (ret != 0)
		exit (1);

	/*
	 * If they compare successfully, clean up.
	 */
	if (i != 0) {
		(void)snprintf(buf, sizeof(buf),
		    "rm -rf %s.%d %s.%d %s.%d %s.%d",
		    home_full, i, home_incr, i, full_out, i, incr_out, i);
		ret = system(buf);
	}
	return (ret);
}

/*
 * Set up all the directories needed for the test.  We have a full backup
 * directory for each iteration and an incremental backup for each iteration.
 * That way we can compare the full and incremental each time through.
 */
static int
setup_directories(void)
{
	int i, ret;
	char buf[1024];

	for (i = 0; i < MAX_ITERATIONS; i++) {
		/*
		 * For incremental backups we need 0-N.  The 0 incremental
		 * directory will compare with the original at the end.
		 */
		snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir %s.%d",
		    home_incr, i, home_incr, i);
		if ((ret = system(buf)) != 0) {
			fprintf(stderr, "%s: failed ret %d\n", buf, ret);
			return (ret);
		}
		if (i == 0)
			continue;
		/*
		 * For full backups we need 1-N.
		 */
		snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir %s.%d",
		    home_full, i, home_full, i);
		if ((ret = system(buf)) != 0) {
			fprintf(stderr, "%s: failed ret %d\n", buf, ret);
			return (ret);
		}
	}
	return (0);
}

static int
add_work(WT_SESSION *session, int iter)
{
	WT_CURSOR *cursor;
	int i, ret;
	char k[32], v[32];

	ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
	/*
	 * Perform some operations with individual auto-commit transactions.
	 */
	for (i = 0; i < MAX_KEYS; i++) {
		snprintf(k, sizeof(k), "key.%d.%d", iter, i);
		snprintf(v, sizeof(v), "value.%d.%d", iter, i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = cursor->close(cursor);
	return (ret);
}

static int
take_full_backup(WT_SESSION *session, int i)
{
	WT_CURSOR *cursor;
	int j, ret;
	char buf[1024], h[256];
	const char *filename, *hdir;

	/*
	 * First time through we take a full backup into the incremental
	 * directories.  Otherwise only into the appropriate full directory.
	 */
	if (i != 0) {
		snprintf(h, sizeof(h), "%s.%d", home_full, i);
		hdir = h;
	} else
		hdir = home_incr;
	ret = session->open_cursor(session, "backup:", NULL, NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &filename);
		if (i == 0)
			/*
			 * Take a full backup into each incremental directory.
			 */
			for (j = 0; j < MAX_ITERATIONS; j++) {
				snprintf(h, sizeof(h), "%s.%d", home_incr, j);
				(void)snprintf(buf, sizeof(buf),
				    "cp %s/%s %s/%s",
				    home, filename, h, filename);
				ret = system(buf);
			}
		else {
			snprintf(h, sizeof(h), "%s.%d", home_full, i);
			(void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
			    home, filename, hdir, filename);
			ret = system(buf);
		}
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", session->strerror(session, ret));
	ret = cursor->close(cursor);
	return (ret);
}

static int
take_incr_backup(WT_SESSION *session, int i)
{
	WT_CURSOR *cursor;
	int j, ret;
	char buf[1024], h[256];
	const char *filename;

	ret = session->open_cursor(session, "backup:",
	    NULL, "target=(\"log:\")", &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &filename);
		/*
		 * Copy into the 0 incremental directory and then each of the
		 * incremental directories for this iteration and later.
		 */
		snprintf(h, sizeof(h), "%s.0", home_incr);
		(void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
		    home, filename, h, filename);
		ret = system(buf);
		for (j = i; j < MAX_ITERATIONS; j++) {
			snprintf(h, sizeof(h), "%s.%d", home_incr, j);
			(void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
			    home, filename, h, filename);
			ret = system(buf);
		}
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", session->strerror(session, ret));
	ret = 0;
	/*
	 * With an incremental cursor, we want to truncate on the backup
	 * cursor to archive the logs.  Only do this if the copy process
	 * was entirely successful.
	 */
	ret = session->truncate(session, "log:", cursor, NULL, NULL);
	ret = cursor->close(cursor);
	return (ret);
}

int
main(void)
{
	WT_CONNECTION *wt_conn;
	WT_SESSION *session;
	int i, ret;
	char cmd_buf[256];

	snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s && mkdir %s", home, home);
	if ((ret = system(cmd_buf)) != 0) {
		fprintf(stderr, "%s: failed ret %d\n", cmd_buf, ret);
		return (EXIT_FAILURE);
	}
	if ((ret = wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	ret = setup_directories();
	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri, "key_format=S,value_format=S");
	printf("Adding initial data\n");
	ret = add_work(session, 0);

	printf("Taking initial backup\n");
	ret = take_full_backup(session, 0);

	ret = session->checkpoint(session, NULL);

	for (i = 1; i < MAX_ITERATIONS; i++) {
		printf("Iteration %d: adding data\n", i);
		ret = add_work(session, i);
		ret = session->checkpoint(session, NULL);
		/*
		 * The full backup here is only needed for testing and
		 * comparison purposes.  A normal incremental backup
		 * procedure would not include this.
		 */
		printf("Iteration %d: taking full backup\n", i);
		ret = take_full_backup(session, i);
		/*
		 * Taking the incremental backup also calls truncate
		 * to archive the log files, if the copies were successful.
		 * See that function for details on that call.
		 */
		printf("Iteration %d: taking incremental backup\n", i);
		ret = take_incr_backup(session, i);

		printf("Iteration %d: dumping and comparing data\n", i);
		ret = compare_backups(i);
	}

	/*
	 * Close the connection.  We're done and want to run the final
	 * comparison between the incremental and original.
	 */
	ret = wt_conn->close(wt_conn, NULL);

	printf("Final comparison: dumping and comparing data\n");
	ret = compare_backups(0);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
