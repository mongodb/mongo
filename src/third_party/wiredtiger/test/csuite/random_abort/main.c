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
#include <signal.h>

static char home[PATH_MAX]; /* Program working dir */

/*
 * These two names for the URI and file system must be maintained in tandem.
 */
static const char *const col_uri = "table:col_main";
static const char *const uri = "table:main";
static bool compaction;
static bool compat;
static bool inmem;
static bool use_lazyfs;

static WT_LAZY_FS lazyfs;

#define MAX_TH 12
#define MIN_TH 5
#define MAX_TIME 40
#define MIN_TIME 10

#define OP_TYPE_DELETE 0
#define OP_TYPE_INSERT 1
#define OP_TYPE_MODIFY 2
#define MAX_NUM_OPS 3

#define DELETE_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "delete-records-%" PRIu32
#define INSERT_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "insert-records-%" PRIu32
#define MODIFY_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "modify-records-%" PRIu32

#define DELETE_RECORD_FILE_ID 0
#define INSERT_RECORD_FILE_ID 1
#define MODIFY_RECORD_FILE_ID 2
#define MAX_RECORD_FILES 3

#define ENV_CONFIG_DEF \
    "create,log=(file_max=10M,enabled),statistics=(all),statistics_log=(json,on_close,wait=1)"
#define ENV_CONFIG_TXNSYNC                                                                        \
    "create,log=(file_max=10M,enabled),"                                                          \
    "transaction_sync=(enabled,method=none),statistics=(all),statistics_log=(json,on_close,wait=" \
    "1)"
#define ENV_CONFIG_TXNSYNC_FSYNC                                                                   \
    "create,log=(file_max=10M,enabled),"                                                           \
    "transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(json,on_close,wait=" \
    "1)"

/*
 * A minimum width of 10, along with zero filling, means that all the keys sort according to their
 * integer value, making each thread's key space distinct.
 */
#define KEY_FORMAT ("%010" PRIu64)

/*
 * Maximum number of modifications that are allowed to perform cursor modify operation.
 */
#define MAX_MODIFY_ENTRIES 10

#define MAX_VAL 4096
/*
 * STR_MAX_VAL is set to MAX_VAL - 1 to account for the extra null character.
 */
#define STR_MAX_VAL "4095"

static void handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-T threads] [-Cclmpv]\n", progname);
    exit(EXIT_FAILURE);
}

typedef struct {
    WT_CONNECTION *conn;
    uint64_t start;
    uint32_t id;
} WT_THREAD_DATA;

/*
 * thread_run --
 *     TODO: Add a comment describing this function.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
    FILE *fp[MAX_RECORD_FILES];
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM data, newv;
    WT_MODIFY entries[MAX_MODIFY_ENTRIES];
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_THREAD_DATA *td;
    size_t lsize;
    size_t maxdiff, new_buf_size;
    uint64_t i;
    int nentries;
    char buf[MAX_VAL], fname[MAX_RECORD_FILES][64], new_buf[MAX_VAL];
    char kname[64], lgbuf[8];
    char large[128 * 1024];
    bool columnar_table;

    __wt_random_init_default(&rnd);
    for (i = 0; i < MAX_RECORD_FILES; i++)
        memset(fname[i], 0, sizeof(fname[i]));
    memset(buf, 0, sizeof(buf));
    memset(new_buf, 0, sizeof(new_buf));
    memset(kname, 0, sizeof(kname));
    lsize = sizeof(large);
    memset(large, 0, lsize);
    nentries = MAX_MODIFY_ENTRIES;
    columnar_table = false;

    td = (WT_THREAD_DATA *)arg;

    testutil_snprintf(fname[DELETE_RECORD_FILE_ID], sizeof(fname[DELETE_RECORD_FILE_ID]),
      DELETE_RECORDS_FILE, td->id);
    testutil_snprintf(fname[INSERT_RECORD_FILE_ID], sizeof(fname[INSERT_RECORD_FILE_ID]),
      INSERT_RECORDS_FILE, td->id);
    testutil_snprintf(fname[MODIFY_RECORD_FILE_ID], sizeof(fname[MODIFY_RECORD_FILE_ID]),
      MODIFY_RECORDS_FILE, td->id);

    /*
     * Set up a large value putting our id in it. Write it in there a bunch of times, but the rest
     * of the buffer can just be zero.
     */
    testutil_snprintf(lgbuf, sizeof(lgbuf), "th-%" PRIu32, td->id);
    for (i = 0; i < 128; i += strlen(lgbuf))
        testutil_snprintf(&large[i], lsize - i, "%s", lgbuf);
    /*
     * Keep a separate file with the records we wrote for checking.
     */
    for (i = 0; i < MAX_RECORD_FILES; i++) {
        (void)unlink(fname[i]);
        if ((fp[i] = fopen(fname[i], "w")) == NULL)
            testutil_die(errno, "fopen");
        /*
         * Set to line buffering. But that is advisory only. We've seen cases where the result files
         * end up with partial lines.
         */
        __wt_stream_set_line_buffer(fp[i]);
    }

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    /* Make alternate threads operate on the column-store table. */
    if (td->id % 2 != 0)
        columnar_table = true;

    if (columnar_table)
        testutil_check(session->open_cursor(session, col_uri, NULL, NULL, &cursor));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /*
     * Write our portion of the key space until we're killed.
     */
    printf("Thread %" PRIu32 " starts at %" PRIu64 "\n", td->id, td->start);
    for (i = td->start;; ++i) {
        /* Record number 0 is invalid for columnar store, check it. */
        if (i == 0)
            i++;

        /*
         * The value is the insert- with key appended.
         */
        testutil_snprintf(buf, sizeof(buf), "insert-%" PRIu64, i);

        if (columnar_table)
            cursor->set_key(cursor, i);
        else {
            testutil_snprintf(kname, sizeof(kname), KEY_FORMAT, i);
            cursor->set_key(cursor, kname);
        }
        /*
         * Every 30th record write a very large record that exceeds the log buffer size. This forces
         * us to use the unbuffered path.
         */
        if (i % 30 == 0) {
            data.size = 128 * 1024;
            data.data = large;
        } else {
            data.size = __wt_random(&rnd) % MAX_VAL;
            data.data = buf;
        }
        cursor->set_value(cursor, &data);
        while ((ret = cursor->insert(cursor)) == WT_ROLLBACK)
            ;
        testutil_assert(ret == 0);

        /*
         * Save the key separately for checking later.
         */
        if (fprintf(fp[INSERT_RECORD_FILE_ID], "%" PRIu64 "\n", i) == -1)
            testutil_die(errno, "fprintf");

        /*
         * If configured, run compaction on database after each epoch of 100000 operations.
         */
        if (compaction && i >= (100 * WT_THOUSAND) && i % (100 * WT_THOUSAND) == 0) {
            printf("Running compaction in Thread %" PRIu32 "\n", td->id);
            if (columnar_table)
                ret = session->compact(session, col_uri, NULL);
            else
                ret = session->compact(session, uri, NULL);
            /*
             * We may have several sessions trying to compact the same URI, in this case, EBUSY is
             * returned.
             */
            testutil_assert(ret == 0 || ret == EBUSY);
        }

        /*
         * Decide what kind of operation can be performed on the already inserted data.
         */
        if (i % MAX_NUM_OPS == OP_TYPE_DELETE) {
            if (columnar_table)
                cursor->set_key(cursor, i);
            else
                cursor->set_key(cursor, kname);

            while ((ret = cursor->remove(cursor)) == WT_ROLLBACK)
                ;
            testutil_assert(ret == 0);

            /* Save the key separately for checking later.*/
            if (fprintf(fp[DELETE_RECORD_FILE_ID], "%" PRIu64 "\n", i) == -1)
                testutil_die(errno, "fprintf");
        } else if (i % MAX_NUM_OPS == OP_TYPE_MODIFY) {
            testutil_snprintf(new_buf, sizeof(new_buf), "modify-%" PRIu64, i);
            new_buf_size = (data.size < MAX_VAL - 1 ? data.size : MAX_VAL - 1);

            newv.data = new_buf;
            newv.size = new_buf_size;
            maxdiff = MAX_VAL;

            /*
             * Make sure the modify operation is carried out in an snapshot isolation level with
             * explicit transaction.
             */
            do {
                testutil_check(session->begin_transaction(session, NULL));

                if (columnar_table)
                    cursor->set_key(cursor, i);
                else
                    cursor->set_key(cursor, kname);

                ret = wiredtiger_calc_modify(session, &data, &newv, maxdiff, entries, &nentries);
                if (ret == 0)
                    ret = cursor->modify(cursor, entries, nentries);
                else {
                    /*
                     * In case if we couldn't able to generate modify vectors, treat this change as
                     * a normal update operation.
                     */
                    cursor->set_value(cursor, &newv);
                    ret = cursor->update(cursor);
                }
                testutil_check(ret == 0 ? session->commit_transaction(session, NULL) :
                                          session->rollback_transaction(session, NULL));
            } while (ret == WT_ROLLBACK);
            testutil_assert(ret == 0);

            /*
             * Save the key and new value separately for checking later.
             */
            if (fprintf(fp[MODIFY_RECORD_FILE_ID], "%s %" PRIu64 "\n", new_buf, i) == -1)
                testutil_die(errno, "fprintf");
        } else if (i % MAX_NUM_OPS != OP_TYPE_INSERT)
            /* Dead code. To catch any op type misses */
            testutil_die(0, "Unsupported operation type.");
    }
    /* NOTREACHED */
}

static void fill_db(uint32_t) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * fill_db --
 *     Child process creates the database and table, and then creates worker threads to add data
 *     until it is killed by the parent.
 */
static void
fill_db(uint32_t nth)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_THREAD_DATA *td;
    wt_thread_t *thr;
    uint32_t i;
    char envconf[512];

    thr = dcalloc(nth, sizeof(*thr));
    td = dcalloc(nth, sizeof(WT_THREAD_DATA));
    if (chdir(home) != 0)
        testutil_die(errno, "Child chdir: %s", home);
    if (inmem)
        strcpy(envconf, ENV_CONFIG_DEF);
    else if (use_lazyfs)
        strcpy(envconf, ENV_CONFIG_TXNSYNC_FSYNC);
    else
        strcpy(envconf, ENV_CONFIG_TXNSYNC);
    if (compat)
        strcat(envconf, TESTUTIL_ENV_CONFIG_COMPAT);

    testutil_check(wiredtiger_open(WT_HOME_DIR, NULL, envconf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, col_uri, "key_format=r,value_format=u"));
    testutil_check(session->create(session, uri, "key_format=S,value_format=u"));
    testutil_check(session->close(session, NULL));

    printf("Create %" PRIu32 " writer threads\n", nth);
    for (i = 0; i < nth; ++i) {
        td[i].conn = conn;
        td[i].start = WT_BILLION * (uint64_t)i;
        td[i].id = i;
        testutil_check(__wt_thread_create(NULL, &thr[i], thread_run, &td[i]));
    }
    printf("Spawned %" PRIu32 " writer threads\n", nth);
    fflush(stdout);
    /*
     * The threads never exit, so the child will just wait here until it is killed.
     */
    for (i = 0; i < nth; ++i)
        testutil_check(__wt_thread_join(NULL, &thr[i]));
    /*
     * NOTREACHED
     */
    free(thr);
    free(td);
    _exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * handler --
 *     TODO: Add a comment describing this function.
 */
static void
handler(int sig)
{
    pid_t pid;

    WT_UNUSED(sig);
    pid = wait(NULL);

    /* Clean up LazyFS. */
    if (use_lazyfs)
        testutil_lazyfs_cleanup(&lazyfs);

    /* The core file will indicate why the child exited. Choose EINVAL here. */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * recover_and_verify --
 *     TODO: Add a comment describing this function.
 */
static int
recover_and_verify(uint32_t nthreads)
{
    FILE *fp[MAX_RECORD_FILES];
    WT_CONNECTION *conn;
    WT_CURSOR *col_cursor, *cursor, *row_cursor;
    WT_DECL_RET;
    WT_ITEM search_value;
    WT_SESSION *session;
    uint64_t absent, count, key, last_key, middle;
    uint32_t i, j;
    char file_value[MAX_VAL];
    char fname[MAX_RECORD_FILES][64], kname[64];
    bool columnar_table, fatal;

    printf("Open database, run recovery and verify content\n");
    testutil_check(wiredtiger_open(WT_HOME_DIR, NULL, TESTUTIL_ENV_CONFIG_REC, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, col_uri, NULL, NULL, &col_cursor));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &row_cursor));

    absent = count = 0;
    fatal = false;
    for (i = 0; i < nthreads; ++i) {

        /*
         * Every alternative thread is operated on column-store table. Make sure that proper cursor
         * is used for verification of recovered records.
         */
        if (i % 2 != 0) {
            columnar_table = true;
            cursor = col_cursor;
        } else {
            columnar_table = false;
            cursor = row_cursor;
        }

        middle = 0;
        testutil_snprintf(fname[DELETE_RECORD_FILE_ID], sizeof(fname[DELETE_RECORD_FILE_ID]),
          DELETE_RECORDS_FILE, i);
        if ((fp[DELETE_RECORD_FILE_ID] = fopen(fname[DELETE_RECORD_FILE_ID], "r")) == NULL)
            testutil_die(errno, "fopen: %s", fname[DELETE_RECORD_FILE_ID]);
        testutil_snprintf(fname[INSERT_RECORD_FILE_ID], sizeof(fname[INSERT_RECORD_FILE_ID]),
          INSERT_RECORDS_FILE, i);
        if ((fp[INSERT_RECORD_FILE_ID] = fopen(fname[INSERT_RECORD_FILE_ID], "r")) == NULL)
            testutil_die(errno, "fopen: %s", fname[INSERT_RECORD_FILE_ID]);
        testutil_snprintf(fname[MODIFY_RECORD_FILE_ID], sizeof(fname[MODIFY_RECORD_FILE_ID]),
          MODIFY_RECORDS_FILE, i);
        if ((fp[MODIFY_RECORD_FILE_ID] = fopen(fname[MODIFY_RECORD_FILE_ID], "r")) == NULL)
            testutil_die(errno, "fopen: %s", fname[MODIFY_RECORD_FILE_ID]);

        /*
         * For every key in the saved file, verify that the key exists in the table after recovery.
         * If we're doing in-memory log buffering we never expect a record missing in the middle,
         * but records may be missing at the end. If we did write-no-sync, we expect every key to
         * have been recovered.
         */
        for (last_key = UINT64_MAX;; ++count, last_key = key) {
            ret = fscanf(fp[INSERT_RECORD_FILE_ID], "%" SCNu64 "\n", &key);
            /*
             * Consider anything other than clear success in getting the key to be EOF. We've seen
             * file system issues where the file ends with zeroes on a 4K boundary and does not
             * return EOF but a ret of zero.
             */
            if (ret != 1)
                break;
            /*
             * If we're unlucky, the last line may be a partially written key at the end that can
             * result in a false negative error for a missing record. Detect it.
             */
            if (last_key != UINT64_MAX && key != last_key + 1) {
                printf("%s: Ignore partial record %" PRIu64 " last valid key %" PRIu64 "\n",
                  fname[INSERT_RECORD_FILE_ID], key, last_key);
                break;
            }

            if (key % MAX_NUM_OPS == OP_TYPE_DELETE) {
                /*
                 * If it is delete operation, make sure the record doesn't exist.
                 */
                ret = fscanf(fp[DELETE_RECORD_FILE_ID], "%" SCNu64 "\n", &key);

                /*
                 * Consider anything other than clear success in getting the key to be EOF. We've
                 * seen file system issues where the file ends with zeroes on a 4K boundary and does
                 * not return EOF but a ret of zero.
                 */
                if (ret != 1)
                    break;

                /*
                 * If we're unlucky, the last line may be a partially written key at the end that
                 * can result in a false negative error for a missing record. Detect it.
                 */
                if (last_key != UINT64_MAX && key <= last_key) {
                    printf("%s: Ignore partial record %" PRIu64 " last valid key %" PRIu64 "\n",
                      fname[DELETE_RECORD_FILE_ID], key, last_key);
                    break;
                }

                if (columnar_table)
                    cursor->set_key(cursor, key);
                else {
                    testutil_snprintf(kname, sizeof(kname), KEY_FORMAT, key);
                    cursor->set_key(cursor, kname);
                }

                while ((ret = cursor->search(cursor)) == WT_ROLLBACK)
                    ;
                if (ret != 0)
                    testutil_assert(ret == WT_NOTFOUND);
                else if (middle != 0) {
                    /*
                     * We should never find an existing key after we have detected one missing for
                     * the thread.
                     */
                    printf("%s: after missing record at %" PRIu64 " key %" PRIu64 " exists\n",
                      fname[DELETE_RECORD_FILE_ID], middle, key);
                    fatal = true;
                } else {
                    if (!inmem)
                        printf("%s: deleted record found with key %" PRIu64 "\n",
                          fname[DELETE_RECORD_FILE_ID], key);
                    absent++;
                    middle = key;
                }
            } else if (key % MAX_NUM_OPS == OP_TYPE_INSERT) {
                /*
                 * If it is insert only operation, make sure the record exists
                 */
                if (columnar_table)
                    cursor->set_key(cursor, key);
                else {
                    testutil_snprintf(kname, sizeof(kname), KEY_FORMAT, key);
                    cursor->set_key(cursor, kname);
                }

                while ((ret = cursor->search(cursor)) == WT_ROLLBACK)
                    ;
                if (ret != 0) {
                    testutil_assert(ret == WT_NOTFOUND);
                    if (!inmem)
                        printf("%s: no insert record with key %" PRIu64 "\n",
                          fname[INSERT_RECORD_FILE_ID], key);
                    absent++;
                    middle = key;
                } else if (middle != 0) {
                    /*
                     * We should never find an existing key after we have detected one missing for
                     * the thread.
                     */
                    printf("%s: after missing record at %" PRIu64 " key %" PRIu64 " exists\n",
                      fname[INSERT_RECORD_FILE_ID], middle, key);
                    fatal = true;
                }
            } else if (key % MAX_NUM_OPS == OP_TYPE_MODIFY) {
                /*
                 * If it is modify operation, make sure value of the fetched record matches with
                 * saved.
                 */
                ret = fscanf(
                  fp[MODIFY_RECORD_FILE_ID], "%" STR_MAX_VAL "s %" SCNu64 "\n", file_value, &key);

                /*
                 * Consider anything other than clear success in getting the key to be EOF. We've
                 * seen file system issues where the file ends with zeroes on a 4K boundary and does
                 * not return EOF but a ret of zero.
                 */
                if (ret != 2)
                    break;

                /*
                 * If we're unlucky, the last line may be a partially written key and value at the
                 * end that can result in a false negative error for a missing record. Detect the
                 * key.
                 */
                if (last_key != UINT64_MAX && key <= last_key) {
                    printf("%s: Ignore partial record %" PRIu64 " last valid key %" PRIu64 "\n",
                      fname[MODIFY_RECORD_FILE_ID], key, last_key);
                    break;
                }

                if (columnar_table)
                    cursor->set_key(cursor, key);
                else {
                    testutil_snprintf(kname, sizeof(kname), KEY_FORMAT, key);
                    cursor->set_key(cursor, kname);
                }

                while ((ret = cursor->search(cursor)) == WT_ROLLBACK)
                    ;
                if (ret != 0) {
                    testutil_assert(ret == WT_NOTFOUND);
                    if (!inmem)
                        printf("%s: no modified record with key %" PRIu64 "\n",
                          fname[MODIFY_RECORD_FILE_ID], key);
                    absent++;
                    middle = key;
                } else if (middle != 0) {
                    /*
                     * We should never find an existing key after we have detected one missing for
                     * the thread.
                     */
                    printf("%s: after missing record at %" PRIu64 " key %" PRIu64 " exists\n",
                      fname[MODIFY_RECORD_FILE_ID], middle, key);
                    fatal = true;
                } else {
                    testutil_check(cursor->get_value(cursor, &search_value));
                    if (strncmp(file_value, search_value.data, search_value.size) == 0)
                        continue;

                    if (!inmem)
                        /*
                         * Once the key exist in the database, there is no way that fetched data can
                         * mismatch with saved.
                         */
                        printf("%s: modified record with data mismatch key %" PRIu64 "\n",
                          fname[MODIFY_RECORD_FILE_ID], key);

                    absent++;
                    middle = key;
                }
            } else
                /* Dead code. To catch any op type misses */
                testutil_die(0, "Unsupported operation type.");
        }
        for (j = 0; j < MAX_RECORD_FILES; j++) {
            if (fclose(fp[j]) != 0)
                testutil_die(errno, "fclose");
        }
    }
    testutil_check(conn->close(conn, NULL));
    if (fatal)
        return (EXIT_FAILURE);
    if (!inmem && absent) {
        printf("%" PRIu64 " record(s) are missed from %" PRIu64 "\n", absent, count);
        return (EXIT_FAILURE);
    }
    printf("%" PRIu64 " records verified\n", count);
    return (EXIT_SUCCESS);
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct stat sb;
    WT_RAND_STATE rnd;
    pid_t pid;
    uint32_t i, j, nth, timeout;
    int ch, ret, status;
    char buf[PATH_MAX], fname[MAX_RECORD_FILES][64];
    char cwd_start[PATH_MAX]; /* The working directory when we started */
    const char *working_dir;
    bool preserve, rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    compaction = compat = inmem = false;
    use_lazyfs = lazyfs_is_implicitly_enabled();
    nth = MIN_TH;
    preserve = false;
    rand_th = rand_time = true;
    timeout = MIN_TIME;
    verify_only = false;
    working_dir = use_lazyfs ? "WT_TEST.random-abort-lazyfs" : "WT_TEST.random-abort";

    while ((ch = __wt_getopt(progname, argc, argv, "Cch:lmpT:t:v")) != EOF)
        switch (ch) {
        case 'C':
            compat = true;
            break;
        case 'c':
            compaction = true;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'l':
            use_lazyfs = true;
            break;
        case 'm':
            inmem = true;
            break;
        case 'p':
            preserve = true;
            break;
        case 'T':
            rand_th = false;
            nth = (uint32_t)atoi(__wt_optarg);
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    /* Home will be the WT_TEST dir that contains the WT_HOME database directory */
    testutil_work_dir_from_path(home, sizeof(home), working_dir);

    /*
     * If the user wants to verify they need to tell us how many threads there were so we can find
     * the old record files.
     */
    if (verify_only && rand_th) {
        fprintf(stderr, "Verify option requires specifying number of threads\n");
        exit(EXIT_FAILURE);
    }

    /* Remember the current working directory. */
    testutil_assert_errno(getcwd(cwd_start, sizeof(cwd_start)) != NULL);

    /* Create the database, run the test, and fail. */
    if (!verify_only) {
        /* Create the test's home directory. */
        testutil_recreate_dir(home);

        /* Set up the test subdirectories. */
        testutil_snprintf(buf, sizeof(buf), "%s/%s", home, RECORDS_DIR);
        testutil_mkdir(buf);
        testutil_snprintf(buf, sizeof(buf), "%s/%s", home, WT_HOME_DIR);
        testutil_mkdir(buf);

        /* Set up LazyFS. */
        if (use_lazyfs)
            testutil_lazyfs_setup(&lazyfs, home);

        /* Set up the rest of the test. */
        __wt_random_init(NULL, &rnd);
        if (rand_time) {
            timeout = __wt_random(&rnd) % MAX_TIME;
            if (timeout < MIN_TIME)
                timeout = MIN_TIME;
        }
        if (rand_th) {
            nth = __wt_random(&rnd) % MAX_TH;
            if (nth < MIN_TH)
                nth = MIN_TH;
        }
        printf("Parent: Compatibility %s in-mem log %s\n", compat ? "true" : "false",
          inmem ? "true" : "false");
        printf("Parent: Create %" PRIu32 " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
        printf("CONFIG: %s%s%s%s%s -h %s -T %" PRIu32 " -t %" PRIu32 "\n", progname,
          compat ? " -C" : "", compaction ? " -c" : "", use_lazyfs ? " -l" : "", inmem ? " -m" : "",
          working_dir, nth, timeout);

        /*
         * Fork a child to insert as many items. We will then randomly kill the child, run recovery
         * and make sure all items we wrote exist after recovery runs.
         */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
        testutil_assert_errno((pid = fork()) >= 0);

        if (pid == 0) { /* child */
            fill_db(nth);
            /* NOTREACHED */
        }

        /* parent */
        /*
         * Sleep for the configured amount of time before killing the child. Start the timeout from
         * the time we notice that the child workers have created their record files. That allows
         * the test to run correctly on really slow machines.
         */
        i = 0;
        while (i < nth) {
            for (j = 0; j < MAX_RECORD_FILES; j++) {
                /*
                 * Wait for each record file to exist.
                 */
                if (j == DELETE_RECORD_FILE_ID)
                    testutil_snprintf(fname[j], sizeof(fname[j]), DELETE_RECORDS_FILE, i);
                else if (j == INSERT_RECORD_FILE_ID)
                    testutil_snprintf(fname[j], sizeof(fname[j]), INSERT_RECORDS_FILE, i);
                else
                    testutil_snprintf(fname[j], sizeof(fname[j]), MODIFY_RECORDS_FILE, i);
                testutil_snprintf(buf, sizeof(buf), "%s/%s", home, fname[j]);
                while (stat(buf, &sb) != 0)
                    testutil_sleep_wait(1, pid);
            }
            ++i;
        }
        sleep(timeout);
        sa.sa_handler = SIG_DFL;
        testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

        /*
         * !!! It should be plenty long enough to make sure more than
         * one log file exists.  If wanted, that check would be added
         * here.
         */
        printf("Kill child\n");
        testutil_assert_errno(kill(pid, SIGKILL) == 0);
        testutil_assert_errno(waitpid(pid, &status, 0) != -1);
    }

    /*
     * !!! If we wanted to take a copy of the directory before recovery,
     * this is the place to do it.
     */

    /* Go inside the home directory (typically WT_TEST). */
    if (chdir(home) != 0)
        testutil_die(errno, "parent chdir: %s", home);

    if (!verify_only)
        /* Copy the data to the home directory for debugging purposes. */
        testutil_copy_data();

    /*
     * Clear the cache, if we are using LazyFS. Do this after we save the data for debugging
     * purposes, so that we can see what we might have lost. If we are using LazyFS, the underlying
     * directory shows the state that we'd get after we clear the cache.
     */
    if (!verify_only && use_lazyfs)
        testutil_lazyfs_clear_cache(&lazyfs);

    /*
     * Recover the database and verify whether all the records from all threads are present or not?
     */
    ret = recover_and_verify(nth);

    /*
     * Clean up.
     */

    /* Clean up the test directory. */
    if (ret == EXIT_SUCCESS && !preserve)
        /* Current working directory is home (aka WT_TEST) */
        testutil_clean_test_artifacts();

    /*
     * We are in the home directory (typically WT_TEST), which we intend to delete. Go to the start
     * directory. We do this to avoid deleting the current directory, which is disallowed on some
     * platforms.
     */
    if (chdir(cwd_start) != 0)
        testutil_die(errno, "root chdir: %s", home);

    /* Clean up LazyFS. */
    if (!verify_only && use_lazyfs)
        testutil_lazyfs_cleanup(&lazyfs);

    /* Delete the work directory. */
    if (ret == EXIT_SUCCESS && !preserve)
        testutil_remove(home);

    return (ret);
}
