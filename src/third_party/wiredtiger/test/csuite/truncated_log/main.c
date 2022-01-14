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
 */

#include "test_util.h"

#include <sys/wait.h>

static char home[1024]; /* Program working dir */
static const char *const uri = "table:main";
static bool use_columns = false;

#define RECORDS_FILE "records"

#define ENV_CONFIG                                      \
    "create,log=(file_max=100K,archive=false,enabled)," \
    "transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_REC "log=(recover=on)"

#define LOG_FILE_1 "WiredTigerLog.0000000001"

#define K_SIZE 16
#define V_SIZE 256

static void write_and_read_new(WT_SESSION *);

/*
 * write_and_read_new --
 *     Write a new log record into the log via log print, then open up a log cursor and walk the log
 *     to make sure we can read it. The reason for this test is that if there is a partial log
 *     record at the end of the previous log file and truncate does not exist, this tests that we
 *     can still read past that record.
 */
static void
write_and_read_new(WT_SESSION *session)
{
    WT_CURSOR *logc;
    WT_ITEM logrec_key, logrec_value;
    uint64_t txnid;
    uint32_t fileid, log_file, log_offset, opcount, optype, rectype;
    bool saw_msg;

    /*
     * Write a log record and force it to disk so we can read it.
     */
    printf("Write log_printf record and verify.\n");
    testutil_check(session->log_printf(session, "Test Log Record"));
    testutil_check(session->log_flush(session, "sync=on"));
    testutil_check(session->open_cursor(session, "log:", NULL, NULL, &logc));
    saw_msg = false;
    while (logc->next(logc) == 0) {
        /*
         * We don't really need to get the key, but in case we want the LSN for some message, get
         * it.
         */
        testutil_check(logc->get_key(logc, &log_file, &log_offset, &opcount));
        testutil_check(
          logc->get_value(logc, &txnid, &rectype, &optype, &fileid, &logrec_key, &logrec_value));
        /*
         * We should never see a record from us in log file 2. We wrote a record there, but then the
         * record in log file 1 was truncated to be a partial record, ending the log there. So
         * everything after that, including everything in log file 2, is invalid until we get to log
         * file 3 which is where the post-recovery records will be written. The one exception in log
         * file two is the system record for the previous log file's LSN. Although it is written by
         * the system, we do walk it when using a cursor.
         */
        if (log_file == 2 && rectype != WT_LOGREC_SYSTEM)
            testutil_die(EINVAL, "Found LSN in Log 2");
#if 0
		printf("LSN [%" PRIu32 "][%" PRIu32 "].%" PRIu32
		    ": record type %" PRIu32 " optype %" PRIu32
		    " txnid %" PRIu64 " fileid %" PRIu32 "\n",
		    log_file, log_offset, opcount,
		    rectype, optype, txnid, fileid);
#endif
        if (rectype == WT_LOGREC_MESSAGE) {
            saw_msg = true;
            printf("Application Record: %s\n", (char *)logrec_value.data);
            break;
        }
    }
    testutil_check(logc->close(logc));
    if (!saw_msg)
        testutil_die(EINVAL, "Did not traverse log printf record");
}

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir]\n", progname);
    exit(EXIT_FAILURE);
}

static void fill_db(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * fill_db --
 *     Child process creates the database and table, and then writes data into the table until it
 *     switches into log file 2.
 */
static void
fill_db(void)
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor, *logc;
    WT_LSN lsn, save_lsn;
    WT_SESSION *session;
    uint32_t i, max_key, min_key, units, unused;
    char k[K_SIZE], v[V_SIZE];
    const char *table_config;
    bool first;

    if (use_columns)
        table_config = "key_format=r,value_format=S";
    else
        table_config = "key_format=S,value_format=S";

    /*
     * Run in the home directory so that the records file is in there too.
     */
    if (chdir(home) != 0)
        testutil_die(errno, "chdir: %s", home);
    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, table_config));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

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
     * Write data into the table until we move to log file 2. We do the calculation below so that we
     * don't have to walk the log for every record.
     *
     * Calculate about how many records should fit in the log file. Subtract a bunch for metadata
     * and file creation records. Then subtract out a few more records to be conservative.
     */
    units = (K_SIZE + V_SIZE) / 128 + 1;
    min_key = 90000 / (units * 128) - 15;
    max_key = min_key * 2;
    first = true;
    for (i = 0; i < max_key; ++i) {
        if (use_columns)
            cursor->set_key(cursor, i + 1);
        else {
            testutil_check(__wt_snprintf(k, sizeof(k), "key%03" PRIu32, i));
            cursor->set_key(cursor, k);
        }
        testutil_check(
          __wt_snprintf(v, sizeof(v), "value%0*" PRIu32, (int)(V_SIZE - (strlen("value") + 1)), i));
        cursor->set_value(cursor, v);
        testutil_check(cursor->insert(cursor));

        /*
         * Walking the ever growing log can be slow, so only start looking for the cross into log
         * file 2 after a minimum.
         */
        if (i > min_key) {
            testutil_check(session->open_cursor(session, "log:", NULL, NULL, &logc));
            if (save_lsn.l.file != 0) {
                logc->set_key(logc, save_lsn.l.file, save_lsn.l.offset, 0);
                testutil_check(logc->search(logc));
            }
            while (logc->next(logc) == 0) {
                testutil_check(logc->get_key(logc, &lsn.l.file, &lsn.l.offset, &unused));
                /*
                 * Save the LSN so that we know the offset of the last LSN in log file 1 later.
                 */
                if (lsn.l.file < 2)
                    save_lsn = lsn;
                else {
                    /*
                     * If this is the first time through that the key is larger than the minimum key
                     * and we're already in log file 2 then we did not calculate correctly and the
                     * test should fail.
                     */
                    if (first)
                        testutil_die(EINVAL, "min_key too high");
                    if (fprintf(fp, "%" PRIu32 " %" PRIu32 "\n", save_lsn.l.offset, i - 1) == -1)
                        testutil_die(errno, "fprintf");
                    break;
                }
            }
            first = false;
            testutil_check(logc->close(logc));
        }
    }
    if (fclose(fp) != 0)
        testutil_die(errno, "fclose");
    _exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    FILE *fp;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    pid_t pid;
    uint64_t new_offset, offset;
    uint32_t count, max_key;
    int ch, ret, status;
    const char *working_dir;
    bool preserve;

    preserve = false;
    (void)testutil_set_progname(argv);

    working_dir = "WT_TEST.truncated-log";
    while ((ch = __wt_getopt(progname, argc, argv, "ch:")) != EOF)
        switch (ch) {
        case 'c':
            /* Variable-length columns only (for now) */
            use_columns = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'p':
            preserve = true;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_make_work_dir(home);

    /*
     * Fork a child to do its work. Wait for it to exit.
     */
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* child */
        fill_db();
        /* NOTREACHED */
    }

    /* parent */
    /* Wait for child to kill itself. */
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);

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
     * The offset is the beginning of the last record. Truncate to the middle of that last record
     * (i.e. ahead of that offset).
     */
    if (offset > UINT64_MAX - V_SIZE)
        testutil_die(ERANGE, "offset");
    new_offset = offset + V_SIZE;
    printf("Parent: Log file 1: Key %" PRIu32 " at %" PRIu64 "\n", max_key, offset);
    printf("Parent: Truncate mid-record to %" PRIu64 "\n", new_offset);
    if (truncate(LOG_FILE_1, (wt_off_t)new_offset) != 0)
        testutil_die(errno, "truncate");

    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /*
     * For every key in the saved file, verify that the key exists in the table after recovery.
     * Since we did write-no-sync, we expect every key to have been recovered.
     */
    count = 0;
    while (cursor->next(cursor) == 0)
        ++count;
    /*
     * The max key in the saved file is the key we truncated, but the key space starts at 0 and
     * we're counting the records here, so we expect the max key number of records. Add one for the
     * system record for the previous LSN that the cursor will see too.
     */
    if (count > (max_key + 1)) {
        printf("expected %" PRIu32 " records found %" PRIu32 "\n", max_key, count);
        return (EXIT_FAILURE);
    }
    printf("%" PRIu32 " records verified\n", count);

    /*
     * Write a log record and then walk the log to make sure we can read that log record that is
     * beyond the truncated record.
     */
    write_and_read_new(session);
    testutil_check(conn->close(conn, NULL));
    if (!preserve) {
        /* At this point $PATH is inside `home`, which we intend to delete. cd to the parent dir. */
        if (chdir("../") != 0)
            testutil_die(errno, "root chdir: %s", home);
        testutil_clean_work_dir(home);
    }

    return (EXIT_SUCCESS);
}
