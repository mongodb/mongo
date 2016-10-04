/*
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
 * ex_sync.c
 * 	demonstrates how to use the transaction sync configuration.
 */
#include <errno.h>
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

static const char *home = "WT_HOME";

static const char * const uri = "table:test";

#define	CONN_CONFIG "create,cache_size=100MB,log=(archive=false,enabled=true)"
#define	MAX_KEYS	100

int
main(void)
{
	WT_CONNECTION *wt_conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int i, record_count, ret;
	char cmd_buf[256], k[16], v[16];
	const char *conf;

	snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s && mkdir %s",
	    home, home);
	if ((ret = system(cmd_buf)) != 0) {
		fprintf(stderr, "%s: failed ret %d\n", cmd_buf, ret);
		return (EXIT_FAILURE);
	}
	if ((ret = wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri, "key_format=S,value_format=S");

	ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
	/*
	 * Perform some operations with individual auto-commit transactions.
	 */
	ret = session->begin_transaction(session, NULL);
	for (record_count = 0, i = 0; i < MAX_KEYS; i++, record_count++) {
		if (i == MAX_KEYS/2) {
			ret = session->commit_transaction(
			    session, "sync=background");
			ret = session->transaction_sync(
			    session, "timeout_ms=0");
			if (ret == ETIMEDOUT)
				printf("Transactions not yet stable\n");
			else if (ret != 0)
				printf("Got error %d\n", ret);
			ret = session->begin_transaction(session, NULL);
		} else {
			if ((record_count % 3) == 0)
				conf = "sync=background";
			else
				conf = "sync=off";
			ret = session->commit_transaction(session, conf);
			ret = session->begin_transaction(session, NULL);
		}
		snprintf(k, sizeof(k), "key%d", i);
		snprintf(v, sizeof(v), "value%d", i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = session->commit_transaction(session, "sync=background");
	printf("Wait forever until stable\n");
	ret = session->transaction_sync(session, NULL);
	printf("Transactions now stable\n");
	ret = session->begin_transaction(session, NULL);
	/*
	 * Perform some operations within a single transaction.
	 */
	for (i = MAX_KEYS; i < MAX_KEYS+5; i++, record_count++) {
		snprintf(k, sizeof(k), "key%d", i);
		snprintf(v, sizeof(v), "value%d", i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = session->commit_transaction(session, "sync=on");
	ret = session->transaction_sync(session, "timeout_ms=0");
	if (ret != 0)
		fprintf(stderr,
		    "Unexpected error %d from WT_SESSION::transaction_sync\n",
		    ret);
	/*
	 * Demonstrate using log_flush to force the log to disk.
	 */
	for (i = 0; i < MAX_KEYS; i++, record_count++) {
		snprintf(k, sizeof(k), "key%d", record_count);
		snprintf(v, sizeof(v), "value%d", record_count);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = session->log_flush(session, "sync=on");

	for (i = 0; i < MAX_KEYS; i++, record_count++) {
		snprintf(k, sizeof(k), "key%d", record_count);
		snprintf(v, sizeof(v), "value%d", record_count);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = cursor->close(cursor);
	ret = session->log_flush(session, "sync=off");
	ret = session->log_flush(session, "sync=on");

	ret = wt_conn->close(wt_conn, NULL);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
