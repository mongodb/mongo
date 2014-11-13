/*-
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
 * ex_log.c
 * 	demonstrates how to logging and log cursors.
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

static const char *home = "WT_HOME_LOG";
static const char *home_full = "WT_HOME_LOG_FULL";
static const char *home_incr = "WT_HOME_LOG_INCR";

static const char *full_out = "./backup_full";
static const char *incr_out = "./backup_incr";

static const char * const uri = "table:logtest";

#define	CONN_CONFIG \
    "create,cache_size=100MB,log=(archive=false,enabled=true,file_max=100K)"
#define	MAX_KEYS	10000

static int
compare_backups(WT_SESSION *session, int i)
{
	int ret;
	WT_CONNECTION *wt_conn;
	char buf[1024];

	(void)snprintf(buf, sizeof(buf),
	    "../../wt -h %s.%d dump logtest > %s.%d",
	    home_full, i, full_out, i);
	ret = system(buf);
	(void)snprintf(buf, sizeof(buf), "../../wt -h %s dump logtest > %s.%d",
	    home_incr, incr_out, i);
	ret = system(buf);
	/*
	 * XXX Need to compare files here.
	 */
	(void)snprintf(buf, sizeof(buf), "cmp %s.%d %s.%d",
	    full_out, i, incr_out, i);
	ret = system(buf);
	if (ret == 0)
		fprintf(stdout,
		    "Iteration %d: Tables %s.%d and %s.%d identical\n",
		    i, full_out, i, incr_out, i);
	else
		fprintf(stdout,
		    "Iteration %d: Tables %s.%d and %s.%d differ\n",
		    i, full_out, i, incr_out, i);
	return (ret);
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
	cursor->close(cursor);
}

static int
take_full_backup(WT_SESSION *session, int i)
{
	WT_CURSOR *cursor;
	int ret;
	char buf[1024], h[256];
	const char *filename, *hdir;

	/*
	 * First time through we take a full backup into the incremental
	 * directory.
	 */
	if (i != 0) {
		snprintf(h, sizeof(h), "%s.%d", home_full, i);
		hdir = h;
		snprintf(buf, sizeof(buf), "rm -rf %s && mkdir %s",
		    hdir, hdir);
		if ((ret = system(buf)) != 0) {
			fprintf(stderr, "%s: failed ret %d\n", buf, ret);
			return (ret);
		}
	} else
		hdir = home_incr;
	ret = session->open_cursor(session, "backup:", NULL, NULL, &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &filename);
		(void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
		    home, filename, hdir, filename);
		if (i == 0)
		ret = system(buf);
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", wiredtiger_strerror(ret));
	ret = cursor->close(cursor);
	return (ret);
}

static int
take_incr_backup(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	int ret;
	char buf[1024];
	const char *filename;

	ret = session->open_cursor(session, "backup:",
	    NULL, "target=(\"log:\")", &cursor);

	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &filename);
		(void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s",
		    home, filename, home_incr, filename);
		ret = system(buf);
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", wiredtiger_strerror(ret));
	/*
	 * With an incremental cursor, we want to truncate on the backup
	 * cursor to archive the logs.
	 */
	ret = session->truncate(session, "log:", cursor, NULL, NULL);
	ret = cursor->close(cursor);
	return (ret);
}

int
main(void)
{
	WT_CONNECTION *wt_conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int i, ret;
	char cmd_buf[256];

	snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s %s && mkdir %s %s",
	    home, home_incr, home, home_incr);
	if ((ret = system(cmd_buf)) != 0) {
		fprintf(stderr, "%s: failed ret %d\n", cmd_buf, ret);
		return (ret);
	}
	if ((ret = wiredtiger_open(home, NULL,
	    CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}

	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri, "key_format=S,value_format=S");
	ret = add_work(session, 0);

	ret = take_full_backup(session, 0);

	ret = session->checkpoint(session, NULL);

	for (i = 1; i < 5; i++) {
		ret = add_work(session, i);
		ret = session->checkpoint(session, NULL);
		ret = take_full_backup(session, i);
		ret = take_incr_backup(session);

		ret = compare_backups(session, i);
	}

	/*
	 * Close and reopen the connection so that the log ends up with
	 * a variety of records such as file sync and checkpoint.  We
	 * have archiving turned off.
	 */
	ret = wt_conn->close(wt_conn, NULL);
	return (ret);
}
