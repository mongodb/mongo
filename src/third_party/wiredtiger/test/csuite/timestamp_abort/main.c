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
#include <dirent.h>
#include <signal.h>

static char home[1024]; /* Program working dir */

/*
 * Create three tables that we will write the same data to and verify that
 * all the types of usage have the expected data in them after a crash and
 * recovery.  We want:
 * 1. A table that is logged and is not involved in timestamps.  This table
 * simulates a user local table.
 * 2. A table that is logged and involved in timestamps.  This simulates
 * the oplog.
 * 3. A table that is not logged and involved in timestamps.  This simulates
 * a typical collection file.
 *
 * We also create a fourth table that is not logged and not involved directly
 * in timestamps to store the stable timestamp.  That way we can know what the
 * latest stable timestamp is on checkpoint.
 *
 * We also create several files that are not WiredTiger tables.  The checkpoint
 * thread creates a file indicating that a checkpoint has completed.  The parent
 * process uses this to know when at least one checkpoint is done and it can
 * start the timer to abort.
 *
 * Each worker thread creates its own records file that records the data it
 * inserted and it records the timestamp that was used for that insertion.
 *
 * This program can be also used to test backups in the presence of failures.
 * It first creates a full backup, after which it periodically creates
 * an incremental backup.  Unlike other tests, each backup is created in a new
 * directory (as opposed to "patching" the previous backup), because it is
 * more robust and easier to reason about.  For example, if we crash while
 * creating a backup, we would still have good backups from which we can
 * recover.  Similarly, if we crash in the middle of taking a checkpoint after
 * a backup, the database may not have properly recorded the ID of the last
 * snapshot.
 */

#define BACKUP_BASE "backup."
#define CHECK_BASE "check."
#define INVALID_KEY UINT64_MAX
#define MAX_BACKUP_INVL 5 /* Maximum interval between backups */
#define MAX_CKPT_INVL 5   /* Maximum interval between checkpoints */
#define MAX_TH 200        /* Maximum configurable threads */
#define MAX_TIME 40
#define MAX_VAL 1024
#define MIN_TH 5
#define MIN_TIME 10
#define PREPARE_DURABLE_AHEAD_COMMIT 10
#define PREPARE_FREQ 5
#define PREPARE_PCT 10
#define PREPARE_YIELD (PREPARE_FREQ * 10)
#define RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "records-%" PRIu32
/* Include worker threads and prepare extra sessions */
#define SESSION_MAX (MAX_TH + 3 + MAX_TH * PREPARE_PCT)
#define STAT_WAIT 1
#define USEC_STAT (50 * WT_THOUSAND)

static const char *table_pfx = "table";
static const char *const uri_collection = "collection";
static const char *const uri_local = "local";
static const char *const uri_oplog = "oplog";
static const char *const uri_shadow = "shadow";

static const char *const ckpt_file = "checkpoint_done";

static bool backup_verify_immediately, backup_verify_quick;
static bool columns, stress, use_backups, use_lazyfs, use_ts;
static uint32_t backup_full_interval, backup_granularity_kb;

static TEST_OPTS *opts, _opts;

static int recover_and_verify(uint32_t backup_index, uint32_t workload_iteration);
extern int __wt_optind;
extern char *__wt_optarg;

/*
 * Print that we are doing backup verification.
 */
#define PRINT_BACKUP_VERIFY(index)                                     \
    printf("--- %s: Verify backup ID%" PRIu32 "%s\n", __func__, index, \
      backup_verify_quick ? " (quick)" : "");
#define PRINT_BACKUP_VERIFY_DONE(index)                                      \
    printf("--- DONE: %s: Verify backup ID%" PRIu32 "%s\n", __func__, index, \
      backup_verify_quick ? " (quick)" : "");

/*
 * The configuration sets the eviction update and dirty targets at 20% so that on average, each
 * thread can have a couple of dirty pages before eviction threads kick in. See below where these
 * symbols are used for cache sizing - we'll have about 10 pages allocated per thread. On the other
 * side, the eviction update and dirty triggers are 90%, so application threads aren't involved in
 * eviction until we're close to running out of cache.
 */
#define ENV_CONFIG_ADD_EVICT_DIRTY ",eviction_dirty_target=20,eviction_dirty_trigger=90"
#define ENV_CONFIG_ADD_STRESS ",timing_stress_for_test=[prepare_checkpoint_delay]"

#define ENV_CONFIG_DEF                                        \
    "cache_size=%" PRIu32                                     \
    "M,create,"                                               \
    "debug_mode=(table_logging=true,checkpoint_retention=5)," \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "log=(enabled,file_max=10M,remove=true),session_max=%d,"  \
    "statistics=(all),statistics_log=(wait=%d,json,on_close)"
#define ENV_CONFIG_TXNSYNC \
    ENV_CONFIG_DEF         \
    ",transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_TXNSYNC_FSYNC \
    ENV_CONFIG_DEF               \
    ",transaction_sync=(enabled,method=fsync)"

/*
 * A minimum width of 10, along with zero filling, means that all the keys sort according to their
 * integer value, making each thread's key space distinct. For column-store we just use the integer
 * values and that has the same effect.
 */
#define KEY_STRINGFORMAT ("%010" PRIu64)

#define SHARED_PARSE_OPTIONS "b:CmP:h:p"

/*
 * We reserve timestamps for each thread for the entire run. The timestamp for the i-th key that a
 * thread writes is given by the macro below. In a given iteration for each thread, there are three
 * timestamps available, though we don't always use the third. The first is used to timestamp the
 * transaction at the beginning. The second is used to timestamp after an insert is done. Then, we
 * sometimes want the durable timestamp ahead of the commit timestamp, so we reserve the last
 * timestamp for that use.
 */
#define RESERVED_TIMESTAMPS_FOR_ITERATION(td, iter) \
    ((uint64_t)WT_BILLION * (td)->workload_iteration + ((iter)*nth + (td)->threadnum) * 3 + 1)

/* The index of a backup. */
#define BACKUP_INDEX(td, sequence_number) \
    ((td)->workload_iteration * WT_THOUSAND + (sequence_number))

/* Get back the workload iteration number from a backup index. */
#define BACKUP_INDEX_TO_ITERATION(index) ((index) / WT_THOUSAND)

/* Get back the sequence number from a backup index. */
#define BACKUP_INDEX_TO_SEQUENCE(index) ((index) % WT_THOUSAND)

typedef struct {
    uint64_t absent_key; /* Last absent key */
    uint64_t exist_key;  /* First existing key after miss */
    uint64_t first_key;  /* First key in range */
    uint64_t first_miss; /* First missing key */
    uint64_t last_key;   /* Last key in range */
} REPORT;

typedef struct {
    WT_CONNECTION *conn;
    uint64_t start;
    uint32_t threadnum;
    uint32_t workload_iteration;
    WT_RAND_STATE data_rnd;
    WT_RAND_STATE extra_rnd;
} THREAD_DATA;

static uint32_t nth;                      /* Number of threads. */
static wt_timestamp_t *active_timestamps; /* Oldest timestamps still in use. */

static void handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

static void handle_conn_close(void);
static void handle_conn_ready(WT_CONNECTION *);
static int handle_general(WT_EVENT_HANDLER *, WT_CONNECTION *, WT_SESSION *, WT_EVENT_TYPE, void *);

static WT_CONNECTION *stat_conn = NULL;
static WT_SESSION *stat_session = NULL;
static volatile bool stat_run = false;
static wt_thread_t stat_th;

static WT_EVENT_HANDLER my_event = {NULL, NULL, NULL, NULL, handle_general};

/*
 * __int_comparator --
 *     "int" comparator.
 */
static int
__int_comparator(const void *a, const void *b)
{
    return (*(int *)a - *(int *)b);
}

/*
 * stat_func --
 *     Function to run with the early connection and gather statistics.
 */
static WT_THREAD_RET
stat_func(void *arg)
{
    WT_CURSOR *stat_c;
    int64_t last, value;
    const char *desc, *pvalue;

    WT_UNUSED(arg);
    testutil_assert(stat_conn != NULL);
    testutil_check(stat_conn->open_session(stat_conn, NULL, NULL, &stat_session));
    desc = pvalue = NULL;
    /* Start last and value at different numbers so we print the first value, likely 0. */
    last = -1;
    value = 0;
    while (stat_run) {
        testutil_check(stat_session->open_cursor(stat_session, "statistics:", NULL, NULL, &stat_c));

        /* Pick some statistic that is likely changed during recovery RTS. */
        stat_c->set_key(stat_c, WT_STAT_CONN_TXN_RTS_PAGES_VISITED);
        testutil_check(stat_c->search(stat_c));
        testutil_check(stat_c->get_value(stat_c, &desc, &pvalue, &value));
        testutil_check(stat_c->close(stat_c));
        if (desc != NULL && value != last)
            printf("%s: %" PRId64 "\n", desc, value);
        last = value;
        usleep(USEC_STAT);
    }
    testutil_check(stat_session->close(stat_session, NULL));
    return (WT_THREAD_RET_VALUE);
}

/*
 * handle_conn_close --
 *     Function to handle connection close callbacks from WiredTiger.
 */
static void
handle_conn_close(void)
{
    /*
     * Signal the statistics thread to exit and clear the global connection. This function cannot
     * return until the user thread stops using the connection.
     */
    stat_run = false;
    testutil_check(__wt_thread_join(NULL, &stat_th));
    stat_conn = NULL;
}

/*
 * handle_conn_ready --
 *     Function to handle connection ready callbacks from WiredTiger.
 */
static void
handle_conn_ready(WT_CONNECTION *conn)
{
    int unused;

    /*
     * Set the global connection for statistics and then start a statistics thread.
     */
    unused = 0;
    testutil_assert(stat_conn == NULL);
    memset(&stat_th, 0, sizeof(stat_th));
    stat_conn = conn;
    stat_run = true;
    testutil_check(__wt_thread_create(NULL, &stat_th, stat_func, (void *)&unused));
}

/*
 * handle_general --
 *     Function to handle general event callbacks.
 */
static int
handle_general(WT_EVENT_HANDLER *handler, WT_CONNECTION *conn, WT_SESSION *session,
  WT_EVENT_TYPE type, void *arg)
{
    WT_UNUSED(handler);
    WT_UNUSED(session);
    WT_UNUSED(arg);

    if (type == WT_EVENT_CONN_CLOSE)
        handle_conn_close();
    else if (type == WT_EVENT_CONN_READY)
        handle_conn_ready(conn);
    return (0);
}

/*
 * usage --
 *     Print usage help for the program.
 */
static void
usage(void)
{
    fprintf(stderr,
      "usage: %s %s [-F full-backup-interval] [-I iterations] [-T threads] [-t time] "
      "[-BCcLlsvz]\n",
      progname, opts->usage);
    exit(EXIT_FAILURE);
}

/*
 * thread_ts_run --
 *     Runner function for a timestamp thread.
 */
static WT_THREAD_RET
thread_ts_run(void *arg)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    THREAD_DATA *td;
    wt_timestamp_t last_ts, ts;
    uint64_t last_reconfig, now;
    uint32_t rand_op;
    int dbg;
    char tscfg[64];

    td = (THREAD_DATA *)arg;
    conn = td->conn;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    __wt_seconds((WT_SESSION_IMPL *)session, &last_reconfig);
    /* Update the oldest/stable timestamps every 1 millisecond. */
    for (last_ts = 0;; __wt_sleep(0, WT_THOUSAND)) {
        /* Get the last committed timestamp periodically in order to update the oldest
         * timestamp. */
        ts = maximum_stable_ts(active_timestamps, nth);
        if (ts == last_ts)
            continue;
        last_ts = ts;

        /* Let the oldest timestamp lag 25% of the time. */
        rand_op = __wt_random(&td->extra_rnd) % 4;
        if (rand_op == 1)
            testutil_check(__wt_snprintf(tscfg, sizeof(tscfg), "stable_timestamp=%" PRIx64, ts));
        else
            testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
              "oldest_timestamp=%" PRIx64 ",stable_timestamp=%" PRIx64, ts, ts));
        testutil_check(conn->set_timestamp(conn, tscfg));

        /*
         * Only perform the reconfigure test after statistics have a chance to run. If we do it too
         * frequently then internal servers like the statistics server get destroyed and restarted
         * too fast to do any work.
         */
        __wt_seconds((WT_SESSION_IMPL *)session, &now);
        if (now > last_reconfig + STAT_WAIT + 1) {
            /*
             * Set and reset the checkpoint retention setting on a regular basis. We want to test
             * racing with the internal log removal thread while we're here.
             */
            dbg = __wt_random(&td->extra_rnd) % 2;
            if (dbg == 0)
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "debug_mode=(checkpoint_retention=0)"));
            else
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "debug_mode=(checkpoint_retention=5)"));
            testutil_check(conn->reconfigure(conn, tscfg));
            last_reconfig = now;
        }
    }
    /* NOTREACHED */
}

/*
 * set_flush_tier_delay --
 *     Set up a random delay for the next flush_tier.
 */
static void
set_flush_tier_delay(WT_RAND_STATE *rnd)
{
    /*
     * We are checkpointing with a random interval up to MAX_CKPT_INVL seconds, and we'll do a flush
     * tier randomly every 0-10 seconds.
     */
    opts->tiered_flush_interval_us = __wt_random(rnd) % (10 * WT_MILLION + 1);
}

/*
 * backup_create_full --
 *     Perform a full backup.
 */
static void
backup_create_full(WT_CONNECTION *conn, bool consolidate, uint32_t index)
{
    FILE *fp;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int nfiles, ret;
    char backup_home[PATH_MAX];
    char buf[4096];
    char *filename;
    const char *cons;

    nfiles = 0;

    printf("Create full backup %" PRIu32 " - start: consolidate=%d, granularity=%" PRIu32 "KB\n",
      index, consolidate, backup_granularity_kb);

    /* Prepare the directory. */
    testutil_check(__wt_snprintf(backup_home, sizeof(backup_home), BACKUP_BASE "%" PRIu32, index));
    testutil_system("rm -rf %s && mkdir %s", backup_home, backup_home);

    /* Open the session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the backup cursor to get the list of files to copy. */
    if (consolidate)
        cons = ",consolidate=true";
    else
        cons = ",consolidate=false";
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "incremental=(granularity=%" PRIu32 "K,enabled=true,%s,this_id=ID%" PRIu32 ")",
      backup_granularity_kb, cons, index));
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    /* Copy the files. */
    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));
        testutil_system("cp %s/%s %s/%s", WT_HOME_DIR, filename, backup_home, filename);
    }
    testutil_assert(ret == WT_NOTFOUND);

    /* Cleanup */
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));

    /* Remember that the backup finished successfully. */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/done", backup_home));
    testutil_assert_errno((fp = fopen(buf, "w")) != NULL);
    testutil_assert_errno(fclose(fp) == 0);

    printf("Create full backup %" PRIu32 " - complete: files=%" PRId32 "\n", index, nfiles);
}

/*
 * backup_create_incremental --
 *     Perform an incremental backup.
 */
static void
backup_create_incremental(WT_CONNECTION *conn, uint32_t src_index, uint32_t index)
{
    FILE *fp;
    WT_CURSOR *cursor, *file_cursor;
    WT_SESSION *session;
    ssize_t rdsize;
    uint64_t offset, size, type;
    int rfd, ret, wfd, nfiles, nranges, nunmodified;
    char backup_home[PATH_MAX], src_backup_home[PATH_MAX];
    char buf[4096];
    char *filename;
    bool first_range;
    void *tmp;

    nfiles = 0;
    nranges = 0;
    nunmodified = 0;

    testutil_check(
      __wt_snprintf(src_backup_home, sizeof(src_backup_home), BACKUP_BASE "%" PRIu32, src_index));

    printf("Create incremental backup %" PRIu32 " - start: source=%" PRIu32 "\n", index, src_index);

    /* Prepare the directory. */
    testutil_check(__wt_snprintf(backup_home, sizeof(backup_home), BACKUP_BASE "%" PRIu32, index));
    testutil_system("rm -rf %s && mkdir %s", backup_home, backup_home);

    /* Open the session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the backup cursor to get the list of files to copy. */
    testutil_check(__wt_snprintf(buf, sizeof(buf),
      "incremental=(src_id=ID%" PRIu32 ",this_id=ID%" PRIu32 ")", src_index, index));
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    /* Process the files. */
    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));

        /* Process ranges within each file. */
        testutil_check(__wt_snprintf(buf, sizeof(buf), "incremental=(file=%s)", filename));
        testutil_check(session->open_cursor(session, NULL, cursor, buf, &file_cursor));

        first_range = true;
        rfd = wfd = 0;
        while ((ret = file_cursor->next(file_cursor)) == 0) {
            testutil_check(file_cursor->get_key(file_cursor, &offset, &size, &type));
            testutil_assert(type == WT_BACKUP_FILE || type == WT_BACKUP_RANGE);

            if (type == WT_BACKUP_RANGE) {
                /*
                 * If this is the first range, copy the base file from the source backup. Then open
                 * the source and target files to set up the rest of the copy.
                 */
                if (first_range) {
                    first_range = false;
                    testutil_system(
                      "cp %s/%s %s/%s", src_backup_home, filename, backup_home, filename);
                    printf(
                      "INCR: cp %s/%s %s/%s\n", src_backup_home, filename, backup_home, filename);
                    fflush(stdout);

                    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", WT_HOME_DIR, filename));
                    testutil_assert((rfd = open(buf, O_RDONLY, 0666)) >= 0);

                    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", backup_home, filename));
                    testutil_assert((wfd = open(buf, O_WRONLY | O_CREAT, 0666)) >= 0);
                }

                /* Copy the range. */
                tmp = dcalloc(1, size);
                rdsize = pread(rfd, tmp, (size_t)size, (wt_off_t)offset);
                testutil_assert(rdsize >= 0);
                testutil_assert(pwrite(wfd, tmp, (size_t)rdsize, (wt_off_t)offset) == rdsize);
                free(tmp);

                nranges++;
            } else {
                /* We are supposed to do the full file copy. */
                testutil_assert(first_range);
                first_range = false;
                testutil_system("cp %s/%s %s/%s", WT_HOME_DIR, filename, backup_home, filename);
            }
        }

        if (rfd > 0)
            testutil_assert(close(rfd) == 0);
        if (wfd > 0)
            testutil_assert(close(wfd) == 0);

        if (first_range) {
            /*
             * If we get here and first_range is still true, it means that there were no changes to
             * the file. The duplicate backup cursor did not return any information. Therefore
             * "copy" the file from the source backup (actually, just create a hard link as an
             * optimization).
             */
            testutil_system("ln %s/%s %s/%s", src_backup_home, filename, backup_home, filename);
            nunmodified++;
        }

        testutil_assert(ret == WT_NOTFOUND);
        testutil_check(file_cursor->close(file_cursor));
    }
    testutil_assert(ret == WT_NOTFOUND);

    /* Cleanup */
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));

    /* Remember that the backup finished successfully. */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/done", backup_home));
    testutil_assert_errno((fp = fopen(buf, "w")) != NULL);
    testutil_assert_errno(fclose(fp) == 0);

    printf("Create incremental backup %" PRIu32 " - complete: files=%" PRId32 ", ranges=%" PRId32
           ", unmodified=%" PRId32 "\n",
      index, nfiles, nranges, nunmodified);

    /* Immediately verify the backup. */
    if (backup_verify_immediately) {
        PRINT_BACKUP_VERIFY(index);
        if (backup_verify_quick) {
            testutil_check(__wt_snprintf(buf, sizeof(buf), "ID%" PRIu32, index));
            testutil_verify_src_backup(conn, backup_home, WT_HOME_DIR, buf);
        } else
            testutil_check(recover_and_verify(index, 0));
        PRINT_BACKUP_VERIFY_DONE(index);
    }
}

/*
 * backup_delete_old_backups --
 *     Delete old backups, keeping just a few recent ones, so that we don't take too much space for
 *     no good reason.
 */
static void
backup_delete_old_backups(int retain, int last_full)
{
    struct dirent *dir;
    struct stat sb;
    DIR *d;
    size_t len;
    int count, i, indexes[256], ndeleted;
    char buf[256];
    bool done;

    len = strlen(BACKUP_BASE);
    ndeleted = 0;
    do {
        done = true;
        testutil_assert_errno((d = opendir(".")) != NULL);
        count = 0;
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, BACKUP_BASE, len) == 0) {
                i = atoi(dir->d_name + len);
                if (i == last_full)
                    continue;
                indexes[count++] = i;

                /* If the backup failed to finish, delete it right away. */
                testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/done", dir->d_name));
                if (stat(buf, &sb) != 0 && errno == ENOENT) {
                    testutil_system("%s %s", RM_COMMAND, dir->d_name);
                    ndeleted++;
                }

                /* If we have too many backups, finish next time. */
                if (count >= (int)(sizeof(indexes) / sizeof(*indexes))) {
                    done = false;
                    break;
                }
            }
        }
        testutil_check(closedir(d));
        if (count <= retain)
            break;

        __wt_qsort(indexes, (size_t)count, sizeof(*indexes), __int_comparator);
        for (i = 0; i < count - retain; i++) {
            testutil_system("%s " BACKUP_BASE "%d", RM_COMMAND, indexes[i]);
            ndeleted++;
        }
    } while (!done);

    printf("Deleted %d old backup%s\n", ndeleted, ndeleted == 1 ? "" : "s");
}

/*
 * thread_ckpt_run --
 *     Runner function for the checkpoint thread.
 */
static WT_THREAD_RET
thread_ckpt_run(void *arg)
{
    FILE *fp;
    THREAD_DATA *td;
    WT_SESSION *session;
    uint64_t stable;
    uint32_t sleep_time;
    int i;
    char ckpt_flush_config[128], ckpt_config[128];
    bool first_ckpt, flush_tier;
    char ts_string[WT_TS_HEX_STRING_SIZE];

    td = (THREAD_DATA *)arg;
    flush_tier = false;
    memset(ckpt_flush_config, 0, sizeof(ckpt_flush_config));
    memset(ckpt_config, 0, sizeof(ckpt_config));

    testutil_check(__wt_snprintf(ckpt_config, sizeof(ckpt_config), "use_timestamp=true"));
    testutil_check(__wt_snprintf(
      ckpt_flush_config, sizeof(ckpt_flush_config), "flush_tier=(enabled,force),%s", ckpt_config));

    set_flush_tier_delay(&td->extra_rnd);

    /*
     * Keep a separate file with the records we wrote for checking.
     */
    (void)unlink(ckpt_file);
    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));
    first_ckpt = true;
    for (i = 1;; ++i) {
        sleep_time = __wt_random(&td->extra_rnd) % MAX_CKPT_INVL;
        testutil_tiered_sleep(opts, session, sleep_time, &flush_tier);
        /*
         * Since this is the default, send in this string even if running without timestamps.
         */
        printf("Checkpoint %d start: Flush: %s.\n", i, flush_tier ? "YES" : "NO");
        testutil_check(session->checkpoint(session, flush_tier ? ckpt_flush_config : ckpt_config));
        testutil_check(td->conn->query_timestamp(td->conn, ts_string, "get=last_checkpoint"));
        testutil_assert(sscanf(ts_string, "%" SCNx64, &stable) == 1);
        printf("Checkpoint %d complete: Flush: %s, at stable %" PRIu64 ".\n", i,
          flush_tier ? "YES" : "NO", stable);

        if (flush_tier) {
            /*
             * FIXME: when we change the API to notify that a flush_tier has completed, we'll need
             * to set up a general event handler and catch that notification, so we can pass the
             * flush_tier "cookie" to the test utility function.
             */
            testutil_tiered_flush_complete(opts, session, NULL);
            flush_tier = false;
            printf("Finished a flush_tier\n");

            set_flush_tier_delay(&td->extra_rnd);
        }

        /*
         * Create the checkpoint file so that the parent process knows at least one checkpoint has
         * finished and can start its timer. If running with timestamps, wait until the stable
         * timestamp has moved past WT_TS_NONE to give writer threads a chance to add something to
         * the database.
         */
        if (first_ckpt && (!use_ts || stable != WT_TS_NONE)) {
            testutil_assert_errno((fp = fopen(ckpt_file, "w")) != NULL);
            first_ckpt = false;
            testutil_assert_errno(fclose(fp) == 0);
        }
    }

    /* NOTREACHED */
}

/*
 * thread_backup_run --
 *     Runner function for the backup thread.
 */
static WT_THREAD_RET
thread_backup_run(void *arg)
{
    struct stat sb;
    THREAD_DATA *td;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    uint32_t i, last_backup, last_full, sleep_time, u;
    int ret;
    char *str;
    char buf[1024];

    td = (THREAD_DATA *)arg;
    last_backup = last_full = 0;

    testutil_check(td->conn->open_session(td->conn, NULL, NULL, &session));

    /*
     * Find the last successful backup.
     */
    if (td->workload_iteration > 1) {
        testutil_check(session->open_cursor(session, "backup:query_id", NULL, NULL, &cursor));
        while ((ret = cursor->next(cursor)) == 0) {
            testutil_check(cursor->get_key(cursor, &str));
            testutil_assert(strncmp(str, "ID", 2) == 0);
            u = (uint32_t)atoi(str + 2);

            /* Check whether the backup has indeed completed. */
            testutil_check(__wt_snprintf(buf, sizeof(buf), BACKUP_BASE "%" PRIu32 "/done", u));
            if (stat(buf, &sb) != 0) {
                testutil_assert_errno(errno == ENOENT);
                printf("Found backup %" PRIu32 ", but it is incomplete\n", u);
                continue;
            }

            printf("Found backup %" PRIu32 "\n", u);
            if (u > last_backup)
                last_backup = u;

            /* Is it a full backup? */
            if (u == 1 ||
              (backup_full_interval > 0 &&
                BACKUP_INDEX_TO_SEQUENCE(u) % backup_full_interval == 0)) {
                if (u > last_full)
                    last_full = u;
            }
        }
        testutil_assert(ret == WT_NOTFOUND);
        testutil_check(cursor->close(cursor));
    }

    /*
     * Create backups until we get killed.
     */
    for (i = 1;; ++i) {
        sleep_time = __wt_random(&td->extra_rnd) % MAX_BACKUP_INVL;
        __wt_sleep(sleep_time, 0);

        /* Create a backup. */
        u = BACKUP_INDEX(td, i);
        if (last_backup == 0 || (backup_full_interval > 0 && i % backup_full_interval == 0)) {
            backup_create_full(td->conn, __wt_random(&td->extra_rnd) % 2, u);
            last_full = u;
        } else
            backup_create_incremental(td->conn, last_backup, u);

        last_backup = u;

        /* Periodically delete old backups. */
        if (i % 5 == 0 || (td->workload_iteration > 1 && i == 1))
            backup_delete_old_backups(5, (int)last_full);
    }

    /* NOTREACHED */
}

/*
 * thread_run --
 *     Runner function for the worker threads.
 */
static WT_THREAD_RET
thread_run(void *arg)
{
    FILE *fp;
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_shadow;
    WT_DECL_RET;
    WT_ITEM data;
    WT_SESSION *prepared_session, *session;
    THREAD_DATA *td;
    uint64_t i, iter, active_ts;
    char cbuf[MAX_VAL], lbuf[MAX_VAL], obuf[MAX_VAL];
    char kname[64], tscfg[64], uri[128];
    bool durable_ahead_commit, use_prep;

    memset(cbuf, 0, sizeof(cbuf));
    memset(lbuf, 0, sizeof(lbuf));
    memset(obuf, 0, sizeof(obuf));
    memset(kname, 0, sizeof(kname));

    prepared_session = NULL;
    td = (THREAD_DATA *)arg;

    /*
     * Set up the separate file for checking.
     */
    testutil_check(__wt_snprintf(cbuf, sizeof(cbuf), RECORDS_FILE, td->threadnum));
    (void)unlink(cbuf);
    testutil_assert_errno((fp = fopen(cbuf, "w")) != NULL);

    /*
     * Set to line buffering. But that is advisory only. We've seen cases where the result files end
     * up with partial lines.
     */
    __wt_stream_set_line_buffer(fp);

    /*
     * Have 10% of the threads use prepared transactions if timestamps are in use. Thread numbers
     * start at 0 so we're always guaranteed that at least one thread is using prepared
     * transactions.
     */
    use_prep = (use_ts && td->threadnum % PREPARE_PCT == 0) ? true : false;
    durable_ahead_commit = false;

    /*
     * For the prepared case we have two sessions so that the oplog session can have its own
     * transaction in parallel with the collection session We need this because prepared
     * transactions cannot have any operations that modify a table that is logged. But we also want
     * to test mixed logged and not-logged transactions.
     */
    testutil_check(td->conn->open_session(td->conn, NULL, "isolation=snapshot", &session));
    if (use_prep)
        testutil_check(
          td->conn->open_session(td->conn, NULL, "isolation=snapshot", &prepared_session));

    /*
     * Open a cursor to each table.
     */
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_collection));
    if (use_prep)
        testutil_check(prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_coll));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_coll));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_shadow));
    if (use_prep)
        testutil_check(
          prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_shadow));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_shadow));

    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_local));
    if (use_prep)
        testutil_check(
          prepared_session->open_cursor(prepared_session, uri, NULL, NULL, &cur_local));
    else
        testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_local));
    testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_oplog));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cur_oplog));

    /*
     * Write our portion of the key space until we're killed.
     */
    printf("Thread %" PRIu32 " starts at %" PRIu64 "\n", td->threadnum, td->start);
    active_ts = 0;
    for (i = td->start, iter = 0;; ++i, ++iter) {
        testutil_check(session->begin_transaction(session, NULL));
        if (use_prep)
            testutil_check(prepared_session->begin_transaction(prepared_session, NULL));

        if (use_ts) {
            /*
             * Set the active timestamp to the first of the three timestamps we reserve for use this
             * iteration. Use the first reserved timestamp.
             */
            active_ts = RESERVED_TIMESTAMPS_FOR_ITERATION(td, iter);
            testutil_check(
              __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));
            /*
             * Set the transaction's timestamp now before performing the operation. If we are using
             * prepared transactions, set the timestamp for the session used for oplog. The
             * collection session in that case would continue to use this timestamp.
             */
            testutil_check(session->timestamp_transaction(session, tscfg));
        }

        if (columns) {
            cur_coll->set_key(cur_coll, i + 1);
            cur_local->set_key(cur_local, i + 1);
            cur_oplog->set_key(cur_oplog, i + 1);
            cur_shadow->set_key(cur_shadow, i + 1);
        } else {
            testutil_check(__wt_snprintf(kname, sizeof(kname), KEY_STRINGFORMAT, i));
            cur_coll->set_key(cur_coll, kname);
            cur_local->set_key(cur_local, kname);
            cur_oplog->set_key(cur_oplog, kname);
            cur_shadow->set_key(cur_shadow, kname);
        }
        /*
         * Put an informative string into the value so that it can be viewed well in a binary dump.
         */
        testutil_check(__wt_snprintf(cbuf, sizeof(cbuf),
          "COLL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->threadnum, active_ts, i));
        testutil_check(__wt_snprintf(lbuf, sizeof(lbuf),
          "LOCAL: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->threadnum, active_ts, i));
        testutil_check(__wt_snprintf(obuf, sizeof(obuf),
          "OPLOG: thread:%" PRIu32 " ts:%" PRIu64 " key: %" PRIu64, td->threadnum, active_ts, i));
        data.size = __wt_random(&td->data_rnd) % MAX_VAL;
        data.data = cbuf;
        cur_coll->set_value(cur_coll, &data);
        if ((ret = cur_coll->insert(cur_coll)) == WT_ROLLBACK)
            goto rollback;
        testutil_check(ret);
        cur_shadow->set_value(cur_shadow, &data);
        if (use_ts) {
            /*
             * Change the timestamp in the middle of the transaction so that we simulate a
             * secondary. This uses our second reserved timestamp.
             */
            ++active_ts;
            testutil_check(
              __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));
            testutil_check(session->timestamp_transaction(session, tscfg));
        }
        if ((ret = cur_shadow->insert(cur_shadow)) == WT_ROLLBACK)
            goto rollback;
        testutil_check(ret);
        data.size = __wt_random(&td->data_rnd) % MAX_VAL;
        data.data = obuf;
        cur_oplog->set_value(cur_oplog, &data);
        if ((ret = cur_oplog->insert(cur_oplog)) == WT_ROLLBACK)
            goto rollback;
        testutil_check(ret);
        if (use_prep) {
            /*
             * Run with prepare every once in a while. And also yield after prepare sometimes too.
             * This is only done on the collection session.
             */
            if (i % PREPARE_FREQ == 0) {
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "prepare_timestamp=%" PRIx64, active_ts));
                testutil_check(prepared_session->prepare_transaction(prepared_session, tscfg));
                if (i % PREPARE_YIELD == 0)
                    __wt_yield();
                /*
                 * Make half of the prepared transactions' durable timestamp larger than their
                 * commit timestamp.
                 */
                durable_ahead_commit = i % PREPARE_DURABLE_AHEAD_COMMIT == 0;
                testutil_check(__wt_snprintf(tscfg, sizeof(tscfg),
                  "commit_timestamp=%" PRIx64 ",durable_timestamp=%" PRIx64, active_ts,
                  durable_ahead_commit ? active_ts + 1 : active_ts));
            } else
                testutil_check(
                  __wt_snprintf(tscfg, sizeof(tscfg), "commit_timestamp=%" PRIx64, active_ts));

            testutil_check(prepared_session->commit_transaction(prepared_session, tscfg));
        }
        testutil_check(session->commit_transaction(session, NULL));
        /*
         * Insert into the local table outside the timestamp txn. This must occur after the
         * timestamp transaction, not before, because of the possibility of rollback in the
         * transaction. The local table must stay in sync with the other tables.
         */
        data.size = __wt_random(&td->data_rnd) % MAX_VAL;
        data.data = lbuf;
        cur_local->set_value(cur_local, &data);
        testutil_check(cur_local->insert(cur_local));

        /*
         * Save the timestamps and key separately for checking later. Optionally use our third
         * reserved timestamp.
         */
        if (fprintf(fp, "%" PRIu64 " %" PRIu64 " %" PRIu64 "\n", active_ts,
              durable_ahead_commit ? active_ts + 1 : active_ts, i) < 0)
            testutil_die(EIO, "fprintf");

        if (0) {
rollback:
            testutil_check(session->rollback_transaction(session, NULL));
            if (use_prep)
                testutil_check(prepared_session->rollback_transaction(prepared_session, NULL));
        }

        /* We're done with the timestamps, allow oldest and stable to move forward. */
        if (use_ts)
            WT_PUBLISH(active_timestamps[td->threadnum], active_ts);
    }
    /* NOTREACHED */
}

/*
 * init_thread_data --
 *     Initialize the thread data struct.
 */
static void
init_thread_data(THREAD_DATA *td, WT_CONNECTION *conn, uint64_t start, uint32_t threadnum,
  uint32_t workload_iteration)
{
    td->conn = conn;
    td->start = start;
    td->threadnum = threadnum;
    td->workload_iteration = workload_iteration;
    testutil_random_from_random(&td->data_rnd, &opts->data_rnd);
    testutil_random_from_random(&td->extra_rnd, &opts->extra_rnd);
}

static void run_workload(uint32_t) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * run_workload --
 *     Child process creates the database and table, and then creates worker threads to add data
 *     until it is killed by the parent.
 */
static void
run_workload(uint32_t workload_iteration)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    THREAD_DATA *td;
    wt_thread_t *thr;
    uint32_t backup_id, cache_mb, ckpt_id, i, ts_id;
    char envconf[512], uri[128];
    const char *table_config, *table_config_nolog;

    thr = dcalloc(nth + 3, sizeof(*thr));
    td = dcalloc(nth + 3, sizeof(THREAD_DATA));
    active_timestamps = dcalloc(nth, sizeof(wt_timestamp_t));

    /*
     * Size the cache appropriately for the number of threads. Each thread adds keys sequentially to
     * its own portion of the key space, so each thread will be dirtying one page at a time. By
     * default, a leaf page grows to 32K in size before it splits and the thread begins to fill
     * another page. We'll budget for 10 full size leaf pages per thread in the cache plus a little
     * extra in the total for overhead.
     */
    cache_mb = ((32 * WT_KILOBYTE * 10) * nth) / WT_MEGABYTE + 20;

    if (opts->inmem)
        testutil_check(__wt_snprintf(
          envconf, sizeof(envconf), ENV_CONFIG_DEF, cache_mb, SESSION_MAX, STAT_WAIT));
    else if (use_lazyfs)
        testutil_check(__wt_snprintf(
          envconf, sizeof(envconf), ENV_CONFIG_TXNSYNC_FSYNC, cache_mb, SESSION_MAX, STAT_WAIT));
    else
        testutil_check(__wt_snprintf(
          envconf, sizeof(envconf), ENV_CONFIG_TXNSYNC, cache_mb, SESSION_MAX, STAT_WAIT));
    if (opts->compat)
        strcat(envconf, TESTUTIL_ENV_CONFIG_COMPAT);
    if (stress)
        strcat(envconf, ENV_CONFIG_ADD_STRESS);

    /*
     * The eviction dirty target and trigger configurations are not compatible with certain other
     * configurations.
     */
    if (!opts->compat && !opts->inmem)
        strcat(envconf, ENV_CONFIG_ADD_EVICT_DIRTY);

    testutil_wiredtiger_open(opts, WT_HOME_DIR, envconf, NULL, &conn, false, false);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Create all the tables on the first iteration.
     */
    if (workload_iteration == 1) {
        if (columns) {
            table_config_nolog = "key_format=r,value_format=u,log=(enabled=false)";
            table_config = "key_format=r,value_format=u";
        } else {
            table_config_nolog = "key_format=S,value_format=u,log=(enabled=false)";
            table_config = "key_format=S,value_format=u";
        }
        testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_collection));
        testutil_check(session->create(session, uri, table_config_nolog));
        testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_shadow));
        testutil_check(session->create(session, uri, table_config_nolog));
        testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_local));
        testutil_check(session->create(session, uri, table_config));
        testutil_check(__wt_snprintf(uri, sizeof(uri), "%s:%s", table_pfx, uri_oplog));
        testutil_check(session->create(session, uri, table_config));
    }

    /*
     * Don't log the stable timestamp table so that we know what timestamp was stored at the
     * checkpoint.
     */
    testutil_check(session->close(session, NULL));

    opts->conn = conn;

    if (opts->tiered_storage) {
        set_flush_tier_delay(&opts->extra_rnd);
        testutil_tiered_begin(opts);
    }

    opts->running = true;

    /* The backup, checkpoint, timestamp, and worker threads are added at the end. */
    backup_id = nth;
    if (use_backups) {
        init_thread_data(&td[backup_id], conn, 0, nth, workload_iteration);
        printf("Create backup thread\n");
        testutil_check(
          __wt_thread_create(NULL, &thr[backup_id], thread_backup_run, &td[backup_id]));
    }

    ckpt_id = nth + 1;
    init_thread_data(&td[ckpt_id], conn, 0, nth, workload_iteration);
    printf("Create checkpoint thread\n");
    testutil_check(__wt_thread_create(NULL, &thr[ckpt_id], thread_ckpt_run, &td[ckpt_id]));

    ts_id = nth + 2;
    if (use_ts) {
        init_thread_data(&td[ts_id], conn, 0, nth, workload_iteration);
        printf("Create timestamp thread\n");
        testutil_check(__wt_thread_create(NULL, &thr[ts_id], thread_ts_run, &td[ts_id]));
    }

    printf("Create %" PRIu32 " writer threads\n", nth);
    for (i = 0; i < nth; ++i) {
        /*
         * We use the following key format:
         *
         *    12004000000123
         *     ^  ^        ^
         *     |  |        |
         *     |  |        +-- key
         *     |  +----------- thread ID
         *     +-------------- iteration ID
         *
         * This setup creates a unique key-space for each thread execution. We can accommodate one
         * billion keys and one thousand threads for each iteration.
         */
        init_thread_data(&td[i], conn,
          (uint64_t)WT_THOUSAND * WT_BILLION * workload_iteration + WT_BILLION * (uint64_t)i, i,
          workload_iteration);
        testutil_check(__wt_thread_create(NULL, &thr[i], thread_run, &td[i]));
    }

    /*
     * The threads never exit, so the child will just wait here until it is killed.
     */
    fflush(stdout);
    for (i = 0; i <= ts_id; ++i)
        testutil_check(__wt_thread_join(NULL, &thr[i]));

    /*
     * NOTREACHED
     */
    free(thr);
    free(td);
    _exit(EXIT_SUCCESS);
}

/*
 * initialize_rep --
 *     Initialize a report structure. Since zero is a valid key we cannot just clear it.
 */
static void
initialize_rep(REPORT *r)
{
    r->first_key = r->first_miss = INVALID_KEY;
    r->absent_key = r->exist_key = r->last_key = INVALID_KEY;
}

/*
 * print_missing --
 *     Print out information if we detect missing records in the middle of the data of a report
 *     structure.
 */
static void
print_missing(REPORT *r, const char *fname, const char *msg)
{
    if (r->exist_key != INVALID_KEY)
        printf("%s: %s error %" PRIu64 " absent records %" PRIu64 "-%" PRIu64 ". Then keys %" PRIu64
               "-%" PRIu64 " exist. Key range %" PRIu64 "-%" PRIu64 "\n",
          fname, msg, (r->exist_key - r->first_miss) - 1, r->first_miss, r->exist_key - 1,
          r->exist_key, r->last_key, r->first_key, r->last_key);
}

/*
 * backup_exists --
 *     Check whether the backup with the given ID exists in the database.
 */
static bool
backup_exists(WT_CONNECTION *conn, uint32_t index)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char backup_id[64];
    const char *idstr;
    bool found;

    testutil_check(__wt_snprintf(backup_id, sizeof(backup_id), "ID%" PRIu32, index));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, "backup:query_id", NULL, NULL, &cursor));
    found = false;
    while (cursor->next(cursor) == 0) {
        testutil_check(cursor->get_key(cursor, &idstr));
        if (strcmp(idstr, backup_id) == 0) {
            found = true;
            break;
        }
    }
    testutil_check(cursor->close(cursor));

    testutil_check(session->close(session, NULL));
    return (found);
}

/*
 * backup_verify --
 *     Verify previous backups created within the given workload iteration (use 0 to verify all).
 */
static void
backup_verify(WT_CONNECTION *conn, uint32_t workload_iteration)
{
    struct dirent *dir;
    struct stat sb;
    DIR *d;
    size_t len;
    uint32_t index;
    char backup_id[64], buf[1024];

    testutil_assert_errno((d = opendir(".")) != NULL);
    len = strlen(BACKUP_BASE);
    while ((dir = readdir(d)) != NULL) {
        if (strncmp(dir->d_name, BACKUP_BASE, len) == 0) {

            /* Verify the backup only if it has completed. */
            testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/done", dir->d_name));
            if (stat(buf, &sb) != 0) {
                testutil_assert_errno(errno == ENOENT);
                continue;
            }

            index = (uint32_t)atoi(dir->d_name + len);
            if (workload_iteration > 0 && BACKUP_INDEX_TO_ITERATION(index) != workload_iteration)
                continue;

            if (backup_verify_quick) {
                /* Just check that chunks that are supposed to be different are indeed different. */
                printf("Verify backup %" PRIu32 " (quick)\n", index);

                /* Continue the verification only if we have the backup ID. */
                if (backup_exists(conn, index)) {
                    PRINT_BACKUP_VERIFY(index);
                    testutil_check(
                      __wt_snprintf(backup_id, sizeof(backup_id), "ID%" PRIu32, index));
                    testutil_verify_src_backup(conn, dir->d_name, WT_HOME_DIR, backup_id);
                    PRINT_BACKUP_VERIFY_DONE(index);
                }
            } else {
                /* Perform a full test. */
                PRINT_BACKUP_VERIFY(index);
                testutil_check(recover_and_verify(index, workload_iteration));
                PRINT_BACKUP_VERIFY_DONE(index);
            }
        }
    }
    testutil_check(closedir(d));

    /* Delete any check directories that we might have created for backup verification. */
    testutil_system("%s " CHECK_BASE "*", RM_COMMAND);
}

/*
 * recover_and_verify --
 *     Run the recovery and verify the database or the given backup (use 0 for the main database).
 *     The workload_iteration argument limits which backups to verify when the backup index is 0
 *     (use 0 to verify all backups irrespective of the iteration in which they were created).
 */
static int
recover_and_verify(uint32_t backup_index, uint32_t workload_iteration)
{
    FILE *fp;
    REPORT c_rep[MAX_TH], l_rep[MAX_TH], o_rep[MAX_TH];
    WT_CONNECTION *conn;
    WT_CURSOR *cur_coll, *cur_local, *cur_oplog, *cur_shadow;
    WT_SESSION *session;
    uint64_t absent_coll, absent_local, absent_oplog, absent_shadow, count, key, last_key;
    uint64_t commit_fp, durable_fp, stable_val;
    uint32_t i;
    int ret;
    char buf[PATH_MAX], fname[64], kname[64];
    char ts_string[WT_TS_HEX_STRING_SIZE];
    bool fatal;

    if (backup_index == 0)
        printf("Open database and run recovery\n");
    else
        printf("Verify backup %" PRIu32 "\n", backup_index);

    /*
     * Open the connection which forces recovery to be run.
     */
    if (backup_index == 0) {
        testutil_wiredtiger_open(opts, WT_HOME_DIR, NULL, &my_event, &conn, true, false);
        printf("Connection open and recovery complete. Verify content\n");
        /* Compare against the copy of the home directory just before recovery. */
        if (use_backups) {
            printf("--- Verify saved dir against the backup source\n");
            testutil_check(__wt_snprintf(buf, sizeof(buf), "%s.SAVE/%s", home, WT_HOME_DIR));
            testutil_verify_src_backup(conn, buf, WT_HOME_DIR, NULL);
            printf("--- DONE: Verify saved dir against the backup source\n");
        }
        /*
         * Only call this when index is 0 because it calls back into here to verify a specific
         * backup.
         */
        if (use_backups)
            backup_verify(conn, workload_iteration);
    } else {
        testutil_check(__wt_snprintf(buf, sizeof(buf), CHECK_BASE "%" PRIu32, backup_index));
        testutil_system(
          "rm -rf " CHECK_BASE "* ; cp -rf " BACKUP_BASE "%" PRIu32 " %s", backup_index, buf);
        /*
         * Open the database connection to the backup. But don't pass our event handlers, so that we
         * don't create another statistics thread. Not only we don't need it here, but trying to
         * create it would cause the test to abort as we currently allow only one statistics thread
         * at a time.
         */
        testutil_wiredtiger_open(opts, buf, NULL, NULL, &conn, true, false);
    }

    /* Sleep to guarantee the statistics thread has enough time to run. */
    usleep(USEC_STAT + 10);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Open a cursor on all the tables.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_collection));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_coll));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_shadow));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_shadow));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_local));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_local));
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s:%s", table_pfx, uri_oplog));
    testutil_check(session->open_cursor(session, buf, NULL, NULL, &cur_oplog));

    /*
     * Find the biggest stable timestamp value that was saved.
     */
    stable_val = 0;
    if (use_ts) {
        testutil_check(conn->query_timestamp(conn, ts_string, "get=recovery"));
        testutil_assert(sscanf(ts_string, "%" SCNx64, &stable_val) == 1);
        printf("Got stable_val %" PRIu64 "\n", stable_val);
    }

    count = 0;
    absent_coll = absent_local = absent_oplog = absent_shadow = 0;
    fatal = false;
    for (i = 0; i < nth; ++i) {
        initialize_rep(&c_rep[i]);
        initialize_rep(&l_rep[i]);
        initialize_rep(&o_rep[i]);
        testutil_check(__wt_snprintf(fname, sizeof(fname), RECORDS_FILE, i));
        if ((fp = fopen(fname, "r")) == NULL)
            testutil_die(errno, "fopen: %s", fname);

        /*
         * For every key in the saved file, verify that the key exists in the table after recovery.
         * If we're doing in-memory log buffering we never expect a record missing in the middle,
         * but records may be missing at the end. If we did write-no-sync, we expect every key to
         * have been recovered.
         */
        for (last_key = INVALID_KEY;; ++count, last_key = key) {
            ret = fscanf(fp, "%" SCNu64 "%" SCNu64 "%" SCNu64 "\n", &commit_fp, &durable_fp, &key);
            if (last_key == INVALID_KEY) {
                c_rep[i].first_key = key;
                l_rep[i].first_key = key;
                o_rep[i].first_key = key;
            }
            if (ret != EOF && ret != 3) {
                /*
                 * If we find a partial line, consider it like an EOF.
                 */
                if (ret == 2 || ret == 1 || ret == 0)
                    break;
                testutil_die(errno, "fscanf");
            }
            if (ret == EOF)
                break;
            /*
             * If we're unlucky, the last line may be a partially written key at the end that can
             * result in a false negative error for a missing record. Detect it.
             */
            if (last_key != INVALID_KEY && key != last_key + 1) {
                printf("%s: Ignore partial record %" PRIu64 " last valid key %" PRIu64 "\n", fname,
                  key, last_key);
                break;
            }

            /*
             * When we are verifying a backup, don't expect anything over the stable timestamp.
             */
            if (backup_index > 0 && durable_fp > stable_val)
                continue;

            if (columns) {
                cur_coll->set_key(cur_coll, key + 1);
                cur_local->set_key(cur_local, key + 1);
                cur_oplog->set_key(cur_oplog, key + 1);
                cur_shadow->set_key(cur_shadow, key + 1);
            } else {
                testutil_check(__wt_snprintf(kname, sizeof(kname), KEY_STRINGFORMAT, key));
                cur_coll->set_key(cur_coll, kname);
                cur_local->set_key(cur_local, kname);
                cur_oplog->set_key(cur_oplog, kname);
                cur_shadow->set_key(cur_shadow, kname);
            }

            /*
             * The collection table should always only have the data as of the checkpoint. The
             * shadow table should always have the exact same data (or not) as the collection table,
             * except for the last key that may be committed after the stable timestamp.
             */
            if ((ret = cur_coll->search(cur_coll)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if ((ret = cur_shadow->search(cur_shadow)) == 0)
                    testutil_die(ret, "shadow search success");

                /*
                 * If we don't find a record, the durable timestamp written to our file better be
                 * larger than the saved one.
                 */
                if (!opts->inmem && durable_fp != 0 && durable_fp <= stable_val) {
                    printf("%s: COLLECTION no record with key %" PRIu64
                           " record durable ts %" PRIu64 " <= stable ts %" PRIu64 "\n",
                      fname, key, durable_fp, stable_val);
                    absent_coll++;
                }
                if (c_rep[i].first_miss == INVALID_KEY)
                    c_rep[i].first_miss = key;
                c_rep[i].absent_key = key;
            } else if ((ret = cur_shadow->search(cur_shadow)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "shadow search");
                /*
                 * We respectively insert the record to the collection table at timestamp t and to
                 * the shadow table at t + 1. If the checkpoint finishes at timestamp t, the last
                 * shadow table record will be removed by rollback to stable after restart.
                 */
                if (durable_fp <= stable_val) {
                    printf("%s: SHADOW no record with key %" PRIu64 "\n", fname, key);
                    absent_shadow++;
                }
            } else if (c_rep[i].absent_key != INVALID_KEY && c_rep[i].exist_key == INVALID_KEY) {
                /*
                 * If we get here we found a record that exists after absent records, a hole in our
                 * data.
                 */
                c_rep[i].exist_key = key;
                fatal = true;
            } else if (!opts->inmem && commit_fp != 0 && commit_fp > stable_val) {
                /*
                 * If we found a record, the commit timestamp written to our file better be no
                 * larger than the checkpoint one.
                 */
                printf("%s: COLLECTION record with key %" PRIu64 " commit record ts %" PRIu64
                       " > stable ts %" PRIu64 "\n",
                  fname, key, commit_fp, stable_val);
                fatal = true;
            } else if ((ret = cur_shadow->search(cur_shadow)) != 0)
                /* Collection and shadow both have the data. */
                testutil_die(ret, "shadow search failure");

            /*
             * The local table should always have all data.
             */
            if ((ret = cur_local->search(cur_local)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if (!opts->inmem)
                    printf("%s: LOCAL no record with key %" PRIu64 "\n", fname, key);
                absent_local++;
                if (l_rep[i].first_miss == INVALID_KEY)
                    l_rep[i].first_miss = key;
                l_rep[i].absent_key = key;
            } else if (l_rep[i].absent_key != INVALID_KEY && l_rep[i].exist_key == INVALID_KEY) {
                /*
                 * We should never find an existing key after we have detected one missing.
                 */
                l_rep[i].exist_key = key;
                fatal = true;
            }
            /*
             * The oplog table should always have all data.
             */
            if ((ret = cur_oplog->search(cur_oplog)) != 0) {
                if (ret != WT_NOTFOUND)
                    testutil_die(ret, "search");
                if (!opts->inmem)
                    printf("%s: OPLOG no record with key %" PRIu64 "\n", fname, key);
                absent_oplog++;
                if (o_rep[i].first_miss == INVALID_KEY)
                    o_rep[i].first_miss = key;
                o_rep[i].absent_key = key;
            } else if (o_rep[i].absent_key != INVALID_KEY && o_rep[i].exist_key == INVALID_KEY) {
                /*
                 * We should never find an existing key after we have detected one missing.
                 */
                o_rep[i].exist_key = key;
                fatal = true;
            }
        }
        c_rep[i].last_key = last_key;
        l_rep[i].last_key = last_key;
        o_rep[i].last_key = last_key;
        testutil_assert_errno(fclose(fp) == 0);
        print_missing(&c_rep[i], fname, "COLLECTION");
        print_missing(&l_rep[i], fname, "LOCAL");
        print_missing(&o_rep[i], fname, "OPLOG");
    }
    testutil_check(conn->close(conn, NULL));
    if (!opts->inmem && absent_coll) {
        printf("COLLECTION: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_coll, count);
        fatal = true;
    }
    if (!opts->inmem && absent_shadow) {
        printf("SHADOW: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_shadow, count);
        fatal = true;
    }
    if (!opts->inmem && absent_local) {
        printf("LOCAL: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_local, count);
        fatal = true;
    }
    if (!opts->inmem && absent_oplog) {
        printf("OPLOG: %" PRIu64 " record(s) absent from %" PRIu64 "\n", absent_oplog, count);
        fatal = true;
    }

    if (fatal) {
        ret = EXIT_FAILURE;
    } else {
        ret = EXIT_SUCCESS;
        printf("%" PRIu64 " records verified\n", count);
    }

    return (ret);
}

/*
 * handler --
 *     Signal handler to catch if the child died unexpectedly.
 */
static void
handler(int sig)
{
    pid_t pid;

    WT_UNUSED(sig);
    pid = wait(NULL);
    /*
     * The core file will indicate why the child exited. Choose EINVAL here.
     */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * main --
 *     The entry point for the test.
 */
int
main(int argc, char *argv[])
{
    struct sigaction sa;
    struct stat sb;
    WT_LAZY_FS lazyfs;
    pid_t pid;
    uint32_t iteration, num_iterations, rand_value, timeout;
    int ch, status, ret;
    char buf[PATH_MAX], bucket[512];
    char cwd_start[PATH_MAX]; /* The working directory when we started */
    bool rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    /* Automatically flush after each newline, so that we don't miss any messages if we crash. */
    __wt_stream_set_line_buffer(stderr);
    __wt_stream_set_line_buffer(stdout);

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));

    backup_full_interval = 4;
    backup_granularity_kb = 1024;
    backup_verify_immediately = false;
    backup_verify_quick = false;
    columns = stress = false;
    nth = MIN_TH;
    num_iterations = 1;
    rand_th = rand_time = true;
    ret = 0;
    timeout = MIN_TIME;
    use_backups = false;
    use_lazyfs = lazyfs_is_implicitly_enabled();
    use_ts = true;
    verify_only = false;

    testutil_parse_begin_opt(argc, argv, SHARED_PARSE_OPTIONS, opts);

    while ((ch = __wt_getopt(progname, argc, argv, "BcF:I:LlsT:t:vz" SHARED_PARSE_OPTIONS)) != EOF)
        switch (ch) {
        case 'B':
            use_backups = true;
            break;
        case 'c':
            /* Variable-length columns only (for now) */
            columns = true;
            break;
        case 'F':
            backup_full_interval = (uint32_t)atoi(__wt_optarg);
            break;
        case 'I':
            num_iterations = (uint32_t)atoi(__wt_optarg);
            if (num_iterations == 0)
                num_iterations = 1;
            break;
        case 'L':
            table_pfx = "lsm";
            break;
        case 'l':
            use_lazyfs = true;
            break;
        case 's':
            stress = true;
            break;
        case 'T':
            rand_th = false;
            nth = (uint32_t)atoi(__wt_optarg);
            if (nth > MAX_TH) {
                fprintf(
                  stderr, "Number of threads is larger than the maximum %" PRId32 "\n", MAX_TH);
                return (EXIT_FAILURE);
            }
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
            break;
        case 'z':
            use_ts = false;
            break;
        default:
            /* The option is either one that we're asking testutil to support, or illegal. */
            if (testutil_parse_single_opt(opts, ch) != 0)
                usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    /*
     * Among other things, this initializes the random number generators in the option structure.
     */
    testutil_parse_end_opt(opts);

    testutil_work_dir_from_path(home, sizeof(home), opts->home);

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

    /* Set up the test. */
    if (!verify_only) {
        /* Create the test's home directory. */
        testutil_make_work_dir(home);

        /* Set up the test subdirectories. */
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", home, RECORDS_DIR));
        testutil_make_work_dir(buf);
        testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/%s", home, WT_HOME_DIR));
        testutil_make_work_dir(buf);

        /* Set up LazyFS. */
        if (use_lazyfs)
            testutil_lazyfs_setup(&lazyfs, home);

        if (opts->tiered_storage) {
            testutil_check(
              __wt_snprintf(bucket, sizeof(bucket), "%s/%s/bucket", home, WT_HOME_DIR));
            testutil_make_work_dir(bucket);
        }

        if (rand_time) {
            timeout = __wt_random(&opts->extra_rnd) % MAX_TIME;
            if (timeout < MIN_TIME)
                timeout = MIN_TIME;
        }

        /*
         * We unconditionally grab a random value to be used for the thread count to keep the RNG in
         * sync for all runs. If we are run first without having a thread count or random seed
         * argument, then when we rerun (with the thread count and random seed that was output),
         * we'll have the same results.
         *
         * We use the data random generator because the number of threads affects the data for this
         * test.
         */
        rand_value = __wt_random(&opts->data_rnd);
        if (rand_th) {
            nth = rand_value % MAX_TH;
            if (nth < MIN_TH)
                nth = MIN_TH;
        }

        printf(
          "Parent: compatibility: %s, in-mem log sync: %s, add timing stress: %s, timestamp in "
          "use: %s\n",
          opts->compat ? "true" : "false", opts->inmem ? "true" : "false",
          stress ? "true" : "false", use_ts ? "true" : "false");
        printf("Parent: Create %" PRIu32 " threads; sleep %" PRIu32 " seconds\n", nth, timeout);
        printf("CONFIG: %s%s%s%s%s%s%s%s%s -F %" PRIu32 " -h %s -I %" PRIu32 " -T %" PRIu32
               " -t %" PRIu32 " " TESTUTIL_SEED_FORMAT "\n",
          progname, use_backups ? " -B" : "", opts->compat ? " -C" : "", columns ? " -c" : "",
          use_lazyfs ? " -l" : "", opts->inmem ? " -m" : "", opts->tiered_storage ? " -PT" : "",
          stress ? " -s" : "", !use_ts ? " -z" : "", backup_full_interval, opts->home,
          num_iterations, nth, timeout, opts->data_seed, opts->extra_seed);

        /*
         * Go inside the home directory (typically WT_TEST), but not all the way into the database's
         * home directory.
         */
        if (chdir(home) != 0)
            testutil_die(errno, "parent chdir: %s", home);

        /*
         * Create the database, run the test, and fail. Do multiple iterations to make sure that we
         * don't only recover, but that we can also keep going, as sometimes bugs can occur during
         * database operation following an unclean shutdown.
         */
        for (iteration = 1; iteration <= num_iterations; iteration++) {

            if (num_iterations > 1)
                printf("\n=== Iteration %" PRIu32 "/%" PRIu32 "\n", iteration, num_iterations);

            /*
             * Fork a child to insert as many items. We will then randomly kill the child, run
             * recovery and make sure all items we wrote exist after recovery runs.
             */
            testutil_assert_errno((pid = fork()) >= 0);
            if (pid == 0) { /* child */
                run_workload(iteration);
                /* NOTREACHED */
            }

            /* parent */

            /*
             * Set the child death handler, but only for the parent process. Setting this before the
             * fork has the unfortunate consequence of the handler getting called on any invocation
             * of system(). But because we set this up after fork, we need to double-check that the
             * child process is still running, i.e., that it did not fail already.
             */
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = handler;
            testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

            /* Check on the child; positive return value indicates that it has already died. */
            testutil_assertfmt(waitpid(pid, &status, WNOHANG) == 0,
              "Child process %" PRIu64 " already exited with status %d", pid, status);

            /*
             * Sleep for the configured amount of time before killing the child. Start the timeout
             * from the time we notice that the file has been created. That allows the test to run
             * correctly on really slow machines.
             */
            while (stat(ckpt_file, &sb) != 0)
                testutil_sleep_wait(1, pid);
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

            /* We don't need the file that checks whether the checkpoint was created. */
            testutil_assert_errno(unlink(ckpt_file) == 0);

            /*
             * !!! If we wanted to take a copy of the directory before recovery,
             * this is the place to do it. Don't do it all the time because
             * it can use a lot of disk space, which can cause test machine
             * issues.
             */

            /* Copy the data to a separate folder for debugging purpose. */
            testutil_copy_data_opt(home, BACKUP_BASE);

            /*
             * Clear the cache, if we are using LazyFS. Do this after we save the data for debugging
             * purposes, so that we can see what we might have lost. If we are using LazyFS, the
             * underlying directory shows the state that we'd get after we clear the cache.
             */
            if (use_lazyfs)
                testutil_lazyfs_clear_cache(&lazyfs);

            /*
             * Clean up any previous backup file. The file would be present if we happen to crash
             * during a backup, in which case, when we recover in the next step, WiredTiger would
             * think that we are recovering from the backup instead of from the main database
             * location. It would ignore the turtle file, and as a side effect, we would lose the
             * information about incremental snapshots.
             */
            if (use_backups) {
                ret = unlink(WT_HOME_DIR DIR_DELIM_STR "WiredTiger.backup");
                testutil_assert_errno(ret == 0 || errno == ENOENT);
                if (ret == 0)
                    printf("Deleted " WT_HOME_DIR DIR_DELIM_STR "WiredTiger.backup\n");
            }

            /*
             * Recover and verify the database, and test all backups.
             */
            ret = recover_and_verify(0, iteration);
            if (ret != EXIT_SUCCESS)
                break;
        }
    } else {
        /* If we are just verifying, first recover the database and then verify. */

        /* Go inside the home directory (typically WT_TEST). */
        if (chdir(home) != 0)
            testutil_die(errno, "parent chdir: %s", home);

        /* Copy the data to a separate folder for debugging purpose. */
        testutil_copy_data_opt(home, BACKUP_BASE);

        /* Now do the actual recovery and verification. */
        ret = recover_and_verify(0, 0);
    }

    /*
     * Clean up.
     */
    /* Clean up the test directory. */
    if (ret == EXIT_SUCCESS && !opts->preserve)
        testutil_clean_test_artifacts(home);

    /* At this point, we are inside `home`, which we intend to delete. cd to the parent dir. */
    if (chdir(cwd_start) != 0)
        testutil_die(errno, "root chdir: %s", home);

    /* Clean up LazyFS. */
    if (!verify_only && use_lazyfs)
        testutil_lazyfs_cleanup(&lazyfs);

    /* Delete the work directory. */
    if (ret == EXIT_SUCCESS && !opts->preserve)
        testutil_clean_work_dir(home);

    testutil_cleanup(opts);
    return (ret);
}
