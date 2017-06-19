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

/*
 * A thread dedicated to appending records into a table. Works with fixed
 * length column stores and variable length column stores.
 * One thread (the first thread created by an application) checks for a
 * terminating condition after each insert.
 */
void *
thread_append(void *arg)
{
	TEST_OPTS *opts;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t id, recno;
	char buf[64];

	opts = (TEST_OPTS *)arg;
	conn = opts->conn;

	id = __wt_atomic_fetch_addv64(&opts->next_threadid, 1);
	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(
	    session->open_cursor(session, opts->uri, NULL, "append", &cursor));

	buf[0] = '\2';
	for (recno = 1; opts->running; ++recno) {
		if (opts->table_type == TABLE_FIX)
			cursor->set_value(cursor, buf[0]);
		else {
			snprintf(buf, sizeof(buf),
			    "%" PRIu64 " VALUE ------", recno);
			cursor->set_value(cursor, buf);
		}
		testutil_check(cursor->insert(cursor));
		if (id == 0) {
			testutil_check(
			    cursor->get_key(cursor, &opts->max_inserted_id));
			if (opts->max_inserted_id >= opts->nrecords)
				opts->running = false;
		}
	}

	return (NULL);
}

/*
 * Append into a row store table.
 */
void *
thread_insert_append(void *arg)
{
	TEST_OPTS *opts;
	WT_CONNECTION *conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	uint64_t i;
	char kbuf[64];

	opts = (TEST_OPTS *)arg;
	conn = opts->conn;

	testutil_check(conn->open_session(conn, NULL, NULL, &session));
	testutil_check(session->open_cursor(
	    session, opts->uri, NULL, NULL, &cursor));

	for (i = 0; i < opts->nrecords; ++i) {
		snprintf(kbuf, sizeof(kbuf), "%010d KEY------", (int)i);
		cursor->set_key(cursor, kbuf);
		cursor->set_value(cursor, "========== VALUE =======");
		testutil_check(cursor->insert(cursor));
		if (i % 100000 == 0) {
			printf("insert: %" PRIu64 "\r", i);
			fflush(stdout);
		}
	}
	printf("\n");

	opts->running = false;

	return (NULL);
}

/*
 * Repeatedly walk backwards through the records in a table.
 */
void *
thread_prev(void *arg)
{
	TEST_OPTS *opts;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int ret;

	opts = (TEST_OPTS *)arg;
	ret = 0;

	testutil_check(
	    opts->conn->open_session(opts->conn, NULL, NULL, &session));
	testutil_check(
	    session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
	while (opts->running) {
		while (opts->running && (ret = cursor->prev(cursor)) == 0)
			;
		if (ret == WT_NOTFOUND)
			ret = 0;
		testutil_check(ret);
	}

	testutil_check(session->close(session, NULL));
	return (NULL);
}
