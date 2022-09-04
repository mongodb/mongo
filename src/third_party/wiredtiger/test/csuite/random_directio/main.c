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

/*
 * This test simulates system crashes. It uses direct I/O, and currently
 * runs only on Linux.
 *
 * Our strategy is to run a subordinate 'writer' process that creates/modifies
 * data, including schema modifications. Every N seconds, asynchronously, we
 * send a stop signal to the writer and then copy (with direct I/O) the entire
 * contents of its database home to a new saved location where we can run and
 * verify the recovered home. Then we send a continue signal. We repeat this:
 *
 *   sleep N, STOP, copy, run recovery, CONTINUE
 *
 * which allows the writer to make continuing progress, while the main
 * process is verifying what's on disk.
 *
 * By using stop signal to suspend the process and copying with direct I/O,
 * we are roughly simulating a system crash, by seeing what's actually on
 * disk (not in file system buffer cache) at the moment that the copy is
 * made. It's not quite as harsh as a system crash, as suspending does not
 * halt writes that are in-flight. Still, it's a reasonable proxy for testing.
 *
 * In the main table, the keys look like:
 *
 *   xxxx:T:LARGE_STRING
 *
 * where xxxx represents an increasing decimal id (0 padded to 12 digits).
 * These ids are only unique per thread, so this key is the xxxx-th key
 * written by a thread.  T represents the thread id reduced to a single
 * hex digit. LARGE_STRING is a portion of a large string that includes
 * the thread id and a lot of spaces, over and over (see the large_buf
 * function).  When forming the key, the large string is truncated so
 * that the key is effectively padded to the right length.
 *
 * The key space for the main table is designed to be interleaved tightly
 * among all the threads.  The matching values in the main table are the
 * same, except with the xxxx string reversed.  So the keys and values
 * are the same size.
 *
 * There is also a reverse table where the keys/values are swapped.
 */

#include "test_util.h"
#include "util.h"

#include <signal.h>
#include <sys/wait.h>

static char home[1024]; /* Program working dir */

static const char *const uri_main = "table:main";
static const char *const uri_rev = "table:rev";

/*
 * The number of threads cannot be more than 16, we are using a hex digit to encode this in the key.
 */
#define MAX_TH 16
#define MIN_TH 5

#define MAX_TIME 40
#define MIN_TIME 10

#define LARGE_WRITE_SIZE (128 * 1024)
#define MIN_DATA_SIZE 30
#define DEFAULT_DATA_SIZE 50

#define DEFAULT_CYCLES 5
#define DEFAULT_INTERVAL 3

#define MAX_CKPT_INVL 6  /* Maximum interval between checkpoints */
#define MAX_FLUSH_INVL 4 /* Maximum interval between flush_tier calls */

#define KEY_SEP "_" /* Must be one char string */

#define ENV_CONFIG                       \
    "create,log=(file_max=10M,enabled)," \
    "transaction_sync=(enabled,method=%s)"
#define ENV_CONFIG_TIER \
    ",tiered_storage=(bucket=./bucket,bucket_prefix=pfx-,local_retention=2,name=dir_store)"
#define ENV_CONFIG_TIER_EXT                                  \
    ",extensions=(%s../../../ext/storage_sources/dir_store/" \
    "libwiredtiger_dir_store.so=(early_load=true))"
#define ENV_CONFIG_REC "log=(recover=on)"

/* 64 spaces */
#define SPACES "                                                                "

/*
 * Set the "schema operation frequency" higher to be less stressful for schema
 * operations.  With the current value, 100, there are sequences of schema
 * operations that are begun when the id is in the range 0 to 9, 100 to 109,
 * 200 to 209, etc. That is, 10 sequences per 100.  A higher number (say 1000)
 * means there are 10 sequences started per 1000.  A sequence of schema
 * operations lasts for 4 ids.  So, for example, if thread 3 is inserting id
 * 100 into the main table, an additional schema operation is done (creating a
 * table), and operations on this table continue (while other schema operations
 * continue).
 *
 * Starting at the insert of id 99 (which has no schema operations), here's
 * what will happen (for thread #3).
 *
 * insert k/v 99 into table:main      (with no additional schema operations)
 *
 * insert k/v 100 into table:main
 * create table:A100-3       (3 for thread #3)
 *
 * insert k/v 101 into table:main
 * insert into table:A100-3     (continuing the sequence)
 * create table:A101-3          (starts a new sequence)
 *
 * insert k/v 102 into table:main
 * rename table:A100-3 -> table:B100-3  (third step in sequence)
 * insert into table:A101-3             (second step in sequence)
 * create table:A102-3                  (starting new sequence)
 *
 * insert k/v 103 into table:main
 * update key in table:B100-3          (fourth step)
 * rename table:A101-3 -> table:B101-3 (third step)
 * insert into table:A102-3
 * create table:A103-3
 *
 * insert k/v 104 into table:main
 * drop table:B100-3                   (fifth and last step)
 * update key in table:B101-3          (fourth step)
 * rename table:A102-3 -> table:B102-3
 * insert into table:A103-3
 * create table:A104-3
 * ...
 *
 * This continues, with the last table created when k/v 109 is inserted into
 * table:main and the last sequence finishing at k/v 113.  Each clump above
 * separated by a blank line represents a transaction.  Meanwhile, other
 * threads are doing the same thing.  That stretch, from id 100 to id 113
 * that has schema operations happens again at id 200, assuming frequency
 * set to 100. So it is a good test of schema operations 'in flight'.
 */
#define SCHEMA_FREQUENCY_DEFAULT 100
static uint64_t schema_frequency;

#define TEST_STREQ(expect, got, message)                                 \
    do {                                                                 \
        if (!WT_STREQ(expect, got)) {                                    \
            printf("FAIL: %s: expect %s, got %s", message, expect, got); \
            testutil_assert(WT_STREQ(expect, got));                      \
        }                                                                \
    } while (0)

/*
 * Values for flags used in various places.
 */
#define SCHEMA_CREATE 0x0001u
#define SCHEMA_CREATE_CHECK 0x0002u
#define SCHEMA_DATA_CHECK 0x0004u
#define SCHEMA_DROP 0x0008u
#define SCHEMA_DROP_CHECK 0x0010u
#define SCHEMA_INTEGRATED 0x0020u
#define SCHEMA_RENAME 0x0040u
#define SCHEMA_VERBOSE 0x0080u
#define SCHEMA_ALL                                                                               \
    (SCHEMA_CREATE | SCHEMA_CREATE_CHECK | SCHEMA_DATA_CHECK | SCHEMA_DROP | SCHEMA_DROP_CHECK | \
      SCHEMA_INTEGRATED | SCHEMA_RENAME)
#define SCHEMA_MASK 0xffffu
#define TEST_CKPT 0x10000u
#define TEST_TIERED 0x20000u

extern int __wt_optind;
extern char *__wt_optarg;

static void handler(int);

typedef struct {
    WT_CONNECTION *conn;
    char *data;
    uint32_t datasize;
    uint32_t id;

    uint32_t flags; /* Uses SCHEMA_* values above */
} WT_THREAD_DATA;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print usage and exit.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  %-20s%s\n", "-B", "use tiered storage, requires -C checkpoint [false]");
    fprintf(stderr, "  %-20s%s\n", "-C", "use checkpoint [false]");
    fprintf(stderr, "  %-20s%s\n", "-d data_size", "approximate size of keys and values [1000]");
    fprintf(stderr, "  %-20s%s\n", "-f schema frequency",
      "restart schema sequence every frequency period [100]");
    fprintf(stderr, "  %-20s%s\n", "-h home", "WiredTiger home directory [WT_TEST.directio]");
    fprintf(
      stderr, "  %-20s%s\n", "-i interval", "interval timeout between copy/recover cycles [3]");
    fprintf(stderr, "  %-20s%s\n", "-m method", "sync method: fsync, dsync, none [none]");
    fprintf(stderr, "  %-20s%s\n", "-n num_cycles", "number of copy/recover cycles [5]");
    fprintf(stderr, "  %-20s%s\n", "-p", "populate only [false]");
    fprintf(stderr, "  %-20s%s\n", "-S arg1,arg2,...",
      "comma separated schema operations, from the following:");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "none", "no schema operations [default]");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "all", "all of the below operations, except verbose");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "create", "create tables");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "create_check",
      "newly created tables are checked (requires create)");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "data_check",
      "check contents of files for various ops (requires create)");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "integrated",
      "schema operations are integrated into main table transactions");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "rename", "rename tables (requires create)");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "drop", "drop tables (requires create)");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "drop_check",
      "after recovery, dropped tables are checked (requires drop)");
    fprintf(stderr, "  %-5s%-15s%s\n", "", "", "that they no longer exist (requires drop)");
    fprintf(
      stderr, "  %-5s%-15s%s\n", "", "verbose", "verbose print during schema operation checks,");
    fprintf(
      stderr, "  %-5s%-15s%s\n", "", "", "done after recovery, so does not effect test timing");
    fprintf(stderr, "  %-20s%s\n", "-T num_threads", "number of threads in writer [random]");
    fprintf(stderr, "  %-20s%s\n", "-t timeout", "initial timeout before first copy [random]");
    fprintf(stderr, "  %-20s%s\n", "-v", "verify only [false]");
    exit(EXIT_FAILURE);
}

/*
 * has_schema_operation --
 *     Return true if a schema operation should be performed for this id. See the comment above
 *     describing schema operation frequency.
 */
static bool
has_schema_operation(uint64_t id, uint32_t offset)
{
    return (id >= offset && (id - offset) % schema_frequency < 10);
}

/*
 * large_buf --
 *     Fill or check a large buffer.
 */
static void
large_buf(char *large, size_t lsize, uint32_t id, bool fill)
{
    size_t len;
    uint64_t i;
    char lgbuf[1024 + 20];

    /*
     * Set up a large value putting our id in it every 1024 bytes or so.
     */
    testutil_check(__wt_snprintf(lgbuf, sizeof(lgbuf),
      "th-%" PRIu32 "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", id, SPACES, SPACES, SPACES, SPACES, SPACES,
      SPACES, SPACES, SPACES, SPACES, SPACES, SPACES, SPACES, SPACES, SPACES, SPACES, SPACES));

    len = strlen(lgbuf);
    for (i = 0; i < lsize - len; i += len)
        if (fill)
            testutil_check(__wt_snprintf(&large[i], lsize - i, "%s", lgbuf));
        else
            testutil_check(strncmp(&large[i], lgbuf, len));
}

/*
 * reverse --
 *     Reverse a string in place.
 */
static void
reverse(char *s)
{
    size_t i, j, len;
    char tmp;

    len = strlen(s);
    for (i = 0, j = len - 1; i < len / 2; i++, j--) {
        tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
    }
}

/*
 * gen_kv --
 *     Generate a key/value.
 */
static void
gen_kv(char *buf, size_t buf_size, uint64_t id, uint32_t threadid, const char *large, bool forward)
{
    size_t keyid_size, large_size;
    char keyid[64];

    testutil_check(__wt_snprintf(keyid, sizeof(keyid), "%10.10" PRIu64, id));
    keyid_size = strlen(keyid);
    if (!forward)
        reverse(keyid);
    testutil_assert(keyid_size + 4 <= buf_size);
    large_size = (buf_size - 4) - keyid_size;
    testutil_check(__wt_snprintf(
      buf, buf_size, "%s" KEY_SEP "%1.1x" KEY_SEP "%.*s", keyid, threadid, (int)large_size, large));
}

/*
 * gen_table_name --
 *     Generate a table name used for the schema test.
 */
static void
gen_table_name(char *buf, size_t buf_size, uint64_t id, uint32_t threadid)
{
    testutil_check(__wt_snprintf(buf, buf_size, "table:A%" PRIu64 "-%" PRIu32, id, threadid));
}

/*
 * gen_table2_name --
 *     Generate a second table name used for the schema test.
 */
static void
gen_table2_name(char *buf, size_t buf_size, uint64_t id, uint32_t threadid, uint32_t flags)
{
    if (!LF_ISSET(SCHEMA_RENAME))
        /* table is not renamed, so use original table name */
        gen_table_name(buf, buf_size, id, threadid);
    else
        testutil_check(__wt_snprintf(buf, buf_size, "table:B%" PRIu64 "-%" PRIu32, id, threadid));
}

/*
 * schema_operation --
 *     TODO: Add a comment describing this function.
 */
static int
schema_operation(WT_SESSION *session, uint32_t threadid, uint64_t id, uint32_t op, uint32_t flags)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char uri1[50], uri2[50];
    const char *retry_opname;

    if (!has_schema_operation(id, op))
        return (0);

    id -= op;
    retry_opname = NULL;

    switch (op) {
    case 0:
        /* Create a table. */
        gen_table_name(uri1, sizeof(uri1), id, threadid);
        /*
        fprintf(stderr, "CREATE: %s\n", uri1);
        */
        testutil_check(session->log_printf(session, "CREATE: %s", uri1));
        testutil_check(session->create(session, uri1, "key_format=S,value_format=S"));
        testutil_check(session->log_printf(session, "CREATE: DONE %s", uri1));
        break;
    case 1:
        /* Insert a value into the table. */
        gen_table_name(uri1, sizeof(uri1), id, threadid);
        /*
        fprintf(stderr, "INSERT: %s\n", uri1);
        */
        testutil_check(session->open_cursor(session, uri1, NULL, NULL, &cursor));
        cursor->set_key(cursor, uri1);
        cursor->set_value(cursor, uri1);
        testutil_check(session->log_printf(session, "INSERT: %s", uri1));
        testutil_check(cursor->insert(cursor));
        testutil_check(session->log_printf(session, "INSERT: DONE %s", uri1));
        testutil_check(cursor->close(cursor));
        break;
    case 2:
        /* Rename the table. */
        if (LF_ISSET(SCHEMA_RENAME)) {
            gen_table_name(uri1, sizeof(uri1), id, threadid);
            gen_table2_name(uri2, sizeof(uri2), id, threadid, flags);
            retry_opname = "rename";
            /*
            fprintf(stderr, "RENAME: %s->%s\n", uri1, uri2);
            */
            testutil_check(session->log_printf(session, "RENAME: %s->%s", uri1, uri2));
            ret = session->rename(session, uri1, uri2, NULL);
            testutil_check(session->log_printf(session, "RENAME: DONE %s->%s", uri1, uri2));
        }
        break;
    case 3:
        /* Update the single value in the table. */
        gen_table_name(uri1, sizeof(uri1), id, threadid);
        gen_table2_name(uri2, sizeof(uri2), id, threadid, flags);
        testutil_check(session->open_cursor(session, uri2, NULL, NULL, &cursor));
        cursor->set_key(cursor, uri1);
        cursor->set_value(cursor, uri2);
        /*
        fprintf(stderr, "UPDATE: %s\n", uri2);
        */
        testutil_check(session->log_printf(session, "UPDATE: %s", uri2));
        testutil_check(cursor->update(cursor));
        testutil_check(session->log_printf(session, "UPDATE: DONE %s", uri2));
        testutil_check(cursor->close(cursor));
        break;
    case 4:
        /* Drop the table. */
        if (LF_ISSET(SCHEMA_DROP)) {
            gen_table2_name(uri1, sizeof(uri1), id, threadid, flags);
            retry_opname = "drop";
            /*
            fprintf(stderr, "DROP: %s\n", uri1);
            */
            testutil_check(session->log_printf(session, "DROP: %s", uri1));
            ret = session->drop(session, uri1, NULL);
            testutil_check(session->log_printf(session, "DROP: DONE %s", uri1));
        }
    }
    /*
     * XXX We notice occasional EBUSY errors from rename or drop, even though neither URI should be
     * used by any other thread. Report it, and retry.
     */
    if (retry_opname != NULL && ret == EBUSY)
        printf("%s(\"%s\", ....) failed, retrying transaction\n", retry_opname, uri1);
    else if (ret != 0) {
        printf("FAIL: %s(\"%s\", ....) returns %d: %s\n", retry_opname, uri1, ret,
          wiredtiger_strerror(ret));
        testutil_check(ret);
    }

    return (ret);
}

/*
 * thread_ckpt_run --
 *     Runner function for the checkpoint thread.
 */
static WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_THREAD_DATA *td;
    uint32_t sleep_time;
    int i;

    __wt_random_init(&rnd);

    td = (WT_THREAD_DATA *)arg;
    /*
     * Keep a separate file with the records we wrote for checking.
     */
    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    for (i = 1;; ++i) {
        sleep_time = __wt_random(&rnd) % MAX_CKPT_INVL;
        sleep(sleep_time);
        testutil_check(session->checkpoint(session, NULL));
        printf("Checkpoint %d complete.\n", i);
        fflush(stdout);
    }
    /* NOTREACHED */
}

/*
 * thread_flush_run --
 *     Runner function for the flush_tier thread.
 */
static WT_THREAD_RET
thread_flush_run(void *arg)
{
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_THREAD_DATA *td;
    uint32_t i, sleep_time;

    __wt_random_init(&rnd);

    td = (WT_THREAD_DATA *)arg;
    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    for (i = 1;; ++i) {
        sleep_time = __wt_random(&rnd) % MAX_FLUSH_INVL;
        sleep(sleep_time);
        /*
         * Currently not testing any of the flush tier configuration strings other than defaults. We
         * expect the defaults are what MongoDB wants for now.
         */
        testutil_check(session->flush_tier(session, NULL));
        printf("Flush tier %" PRIu32 " completed.\n", i);
        fflush(stdout);
    }
    /* NOTREACHED */
}
static WT_THREAD_RET thread_run(void *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * thread_run --
 *     Run a writer thread.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
    WT_CURSOR *cursor, *rev;
    WT_DECL_RET;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    WT_THREAD_DATA *td;
    size_t lsize;
    uint64_t i;
    uint32_t kvsize, op;
    char *buf1, *buf2;
    char large[LARGE_WRITE_SIZE];

    __wt_random_init(&rnd);
    lsize = sizeof(large);
    memset(large, 0, lsize);

    td = (WT_THREAD_DATA *)arg;
    large_buf(large, lsize, td->id, true);

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri_main, NULL, NULL, &cursor));
    testutil_check(session->open_cursor(session, uri_rev, NULL, NULL, &rev));

    /*
     * Split the allocated buffer into two parts, one for the key, one for the value.
     */
    kvsize = td->datasize / 2;
    buf1 = td->data;
    buf2 = &td->data[kvsize];

    /*
     * Continuing writing until we're killed.
     */
    printf("Thread %" PRIu32 "\n", td->id);
    for (i = 0;; ++i) {
again:
        /*
        if (i > 0 && i % 10000 == 0)
                printf("Thread %" PRIu32
                    " completed %" PRIu64 " entries\n",
                    td->id, i);
        */

        gen_kv(buf1, kvsize, i, td->id, large, true);
        gen_kv(buf2, kvsize, i, td->id, large, false);

        testutil_check(session->begin_transaction(session, NULL));
        cursor->set_key(cursor, buf1);
        /*
         * Every 1000th record write a very large value that exceeds the log buffer size. This
         * forces us to use the unbuffered path.
         */
        if (i % 1000 == 0) {
            cursor->set_value(cursor, large);
        } else {
            cursor->set_value(cursor, buf2);
        }
        testutil_check(cursor->insert(cursor));

        /*
         * The reverse table has no very large records.
         */
        rev->set_key(rev, buf2);
        rev->set_value(rev, buf1);
        testutil_check(rev->insert(rev));

        /*
         * If we are not running integrated tests, then we commit the transaction now so that schema
         * operations are not part of the transaction operations for the main table. If we are
         * running 'integrated' then we'll first do the schema operations and commit later.
         */
        if (!F_ISSET(td, SCHEMA_INTEGRATED))
            testutil_check(session->commit_transaction(session, NULL));
        /*
         * If we are doing a schema test, generate operations for additional tables. Each table has
         * a 'lifetime' of 4 values of the id.
         */
        if (F_ISSET(td, SCHEMA_ALL)) {
            /* Create is implied by any schema operation. */
            testutil_assert(F_ISSET(td, SCHEMA_CREATE));

            /*
             * Any or all of the schema operations may be performed as part of this transaction. See
             * the comment for schema operation frequency.
             */
            ret = 0;
            for (op = 0; op <= 4 && ret == 0; op++)
                ret = schema_operation(session, td->id, i, op, td->flags);
            if (ret == EBUSY) {
                /*
                 * Only rollback if integrated and we have an active transaction.
                 */
                if (F_ISSET(td, SCHEMA_INTEGRATED))
                    testutil_check(session->rollback_transaction(session, NULL));
                sleep(1);
                goto again;
            }
        }
        /*
         * If schema operations are integrated, commit the transaction now that they're complete.
         */
        if (F_ISSET(td, SCHEMA_INTEGRATED))
            testutil_check(session->commit_transaction(session, NULL));
    }
    /* NOTREACHED */
}

/*
 * create_db --
 *     Creates the database and tables so they are fully ready to be accessed by subordinate
 *     threads, and copied/recovered.
 */
static void
create_db(const char *method, uint32_t flags)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char envconf[512], tierconf[128];

    testutil_check(__wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG, method));
    if (LF_ISSET(TEST_TIERED)) {
        testutil_check(__wt_snprintf(tierconf, sizeof(tierconf), ENV_CONFIG_TIER_EXT, ""));
        strcat(envconf, tierconf);
        strcat(envconf, ENV_CONFIG_TIER);
    }

    printf("create_db: wiredtiger_open configuration: %s\n", envconf);
    testutil_check(wiredtiger_open(home, NULL, envconf, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri_main, "key_format=S,value_format=S"));
    testutil_check(session->create(session, uri_rev, "key_format=S,value_format=S"));
    /*
     * Checkpoint to help ensure that everything gets out to disk, so any direct I/O copy will have
     * at least have tables that can be opened.
     */
    testutil_check(session->checkpoint(session, NULL));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
}

static void fill_db(uint32_t, uint32_t, const char *, uint32_t)
  WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * fill_db --
 *     The child process creates worker threads to add data until it is killed by the parent.
 */
static void
fill_db(uint32_t nth, uint32_t datasize, const char *method, uint32_t flags)
{
    WT_CONNECTION *conn;
    WT_THREAD_DATA *td;
    wt_thread_t *thr;
    uint32_t ckpt_id, flush_id, i;
    char envconf[512], tierconf[128];

    /* Allocate number of threads plus two more for checkpoint and flush. */
    thr = dcalloc(nth + 2, sizeof(*thr));
    td = dcalloc(nth + 2, sizeof(WT_THREAD_DATA));
    if (chdir(home) != 0)
        testutil_die(errno, "Child chdir: %s", home);
    testutil_check(__wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG, method));
    if (LF_ISSET(TEST_TIERED)) {
        testutil_check(__wt_snprintf(tierconf, sizeof(tierconf), ENV_CONFIG_TIER_EXT, "../"));
        strcat(envconf, tierconf);
        strcat(envconf, ENV_CONFIG_TIER);
    }

    printf("fill_db: wiredtiger_open configuration: %s\n", envconf);
    testutil_check(wiredtiger_open(".", NULL, envconf, &conn));

    datasize += 1; /* Add an extra byte for string termination */
    printf(
      "Create %" PRIu32 " writer threads. Schema frequency %" PRIu64 "\n", nth, schema_frequency);
    for (i = 0; i < nth; ++i) {
        td[i].conn = conn;
        td[i].data = dcalloc(datasize, 1);
        td[i].datasize = datasize;
        td[i].id = i;
        td[i].flags = flags;
        testutil_check(__wt_thread_create(NULL, &thr[i], thread_run, &td[i]));
    }
    printf("Spawned %" PRIu32 " writer threads\n", nth);
    fflush(stdout);
    if (LF_ISSET(TEST_CKPT)) {
        ckpt_id = nth;
        td[ckpt_id].conn = conn;
        td[ckpt_id].id = ckpt_id;
        testutil_check(__wt_thread_create(NULL, &thr[ckpt_id], thread_ckpt_run, &td[ckpt_id]));
    }
    if (LF_ISSET(TEST_TIERED)) {
        flush_id = nth + 1;
        td[flush_id].conn = conn;
        td[flush_id].id = flush_id;
        testutil_check(__wt_thread_create(NULL, &thr[flush_id], thread_flush_run, &td[flush_id]));
    }
    /*
     * The threads never exit, so the child will just wait here until it is killed.
     */
    for (i = 0; i < nth; ++i) {
        testutil_check(__wt_thread_join(NULL, &thr[i]));
        free(td[i].data);
    }
    /*
     * NOTREACHED
     */
    free(thr);
    free(td);
    _exit(EXIT_SUCCESS);
}

/*
 * check_kv --
 *     Check that a key exists with a value, or does not exist.
 */
static void
check_kv(WT_CURSOR *cursor, const char *key, const char *value, bool exists)
{
    WT_DECL_RET;
    char *got;

    cursor->set_key(cursor, key);
    ret = cursor->search(cursor);
    if ((ret = cursor->search(cursor)) == WT_NOTFOUND) {
        if (exists) {
            printf("FAIL: expected rev file to have: %s\n", key);
            testutil_assert(!exists);
        }
    } else {
        testutil_check(ret);
        if (!exists) {
            printf("FAIL: unexpected key in rev file: %s\n", key);
            testutil_assert(exists);
        }
        testutil_check(cursor->get_value(cursor, &got));
        TEST_STREQ(value, got, "value");
    }
}

/*
 * check_dropped --
 *     Check that the uri has been dropped.
 */
static void
check_dropped(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    ret = session->open_cursor(session, uri, NULL, NULL, &cursor);
    testutil_assert(ret == WT_NOTFOUND);
}

/*
 * check_empty --
 *     Check that the uri exists and is empty.
 */
static void
check_empty(WT_SESSION *session, const char *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    ret = cursor->next(cursor);
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
}

/*
 * check_one_entry --
 *     Check that the uri exists and has one entry.
 */
static void
check_one_entry(WT_SESSION *session, const char *uri, const char *key, const char *value)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char *gotkey, *gotvalue;

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    testutil_check(cursor->next(cursor));
    testutil_check(cursor->get_key(cursor, &gotkey));
    testutil_check(cursor->get_value(cursor, &gotvalue));
    testutil_assert(WT_STREQ(key, gotkey));
    testutil_assert(WT_STREQ(value, gotvalue));
    ret = cursor->next(cursor);
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
}

/*
 * check_schema --
 *     Check that the database has the expected schema according to the last id seen for this
 *     thread.
 */
static void
check_schema(WT_SESSION *session, uint64_t lastid, uint32_t threadid, uint32_t flags)
{
    char uri[50], uri2[50];

    if (!LF_ISSET(SCHEMA_ALL) || !LF_ISSET(SCHEMA_INTEGRATED))
        return;

    if (LF_ISSET(SCHEMA_VERBOSE))
        fprintf(stderr, "check_schema(%" PRIu64 ", thread=%" PRIu32 ")\n", lastid, threadid);
    if (has_schema_operation(lastid, 0)) {
        /* Create table operation. */
        gen_table_name(uri, sizeof(uri), lastid, threadid);
        if (LF_ISSET(SCHEMA_VERBOSE))
            fprintf(stderr, " create %s\n", uri);
        if (LF_ISSET(SCHEMA_CREATE_CHECK))
            check_empty(session, uri);
    }
    if (has_schema_operation(lastid, 1)) {
        /* Insert value operation. */
        gen_table_name(uri, sizeof(uri), lastid - 1, threadid);
        if (LF_ISSET(SCHEMA_VERBOSE))
            fprintf(stderr, " insert %s\n", uri);
        if (LF_ISSET(SCHEMA_DATA_CHECK))
            check_one_entry(session, uri, uri, uri);
    }
    if (LF_ISSET(SCHEMA_RENAME) && has_schema_operation(lastid, 2)) {
        /* Table rename operation. */
        gen_table_name(uri, sizeof(uri), lastid - 2, threadid);
        gen_table2_name(uri2, sizeof(uri2), lastid - 2, threadid, flags);
        if (LF_ISSET(SCHEMA_VERBOSE))
            fprintf(stderr, " rename %s,%s\n", uri, uri2);
        if (LF_ISSET(SCHEMA_DROP_CHECK))
            check_dropped(session, uri);
        if (LF_ISSET(SCHEMA_CREATE_CHECK))
            check_one_entry(session, uri2, uri, uri);
    }
    if (has_schema_operation(lastid, 3)) {
        /* Value update operation. */
        gen_table_name(uri, sizeof(uri), lastid - 2, threadid);
        gen_table2_name(uri2, sizeof(uri2), lastid - 2, threadid, flags);
        if (LF_ISSET(SCHEMA_VERBOSE))
            fprintf(stderr, " update %s\n", uri2);
        if (LF_ISSET(SCHEMA_DATA_CHECK))
            check_one_entry(session, uri2, uri, uri2);
    }
    if (LF_ISSET(SCHEMA_DROP_CHECK) && has_schema_operation(lastid, 4)) {
        /* Drop table operation. */
        gen_table2_name(uri2, sizeof(uri2), lastid - 2, threadid, flags);
        if (LF_ISSET(SCHEMA_VERBOSE))
            fprintf(stderr, " drop %s\n", uri2);
        check_dropped(session, uri2);
    }
}

/*
 * kill_child --
 *     TODO: Add a comment describing this function.
 */
static void
kill_child(pid_t pid)
{
    int status;

    /*
     * The child is stopped, it won't process an abort until it is continued. First signal the
     * abort, then signal continue so that the child process will process the abort and dump core.
     */
    printf("Send abort to child process ID %d\n", (int)pid);
    testutil_assert_errno(kill(pid, SIGABRT) == 0);
    testutil_assert_errno(kill(pid, SIGCONT) == 0);
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);
}

/*
 * check_db --
 *     Make a copy of the database and verify its contents.
 */
static bool
check_db(uint32_t nth, uint32_t datasize, pid_t pid, bool directio, uint32_t flags)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor, *meta, *rev;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t gotid, id;
    uint64_t *lastid;
    uint32_t gotth, kvsize, th, threadmap;
    char checkdir[4096], dbgdir[4096], envconf[512], savedir[4096], tierconf[128];
    char *gotkey, *gotvalue, *keybuf, *p;
    char **large_arr;

    keybuf = dcalloc(datasize, 1);
    lastid = dcalloc(nth, sizeof(uint64_t));

    large_arr = dcalloc(nth, sizeof(char *));
    for (th = 0; th < nth; th++) {
        large_arr[th] = dcalloc(LARGE_WRITE_SIZE, 1);
        large_buf(large_arr[th], LARGE_WRITE_SIZE, th, true);
    }
    testutil_check(__wt_snprintf(checkdir, sizeof(checkdir), "../%s.CHECK", home));
    testutil_check(__wt_snprintf(dbgdir, sizeof(savedir), "../%s.DEBUG", home));
    testutil_check(__wt_snprintf(savedir, sizeof(savedir), "../%s.SAVE", home));

    /*
     * We make a copy of the directory (possibly using direct I/O) for recovery and checking, and an
     * identical copy that keeps the state of all files before recovery starts.
     */
    printf(
      "Copy database home directory using direct I/O to run recovery,\n"
      "along with a saved 'pre-recovery' copy.\n");
    /*
     * Copy the original home directory explicitly without direct I/O. Copy this first because
     * copying with directio may abort and we want to see what the original copy saw.
     */
    copy_directory(home, dbgdir, false);
    copy_directory(home, checkdir, directio);
    copy_directory(checkdir, savedir, false);

    printf("Open database, run recovery and verify content\n");
    testutil_check(__wt_snprintf(envconf, sizeof(envconf), ENV_CONFIG_REC));
    if (LF_ISSET(TEST_TIERED)) {
        testutil_check(__wt_snprintf(tierconf, sizeof(tierconf), ENV_CONFIG_TIER_EXT, ""));
        strcat(envconf, tierconf);
        strcat(envconf, ENV_CONFIG_TIER);
    }
    ret = wiredtiger_open(checkdir, NULL, envconf, &conn);
    /* If this fails, abort the child process before we die so we can see what it was doing. */
    if (ret != 0) {
        if (pid != 0)
            kill_child(pid);
        testutil_check(ret);
    }
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, uri_main, NULL, NULL, &cursor));
    testutil_check(session->open_cursor(session, uri_rev, NULL, NULL, &rev));
    kvsize = datasize / 2;

/*
 * We're most interested in the final records on disk. Rather than walk all records, we do a quick
 * scan to find the last complete set of written ids. Each thread writes each id, along with the
 * thread id, so they are interleaved. Once we have the neighborhood where some keys may be missing,
 * we'll back up to do a scan from that point.
 */
#define CHECK_INCR 1000
    for (id = 0;; id += CHECK_INCR) {
        gen_kv(keybuf, kvsize, id, 0, large_arr[0], true);
        cursor->set_key(cursor, keybuf);
        if ((ret = cursor->search(cursor)) == WT_NOTFOUND)
            break;
        testutil_check(ret);
        for (th = 1; th < nth; th++) {
            gen_kv(keybuf, kvsize, id, th, large_arr[th], true);
            cursor->set_key(cursor, keybuf);
            if ((ret = cursor->search(cursor)) == WT_NOTFOUND)
                break;
            testutil_check(ret);
        }
        if (ret == WT_NOTFOUND)
            break;
    }
    if (id < CHECK_INCR * 2)
        id = 0;
    else
        id -= CHECK_INCR * 2;

    printf("starting full scan at %" PRIu64 "\n", id);
    gen_kv(keybuf, kvsize, id, 0, large_arr[0], true);
    cursor->set_key(cursor, keybuf);
    th = 0;

    /* Keep bitmap of "active" threads. */
    threadmap = (0x1U << nth) - 1;
    for (ret = cursor->search(cursor); ret != WT_NOTFOUND && threadmap != 0;
         ret = cursor->next(cursor)) {
        testutil_check(ret);
        testutil_check(cursor->get_key(cursor, &gotkey));
        gotid = (uint64_t)strtol(gotkey, &p, 10);
        testutil_assert(*p == KEY_SEP[0]);
        p++;
        testutil_assert(isxdigit((unsigned char)*p));
        if (isdigit((unsigned char)*p))
            gotth = (uint32_t)(*p - '0');
        else if (*p >= 'a' && *p <= 'f')
            gotth = (uint32_t)((*p - 'a') + 10);
        else
            gotth = (uint32_t)((*p - 'A') + 10);
        p++;
        testutil_assert(*p == KEY_SEP[0]);
        p++;

        /*
         * See if the expected thread has finished at this point. If so, remove it from the thread
         * map.
         */
        while (gotth != th) {
            if ((threadmap & (0x1U << th)) != 0) {
                threadmap &= ~(0x1U << th);
                lastid[th] = id - 1;
                /*
                 * Any newly removed value in the main table should not be present as a key in the
                 * reverse table, since they were transactionally inserted at the same time.
                 */
                gen_kv(keybuf, kvsize, id, th, large_arr[th], false);
                check_kv(rev, keybuf, NULL, false);
                check_schema(session, id - 1, th, flags);
            }
            th = (th + 1) % nth;
            if (th == 0)
                id++;
        }
        testutil_assert(gotid == id);
        /*
         * Check that the key and value fully match.
         */
        gen_kv(keybuf, kvsize, id, th, large_arr[th], true);
        gen_kv(&keybuf[kvsize], kvsize, id, th, large_arr[th], false);
        testutil_check(cursor->get_value(cursor, &gotvalue));
        TEST_STREQ(keybuf, gotkey, "main table key");

        /*
         * Every 1000th record is large.
         */
        if (id % 1000 == 0)
            TEST_STREQ(large_arr[th], gotvalue, "main table large value");
        else
            TEST_STREQ(&keybuf[kvsize], gotvalue, "main table value");

        /*
         * Check the reverse file, with key/value reversed.
         */
        check_kv(rev, &keybuf[kvsize], keybuf, true);

        check_schema(session, id, th, flags);

        /* Bump thread number and id to the next expected key. */
        th = (th + 1) % nth;
        if (th == 0)
            id++;
    }
    printf("scanned to %" PRIu64 "\n", id);

    if (LF_ISSET(SCHEMA_ALL)) {
        /*
         * Check metadata to see if there are any tables present that shouldn't be there.
         */
        testutil_check(session->open_cursor(session, "metadata:", NULL, NULL, &meta));
        while ((ret = meta->next(meta)) != WT_NOTFOUND) {
            testutil_check(ret);
            testutil_check(meta->get_key(meta, &gotkey));
            /*
             * Names involved in schema testing are of the form:
             *   table:Axxx-t
             *   table:Bxxx-t
             * xxx corresponds to the id inserted into the main
             * table when the table was created, and t corresponds
             * to the thread id that did this.
             */
            if (WT_PREFIX_SKIP(gotkey, "table:") && (*gotkey == 'A' || *gotkey == 'B')) {
                gotid = (uint64_t)strtol(gotkey + 1, &p, 10);
                testutil_assert(*p == '-');
                th = (uint32_t)strtol(p + 1, &p, 10);
                testutil_assert(*p == '\0');
                /*
                 * If table operations are truly transactional, then there shouldn't be any extra
                 * files that unaccounted for.
                 */
                if (LF_ISSET(SCHEMA_DROP_CHECK))
                    testutil_assert(gotid == lastid[th]);
            }
        }
        testutil_check(meta->close(meta));
    }

    testutil_check(cursor->close(cursor));
    testutil_check(rev->close(rev));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    for (th = 0; th < nth; th++)
        free(large_arr[th]);
    free(large_arr);
    free(keybuf);
    free(lastid);
    return (true);
}

/*
 * handler --
 *     Child signal handler
 */
static void
handler(int sig)
{
    pid_t pid;
    int status, termsig;

    WT_UNUSED(sig);
    testutil_assert_errno((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) != -1);
    if (pid == 0)
        return; /* Nothing to wait for. */
    if (WIFSTOPPED(status))
        return;
    if (WIFSIGNALED(status)) {
        termsig = WTERMSIG(status);
        if (termsig == SIGCONT || termsig == SIGSTOP)
            return;
        printf("Child got signal %d (status = %d, 0x%x)\n", termsig, status, (u_int)status);
#ifdef WCOREDUMP
        if (WCOREDUMP(status))
            printf("Child process id=%" PRIuMAX " created core file\n", (uintmax_t)pid);
#endif
    }

    /*
     * The core file will indicate why the child exited. Choose EINVAL here.
     */
    testutil_die(EINVAL, "Child process %" PRIuMAX " abnormally exited, status=%d (0x%x)",
      (uintmax_t)pid, status, (u_int)status);
}

/*
 * has_direct_io --
 *     Check for direct I/O support.
 */
static bool
has_direct_io(void)
{
#ifdef O_DIRECT
    return (true);
#else
    return (false);
#endif
}

/*
 * main --
 *     Top level test.
 */
int
main(int argc, char *argv[])
{
    struct sigaction sa;
    WT_RAND_STATE rnd;
    pid_t pid;
    size_t size;
    uint32_t datasize, flags, i, interval, ncycles, nth, timeout;
    int ch, status;
    char *arg, *p;
    char args[1024], buf[1024];
    const char *method, *working_dir;
    bool populate_only, preserve, rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    datasize = DEFAULT_DATA_SIZE;
    nth = MIN_TH;
    ncycles = DEFAULT_CYCLES;
    rand_th = rand_time = true;
    timeout = MIN_TIME;
    interval = DEFAULT_INTERVAL;
    flags = 0;
    populate_only = preserve = verify_only = false;
    working_dir = "WT_TEST.random-directio";
    method = "none";
    pid = 0;
    schema_frequency = SCHEMA_FREQUENCY_DEFAULT;
    memset(args, 0, sizeof(args));

    if (!has_direct_io()) {
        fprintf(stderr,
          "**** test_random_directio: this system does "
          "not support direct I/O.\n**** Skipping test.\n");
        return (EXIT_SUCCESS);
    }
    for (i = 0, p = args; i < (uint32_t)argc; i++) {
        testutil_check(
          __wt_snprintf_len_set(p, sizeof(args) - (size_t)(p - args), &size, " %s", argv[i]));
        p += size;
    }
    while ((ch = __wt_getopt(progname, argc, argv, "BCd:f:h:i:m:n:PpS:T:t:v")) != EOF)
        switch (ch) {
        case 'B':
            LF_SET(TEST_TIERED);
            break;
        case 'C':
            LF_SET(TEST_CKPT);
            break;
        case 'd':
            datasize = (uint32_t)atoi(__wt_optarg);
            if (datasize > LARGE_WRITE_SIZE || datasize < MIN_DATA_SIZE) {
                fprintf(stderr, "-d value is larger than maximum %" PRId32 "\n", LARGE_WRITE_SIZE);
                return (EXIT_FAILURE);
            }
            break;
        case 'f':
            schema_frequency = (uint64_t)atoi(__wt_optarg);
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'i':
            interval = (uint32_t)atoi(__wt_optarg);
            break;
        case 'm':
            method = __wt_optarg;
            if (!WT_STREQ(method, "fsync") && !WT_STREQ(method, "dsync") &&
              !WT_STREQ(method, "none")) {
                fprintf(stderr, "-m option requires fsync|dsync|none\n");
                return (EXIT_FAILURE);
            }
            break;
        case 'n':
            ncycles = (uint32_t)atoi(__wt_optarg);
            break;
        case 'p':
            populate_only = true;
            break;
        case 'P':
            preserve = true;
            break;
        case 'S':
            p = __wt_optarg;
            while ((arg = strtok_r(p, ",", &p)) != NULL) {
                if (WT_STREQ(arg, "all"))
                    LF_SET(SCHEMA_ALL);
                else if (WT_STREQ(arg, "create"))
                    LF_SET(SCHEMA_CREATE);
                else if (WT_STREQ(arg, "create_check"))
                    LF_SET(SCHEMA_CREATE_CHECK);
                else if (WT_STREQ(arg, "data_check"))
                    LF_SET(SCHEMA_DATA_CHECK);
                else if (WT_STREQ(arg, "drop"))
                    LF_SET(SCHEMA_DROP);
                else if (WT_STREQ(arg, "drop_check"))
                    LF_SET(SCHEMA_DROP_CHECK);
                else if (WT_STREQ(arg, "integrated"))
                    LF_SET(SCHEMA_INTEGRATED);
                else if (WT_STREQ(arg, "none"))
                    flags = flags & ~SCHEMA_MASK;
                else if (WT_STREQ(arg, "rename"))
                    LF_SET(SCHEMA_RENAME);
                else if (WT_STREQ(arg, "verbose"))
                    LF_SET(SCHEMA_VERBOSE);
                else {
                    fprintf(stderr, "Unknown -S arg '%s'\n", arg);
                    usage();
                }
            }
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

    if (LF_ISSET(TEST_TIERED) && !LF_ISSET(TEST_CKPT))
        usage();

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    /*
     * If the user wants to verify they need to tell us how many threads there were so we know what
     * records we can expect.
     */
    if (verify_only && rand_th) {
        fprintf(stderr, "Verify option requires specifying number of threads\n");
        return (EXIT_FAILURE);
    }
    if ((LF_ISSET(SCHEMA_RENAME | SCHEMA_DROP | SCHEMA_CREATE_CHECK | SCHEMA_DATA_CHECK) &&
          !LF_ISSET(SCHEMA_CREATE)) ||
      (LF_ISSET(SCHEMA_DROP_CHECK) && !LF_ISSET(SCHEMA_DROP))) {
        fprintf(stderr, "Schema operations incompatible\n");
        usage();
    }
    if (!LF_ISSET(SCHEMA_INTEGRATED) &&
      LF_ISSET(SCHEMA_CREATE_CHECK | SCHEMA_DATA_CHECK | SCHEMA_DROP_CHECK)) {
        fprintf(stderr, "Schema '*check' options cannot be used without 'integrated'\n");
        usage();
    }
    printf("CONFIG:%s\n", args);
    if (!verify_only) {
        testutil_check(__wt_snprintf(buf, sizeof(buf), "rm -rf %s", home));
        if ((status = system(buf)) < 0)
            testutil_die(status, "system: %s", buf);
        testutil_make_work_dir(home);
        if (LF_ISSET(TEST_TIERED)) {
            testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/bucket", home));
            testutil_make_work_dir(buf);
        }

        __wt_random_init_seed(NULL, &rnd);
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
        printf("Parent: Create %" PRIu32 " threads; sleep %" PRIu32 " seconds\n", nth, timeout);

        create_db(method, flags);
        if (!populate_only) {
            /*
             * Fork a child to insert as many items. We will then randomly suspend the child, run
             * recovery and make sure all items we wrote exist after recovery runs.
             */
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = handler;
            testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
            testutil_assert_errno((pid = fork()) >= 0);
        }
        if (pid == 0) { /* child, or populate_only */
            fill_db(nth, datasize, method, flags);
            /* NOTREACHED */
        }

        /* parent */
        /*
         * Sleep for the configured amount of time before killing the child.
         */
        testutil_sleep_wait(timeout, pid);

        /*
         * Begin our cycles of suspend, copy, recover.
         */
        for (i = 0; i < ncycles; i++) {
            printf("Beginning cycle %" PRIu32 "/%" PRIu32 "\n", i + 1, ncycles);
            if (i != 0)
                testutil_sleep_wait(interval, pid);
            printf("Suspend child\n");
            if (kill(pid, SIGSTOP) != 0)
                testutil_die(errno, "kill");
            printf("Check DB\n");
            fflush(stdout);
            if (!check_db(nth, datasize, pid, true, flags))
                return (EXIT_FAILURE);
            if (kill(pid, SIGCONT) != 0)
                testutil_die(errno, "kill");
            printf("\n");
        }

        printf("Kill child\n");
        sa.sa_handler = SIG_DFL;
        testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);
        testutil_assert_errno(kill(pid, SIGKILL) == 0);
        testutil_assert_errno(waitpid(pid, &status, 0) != -1);
    }
    if (verify_only && !check_db(nth, datasize, 0, false, flags)) {
        printf("FAIL\n");
        return (EXIT_FAILURE);
    }
    printf("SUCCESS\n");

    if (!preserve) {
        testutil_clean_test_artifacts(home);
        testutil_clean_work_dir(home);
    }

    return (EXIT_SUCCESS);
}
