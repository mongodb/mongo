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
 * ex_log.c
 * 	demonstrates how to use logging and log cursors.
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

static const char *home1 = "WT_HOME_LOG_1";
static const char *home2 = "WT_HOME_LOG_2";

static const char * const uri = "table:logtest";

#define	CONN_CONFIG "create,cache_size=100MB,log=(archive=false,enabled=true)"
#define	MAX_KEYS	10

static int
setup_copy(WT_CONNECTION **wt_connp, WT_SESSION **sessionp)
{
	int ret;

	if ((ret = wiredtiger_open(home2, NULL, CONN_CONFIG, wt_connp)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home1, wiredtiger_strerror(ret));
		return (ret);
	}

	ret = (*wt_connp)->open_session(*wt_connp, NULL, NULL, sessionp);
	ret = (*sessionp)->create(*sessionp, uri,
	    "key_format=S,value_format=S");
	return (ret);
}

static int
compare_tables(WT_SESSION *session, WT_SESSION *sess_copy)
{
	WT_CURSOR *cursor, *curs_copy;
	int ret;
	const char *key, *key_copy, *value, *value_copy;

	ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
	ret = sess_copy->open_cursor(sess_copy, uri, NULL, NULL, &curs_copy);

	while ((ret = cursor->next(cursor)) == 0) {
		ret = curs_copy->next(curs_copy);
		ret = cursor->get_key(cursor, &key);
		ret = cursor->get_value(cursor, &value);
		ret = curs_copy->get_key(curs_copy, &key_copy);
		ret = curs_copy->get_value(curs_copy, &value_copy);
		if (strcmp(key, key_copy) != 0 ||
		    strcmp(value, value_copy) != 0) {
			fprintf(stderr,
			    "Mismatched: key %s, key_copy %s "
			    "value %s value_copy %s\n",
			    key, key_copy, value, value_copy);
			return (1);
		}
	}
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "WT_CURSOR.next: %s\n", session->strerror(session, ret));
	ret = cursor->close(cursor);

	ret = curs_copy->next(curs_copy);
	if (ret != WT_NOTFOUND)
		fprintf(stderr,
		    "copy: WT_CURSOR.next: %s\n",
		    session->strerror(session, ret));
	ret = curs_copy->close(curs_copy);

	return (ret);
}

/*! [log cursor walk] */
static void
print_record(uint32_t log_file, uint32_t log_offset, uint32_t opcount,
   uint32_t rectype, uint32_t optype, uint64_t txnid, uint32_t fileid,
   WT_ITEM *key, WT_ITEM *value)
{
	printf(
	    "LSN [%" PRIu32 "][%" PRIu32 "].%" PRIu32
	    ": record type %" PRIu32 " optype %" PRIu32
	    " txnid %" PRIu64 " fileid %" PRIu32,
	    log_file, log_offset, opcount,
	    rectype, optype, txnid, fileid);
	printf(" key size %zu value size %zu\n", key->size, value->size);
	if (rectype == WT_LOGREC_MESSAGE)
		printf("Application Record: %s\n", (char *)value->data);
}

/*
 * simple_walk_log --
 *	A simple walk of the log.
 */
static int
simple_walk_log(WT_SESSION *session, int count_min)
{
	WT_CURSOR *cursor;
	WT_ITEM logrec_key, logrec_value;
	uint64_t txnid;
	uint32_t fileid, log_file, log_offset, opcount, optype, rectype;
	int count, ret;

	/*! [log cursor open] */
	ret = session->open_cursor(session, "log:", NULL, NULL, &cursor);
	/*! [log cursor open] */

	count = 0;
	while ((ret = cursor->next(cursor)) == 0) {
		count++;
		/*! [log cursor get_key] */
		ret = cursor->get_key(cursor, &log_file, &log_offset, &opcount);
		/*! [log cursor get_key] */
		/*! [log cursor get_value] */
		ret = cursor->get_value(cursor, &txnid,
		    &rectype, &optype, &fileid, &logrec_key, &logrec_value);
		/*! [log cursor get_value] */

		print_record(log_file, log_offset, opcount,
		    rectype, optype, txnid, fileid, &logrec_key, &logrec_value);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;
	ret = cursor->close(cursor);
	if (count < count_min) {
		fprintf(stderr,
		    "Expected minimum %d records, found %d\n",
		    count_min, count);
		abort();
	}
	return (ret);
}
/*! [log cursor walk] */

static int
walk_log(WT_SESSION *session)
{
	WT_CONNECTION *wt_conn2;
	WT_CURSOR *cursor, *cursor2;
	WT_ITEM logrec_key, logrec_value;
	WT_SESSION *session2;
	uint64_t txnid;
	uint32_t fileid, opcount, optype, rectype;
	uint32_t log_file, log_offset, save_file, save_offset;
	int first, i, in_txn, ret;

	ret = setup_copy(&wt_conn2, &session2);
	ret = session->open_cursor(session, "log:", NULL, NULL, &cursor);
	ret = session2->open_cursor(session2, uri, NULL, "raw=true", &cursor2);
	i = 0;
	in_txn = 0;
	txnid = 0;
	save_file = save_offset = 0;
	while ((ret = cursor->next(cursor)) == 0) {
		ret = cursor->get_key(cursor, &log_file, &log_offset, &opcount);
		/*
		 * Save one of the LSNs we get back to search for it
		 * later.  Pick a later one because we want to walk from
		 * that LSN to the end (where the multi-step transaction
		 * was performed).  Just choose the record that is MAX_KEYS.
		 */
		if (++i == MAX_KEYS) {
			save_file = log_file;
			save_offset = log_offset;
		}
		ret = cursor->get_value(cursor, &txnid, &rectype,
		    &optype, &fileid, &logrec_key, &logrec_value);

		print_record(log_file, log_offset, opcount,
		    rectype, optype, txnid, fileid, &logrec_key, &logrec_value);

		/*
		 * If we are in a transaction and this is a new one, end
		 * the previous one.
		 */
		if (in_txn && opcount == 0) {
			ret = session2->commit_transaction(session2, NULL);
			in_txn = 0;
		}

		/*
		 * If the operation is a put, replay it here on the backup
		 * connection.
		 *
		 * !!!
		 * Minor cheat: the metadata is fileid 0, skip its records.
		 */
		if (fileid != 0 &&
		    rectype == WT_LOGREC_COMMIT && optype == WT_LOGOP_ROW_PUT) {
			if (!in_txn) {
				ret = session2->begin_transaction(session2,
				    NULL);
				in_txn = 1;
			}
			cursor2->set_key(cursor2, &logrec_key);
			cursor2->set_value(cursor2, &logrec_value);
			ret = cursor2->insert(cursor2);
		}
	}
	if (in_txn)
		ret = session2->commit_transaction(session2, NULL);

	ret = cursor2->close(cursor2);
	/*
	 * Compare the tables after replay.  They should be identical.
	 */
	if (compare_tables(session, session2))
		printf("compare failed\n");
	ret = session2->close(session2, NULL);
	ret = wt_conn2->close(wt_conn2, NULL);

	ret = cursor->reset(cursor);
	/*! [log cursor set_key] */
	cursor->set_key(cursor, save_file, save_offset, 0);
	/*! [log cursor set_key] */
	/*! [log cursor search] */
	ret = cursor->search(cursor);
	/*! [log cursor search] */
	printf("Reset to saved...\n");
	/*
	 * Walk all records starting with this key.
	 */
	first = 1;
	while ((ret = cursor->get_key(cursor,
	    &log_file, &log_offset, &opcount)) == 0) {
		if (first) {
			first = 0;
			if (save_file != log_file ||
			    save_offset != log_offset) {
				fprintf(stderr,
				    "search returned the wrong LSN\n");
				exit (1);
			}
		}
		ret = cursor->get_value(cursor, &txnid, &rectype,
		    &optype, &fileid, &logrec_key, &logrec_value);

		print_record(log_file, log_offset, opcount,
		    rectype, optype, txnid, fileid, &logrec_key, &logrec_value);

		ret = cursor->next(cursor);
		if (ret != 0)
			break;
	}
	ret = cursor->close(cursor);
	return (ret);
}

int
main(void)
{
	WT_CONNECTION *wt_conn;
	WT_CURSOR *cursor;
	WT_SESSION *session;
	int count_min, i, record_count, ret;
	char cmd_buf[256], k[16], v[16];

	count_min = 0;
	snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s %s && mkdir %s %s",
	    home1, home2, home1, home2);
	if ((ret = system(cmd_buf)) != 0) {
		fprintf(stderr, "%s: failed ret %d\n", cmd_buf, ret);
		return (EXIT_FAILURE);
	}
	if ((ret = wiredtiger_open(home1, NULL, CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home1, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = session->create(session, uri, "key_format=S,value_format=S");
	count_min++;

	ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
	/*
	 * Perform some operations with individual auto-commit transactions.
	 */
	for (record_count = 0, i = 0; i < MAX_KEYS; i++, record_count++) {
		snprintf(k, sizeof(k), "key%d", i);
		snprintf(v, sizeof(v), "value%d", i);
		cursor->set_key(cursor, k);
		cursor->set_value(cursor, v);
		ret = cursor->insert(cursor);
		count_min++;
	}
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
	ret = session->commit_transaction(session, NULL);
	count_min++;
	ret = cursor->close(cursor);

	/*! [log cursor printf] */
	ret = session->log_printf(session, "Wrote %d records", record_count);
	count_min++;
	/*! [log cursor printf] */

	/*
	 * Close and reopen the connection so that the log ends up with
	 * a variety of records such as file sync and checkpoint.  We
	 * have archiving turned off.
	 */
	ret = wt_conn->close(wt_conn, NULL);
	if ((ret = wiredtiger_open(home1, NULL, CONN_CONFIG, &wt_conn)) != 0) {
		fprintf(stderr, "Error connecting to %s: %s\n",
		    home1, wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	ret = wt_conn->open_session(wt_conn, NULL, NULL, &session);
	ret = simple_walk_log(session, count_min);
	ret = walk_log(session);
	ret = wt_conn->close(wt_conn, NULL);

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
