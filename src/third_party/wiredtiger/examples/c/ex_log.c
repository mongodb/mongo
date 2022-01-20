/*-
 * Public Domain 2014-present MongoDB, Inc.
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
#include <test_util.h>

static const char *home1 = "WT_HOME_LOG_1";
static const char *home2 = "WT_HOME_LOG_2";

static const char *const uri = "table:logtest";

#define CONN_CONFIG "create,cache_size=100MB,log=(enabled=true,remove=false)"
#define MAX_KEYS 10

static void
setup_copy(WT_CONNECTION **wt_connp, WT_SESSION **sessionp)
{
    error_check(wiredtiger_open(home2, NULL, CONN_CONFIG, wt_connp));

    error_check((*wt_connp)->open_session(*wt_connp, NULL, NULL, sessionp));
    error_check((*sessionp)->create(*sessionp, uri, "key_format=S,value_format=S"));
}

static void
compare_tables(WT_SESSION *session, WT_SESSION *sess_copy)
{
    WT_CURSOR *cursor, *curs_copy;
    int ret;
    const char *key, *key_copy, *value, *value_copy;

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    error_check(sess_copy->open_cursor(sess_copy, uri, NULL, NULL, &curs_copy));

    while ((ret = cursor->next(cursor)) == 0) {
        error_check(curs_copy->next(curs_copy));
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));
        error_check(curs_copy->get_key(curs_copy, &key_copy));
        error_check(curs_copy->get_value(curs_copy, &value_copy));
        if (strcmp(key, key_copy) != 0 || strcmp(value, value_copy) != 0) {
            fprintf(stderr, "Mismatched: key %s, key_copy %s value %s value_copy %s\n", key,
              key_copy, value, value_copy);
            exit(1);
        }
    }
    scan_end_check(ret == WT_NOTFOUND);

    error_check(cursor->close(cursor));

    ret = curs_copy->next(curs_copy);
    scan_end_check(ret == WT_NOTFOUND);

    error_check(curs_copy->close(curs_copy));
}

/*! [log cursor walk] */
static void
print_record(uint32_t log_file, uint32_t log_offset, uint32_t opcount, uint32_t rectype,
  uint32_t optype, uint64_t txnid, uint32_t fileid, WT_ITEM *key, WT_ITEM *value)
{
    printf("LSN [%" PRIu32 "][%" PRIu32 "].%" PRIu32 ": record type %" PRIu32 " optype %" PRIu32
           " txnid %" PRIu64 " fileid %" PRIu32,
      log_file, log_offset, opcount, rectype, optype, txnid, fileid);
    printf(" key size %zu value size %zu\n", key->size, value->size);
    if (rectype == WT_LOGREC_MESSAGE)
        printf("Application Record: %s\n", (char *)value->data);
}

/*
 * simple_walk_log --
 *     A simple walk of the log.
 */
static void
simple_walk_log(WT_SESSION *session, int count_min)
{
    WT_CURSOR *cursor;
    WT_ITEM logrec_key, logrec_value;
    uint64_t txnid;
    uint32_t fileid, log_file, log_offset, opcount, optype, rectype;
    int count, ret;

    /*! [log cursor open] */
    error_check(session->open_cursor(session, "log:", NULL, NULL, &cursor));
    /*! [log cursor open] */

    count = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        count++;
        /*! [log cursor get_key] */
        error_check(cursor->get_key(cursor, &log_file, &log_offset, &opcount));
        /*! [log cursor get_key] */
        /*! [log cursor get_value] */
        error_check(cursor->get_value(
          cursor, &txnid, &rectype, &optype, &fileid, &logrec_key, &logrec_value));
        /*! [log cursor get_value] */

        print_record(log_file, log_offset, opcount, rectype, optype, txnid, fileid, &logrec_key,
          &logrec_value);
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));

    if (count < count_min) {
        fprintf(stderr, "Expected minimum %d records, found %d\n", count_min, count);
        exit(1);
    }
}
/*! [log cursor walk] */

static void
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

    setup_copy(&wt_conn2, &session2);
    error_check(session->open_cursor(session, "log:", NULL, NULL, &cursor));
    error_check(session2->open_cursor(session2, uri, NULL, "raw=true", &cursor2));
    i = 0;
    in_txn = 0;
    txnid = 0;
    save_file = save_offset = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &log_file, &log_offset, &opcount));
        /*
         * Save one of the LSNs we get back to search for it later. Pick a later one because we want
         * to walk from that LSN to the end (where the multi-step transaction was performed). Just
         * choose the record that is MAX_KEYS.
         */
        if (++i == MAX_KEYS) {
            save_file = log_file;
            save_offset = log_offset;
        }
        error_check(cursor->get_value(
          cursor, &txnid, &rectype, &optype, &fileid, &logrec_key, &logrec_value));

        print_record(log_file, log_offset, opcount, rectype, optype, txnid, fileid, &logrec_key,
          &logrec_value);

        /*
         * If we are in a transaction and this is a new one, end the previous one.
         */
        if (in_txn && opcount == 0) {
            error_check(session2->commit_transaction(session2, NULL));
            in_txn = 0;
        }

        /*
         * If the operation is a put, replay it here on the backup
         * connection.
         *
         * !!!
         * Minor cheat: the metadata is fileid 0, skip its records.
         */
        if (fileid != 0 && rectype == WT_LOGREC_COMMIT && optype == WT_LOGOP_ROW_PUT) {
            if (!in_txn) {
                error_check(session2->begin_transaction(session2, NULL));
                in_txn = 1;
            }
            cursor2->set_key(cursor2, &logrec_key);
            cursor2->set_value(cursor2, &logrec_value);
            error_check(cursor2->insert(cursor2));
        }
    }
    if (in_txn)
        error_check(session2->commit_transaction(session2, NULL));

    error_check(cursor2->close(cursor2));
    /*
     * Compare the tables after replay. They should be identical.
     */
    compare_tables(session, session2);
    error_check(session2->close(session2, NULL));
    error_check(wt_conn2->close(wt_conn2, NULL));

    error_check(cursor->reset(cursor));
    /*! [log cursor set_key] */
    cursor->set_key(cursor, save_file, save_offset, 0);
    /*! [log cursor set_key] */
    /*! [log cursor search] */
    error_check(cursor->search(cursor));
    /*! [log cursor search] */
    printf("Reset to saved...\n");
    /*
     * Walk all records starting with this key.
     */
    for (first = 1;;) {
        error_check(cursor->get_key(cursor, &log_file, &log_offset, &opcount));
        if (first) {
            first = 0;
            if (save_file != log_file || save_offset != log_offset) {
                fprintf(stderr, "search returned the wrong LSN\n");
                exit(1);
            }
        }
        error_check(cursor->get_value(
          cursor, &txnid, &rectype, &optype, &fileid, &logrec_key, &logrec_value));

        print_record(log_file, log_offset, opcount, rectype, optype, txnid, fileid, &logrec_key,
          &logrec_value);

        ret = cursor->next(cursor);
        if (ret != 0)
            break;
    }
    scan_end_check(ret == WT_NOTFOUND);

    error_check(cursor->close(cursor));
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *wt_conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int count_min, i, record_count;
    char cmd_buf[256], k[32], v[32];

    (void)argc; /* Unused variable */
    (void)testutil_set_progname(argv);

    count_min = 0;

    (void)snprintf(
      cmd_buf, sizeof(cmd_buf), "rm -rf %s %s && mkdir %s %s", home1, home2, home1, home2);
    error_check(system(cmd_buf));
    error_check(wiredtiger_open(home1, NULL, CONN_CONFIG, &wt_conn));

    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));
    error_check(session->create(session, uri, "key_format=S,value_format=S"));
    count_min++;

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    /*
     * Perform some operations with individual auto-commit transactions.
     */
    for (record_count = 0, i = 0; i < MAX_KEYS; i++, record_count++) {
        (void)snprintf(k, sizeof(k), "key%d", i);
        (void)snprintf(v, sizeof(v), "value%d", i);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        error_check(cursor->insert(cursor));
        count_min++;
    }
    error_check(session->begin_transaction(session, NULL));
    /*
     * Perform some operations within a single transaction.
     */
    for (i = MAX_KEYS; i < MAX_KEYS + 5; i++, record_count++) {
        (void)snprintf(k, sizeof(k), "key%d", i);
        (void)snprintf(v, sizeof(v), "value%d", i);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        error_check(cursor->insert(cursor));
    }
    error_check(session->commit_transaction(session, NULL));
    count_min++;
    error_check(cursor->close(cursor));

    /*! [log cursor printf] */
    error_check(session->log_printf(session, "Wrote %d records", record_count));
    /*! [log cursor printf] */
    count_min++;

    /*
     * Close and reopen the connection so that the log ends up with a variety of records such as
     * file sync and checkpoint. We have removal turned off.
     */
    error_check(wt_conn->close(wt_conn, NULL));
    error_check(wiredtiger_open(home1, NULL, CONN_CONFIG, &wt_conn));

    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));
    simple_walk_log(session, count_min);
    walk_log(session);
    error_check(wt_conn->close(wt_conn, NULL));

    return (EXIT_SUCCESS);
}
