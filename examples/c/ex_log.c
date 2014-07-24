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
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>

const char *home = NULL;
const char *uri = "table:logtest";

#define	MAX_KEYS	10

static int
walk_log(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	WT_LSN lsn, lsnsave;
	WT_ITEM log_rec;
	int i, ret;

	/*! [log cursor] */
	ret = session->open_cursor(session, "log:", NULL, NULL, &cursor);
	/*! [log cursor] */
	i = 0;
	memset(&lsnsave, 0, sizeof(lsnsave));
	while ((ret = cursor->next(cursor)) == 0) {
		/*! [log cursor get_key] */
		ret = cursor->get_key(cursor, &lsn.file, &lsn.offset);
		/*! [log cursor get_key] */
		if (++i == 3)
			lsnsave = lsn;
		/*! [log cursor get_value] */
		ret = cursor->get_value(cursor, &log_rec);
		/*! [log cursor get_value] */
		printf("LSN [%d][%" PRIu64 "]:  Size %" PRIu64 "\n",
		    lsn.file, lsn.offset, (u_long)log_rec.size);
	}
	cursor->reset(cursor);
	/*! [log cursor set_key] */
	cursor->set_key(cursor, lsnsave.file, lsnsave.offset);
	/*! [log cursor set_key] */
	/*! [log cursor search] */
	ret = cursor->search(cursor);
	/*! [log cursor search] */
	ret = cursor->get_value(cursor, &log_rec);
	printf("Searched LSN [%d][%" PRIu64 "]:  Size %" PRIu64 "\n",
	    lsnsave.file, lsnsave.offset, (u_long)log_rec.size);
	ret = cursor->get_key(cursor, &lsn.file, &lsn.offset);
	assert(lsnsave.file == lsn.file && lsnsave.offset == lsn.offset);
	cursor->close(cursor);
	return (ret);
}

static int
iterate_log(WT_SESSION *session)
{
	WT_CURSOR *cursor;
	WT_LSN lsn, lsnsave;
	WT_ITEM log_rec;
	uint64_t txnid;
	uint32_t oprec;
	int first, i, ret;

	/*! [log iterator open] */
	ret = session->open_cursor(session,
	    "log:", NULL, "iterator=true", &cursor);
	/*! [log iterator open] */
	i = 0;
	memset(&lsnsave, 0, sizeof(lsnsave));
	while ((ret = cursor->next(cursor)) == 0) {
		/*! [log iterator get_key] */
		ret = cursor->get_key(cursor, &lsn.file, &lsn.offset,
		    &txnid, &oprec);
		/*! [log iterator get_key] */
		if (++i == MAX_KEYS)
			lsnsave = lsn;
		/*! [log iterator get_value] */
		ret = cursor->get_value(cursor, &log_rec);
		/*! [log iterator get_value] */
		printf("LSN [%d][%" PRIu64 "].%" PRIu64
		    ".%d:  Size %" PRIu64 "\n",
		    lsn.file, lsn.offset, txnid, oprec, (u_long)log_rec.size);
	}
	cursor->reset(cursor);
	/*! [log iterator set_key] */
	cursor->set_key(cursor, lsnsave.file, lsnsave.offset, 0, 0);
	/*! [log iterator set_key] */
	/*! [log iterator search] */
	ret = cursor->search(cursor);
	/*! [log iterator search] */
	printf("Searched ");
	/*
	 * Walk all records starting with this key.
	 */
	first = 1;
	while ((ret = cursor->get_key(cursor,
	    &lsn.file, &lsn.offset, &txnid, &oprec)) == 0) {
		if (first) {
			first = 0;
			assert(lsnsave.file == lsn.file &&
			    lsnsave.offset == lsn.offset);
		}
		ret = cursor->get_value(cursor, &log_rec);
		printf("LSN [%d][%" PRIu64 "].%" PRIu64
		    ".%d:  Size %" PRIu64 "\n",
		    lsn.file, lsn.offset, txnid, oprec, (u_long)log_rec.size);
		ret = cursor->next(cursor);
		if (ret != 0)
			break;
	}
	cursor->close(cursor);
	return (ret);
}

int main(void)
{
	WT_CONNECTION *wt_conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int i, ret;
	char k[16], v[16];

#define	CONN_CONFIG "create,cache_size=100MB,log=(enabled=true)"
	if ((ret = wiredtiger_open(home, NULL,
	    CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home, wiredtiger_strerror(ret));
		return (ret);
	}

	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri,
	    "key_format=S,value_format=S");

	ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
	/*
	 * Perform some operations with individual auto-commit transactions.
	 */
	for (i = 0; i < MAX_KEYS; i++) {
		snprintf(k, sizeof(k), "key%d", i);
		snprintf(v, sizeof(v), "value%d", i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = session->begin_transaction(session, NULL);
	/*
	 * Perform some operations within a single transaction.
	 */
	for (i = MAX_KEYS; i < MAX_KEYS+5; i++) {
		snprintf(k, sizeof(k), "key%d", i);
		snprintf(v, sizeof(v), "value%d", i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
	}
	ret = session->commit_transaction(session, NULL);
	cursor->close(cursor);

	ret = walk_log(session);
	ret = iterate_log(session);

	ret = wt_conn->close(wt_conn, NULL);
	return (ret);
}
