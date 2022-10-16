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

#define HOME_SIZE 512
#define HOME_BASE "WT_TEST"
static char home[HOME_SIZE];    /* Base home directory */
static char hometmp[HOME_SIZE]; /* Each conn home directory */
static const char *const uri = "table:main";

#define WTOPEN_CFG_COMMON                             \
    "create,log=(enabled,file_max=10M,remove=false)," \
    "statistics=(all),statistics_log=(json,on_close,wait=5),"
#define WT_CONFIG0    \
    WTOPEN_CFG_COMMON \
    "transaction_sync=(enabled=false)"
#define WT_CONFIG1    \
    WTOPEN_CFG_COMMON \
    "transaction_sync=(enabled,method=none)"
#define WT_CONFIG2    \
    WTOPEN_CFG_COMMON \
    "transaction_sync=(enabled,method=fsync)"

#define MAX_DBS 10
#define MAX_IDLE_TIME 30
#define IDLE_INCR 5

#define MAX_KV 100
#define MAX_VAL 128

/*
 * Maximum expected condition variable wakeups. POSIX allows arbitrarily many spurious wakeups to
 * happen, so we need to be able to adjust this expectation per-platform. There are two cases: when
 * completely idle, and when running a light workload. The latter is expressed as a fraction of the
 * total number of condition variable sleeps; the former is a constant.
 */
#if defined(__NetBSD__) || defined(_WIN32)
/*
 * NetBSD should never generate spurious wakeups, but does: see https://gnats.netbsd.org/56275.
 * Windows can also generate spurious wakeups:
 * https://docs.microsoft.com/en-us/windows/win32/sync/condition-variables These values allow the
 * test to complete in spite of that.
 */
#define CV_RESET_THRESHOLD_IDLE 20
#define CV_RESET_THRESHOLD_DENOM 10
#else
/* Default values: should be no wakeups when idle and allow 1/20 otherwise. */
#define CV_RESET_THRESHOLD_IDLE 0
#define CV_RESET_THRESHOLD_DENOM 20
#endif

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-I] [-D maxdbs] [-h dir]\n", progname);
    exit(EXIT_FAILURE);
}

extern int __wt_optind;
extern char *__wt_optarg;

static WT_CONNECTION **connections = NULL;
static WT_CURSOR **cursors = NULL;
static WT_RAND_STATE rnd;
static WT_SESSION **sessions = NULL;

/*
 * get_stat --
 *     TODO: Add a comment describing this function.
 */
static int
get_stat(WT_SESSION *stat_session, int stat_field, uint64_t *valuep)
{
    WT_CURSOR *statc;
    int ret;
    const char *desc, *pvalue;

    testutil_check(stat_session->open_cursor(stat_session, "statistics:", NULL, NULL, &statc));
    statc->set_key(statc, stat_field);
    if ((ret = statc->search(statc)) != 0)
        return (ret);

    ret = statc->get_value(statc, &desc, &pvalue, valuep);
    testutil_check(statc->close(statc));
    return (ret);
}

/*
 * run_ops --
 *     TODO: Add a comment describing this function.
 */
static void
run_ops(int dbs)
{
    WT_ITEM data;
    uint32_t db;
    uint8_t buf[MAX_VAL];
    int db_set, i, key;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < MAX_VAL; ++i)
        buf[i] = (uint8_t)__wt_random(&rnd);
    data.data = buf;
    /*
     * Write a small amount of data into a random subset of the databases.
     */
    db_set = dbs / 4;
    for (i = 0; i < db_set; ++i) {
        db = __wt_random(&rnd) % (uint32_t)dbs;
        printf("Write to database %" PRIu32 "\n", db);
        for (key = 0; key < MAX_KV; ++key) {
            data.size = __wt_random(&rnd) % MAX_VAL;
            cursors[db]->set_key(cursors[db], key);
            cursors[db]->set_value(cursors[db], &data);
            testutil_check(cursors[db]->insert(cursors[db]));
        }
    }
}

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    uint64_t cond_reset, cond_wait;
    uint64_t *cond_reset_orig;
    int cfg, ch, dbs, i;
    char cmd[128];
    const char *working_dir, *wt_cfg;
    bool idle;

    (void)testutil_set_progname(argv);

    dbs = MAX_DBS;
    working_dir = HOME_BASE;
    idle = false;
    while ((ch = __wt_getopt(progname, argc, argv, "D:h:I")) != EOF)
        switch (ch) {
        case 'D':
            dbs = atoi(__wt_optarg);
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        case 'I':
            idle = true;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    /*
     * Allocate arrays for connection handles, sessions, statistics cursors and, if needed, data
     * cursors.
     */
    connections = dcalloc((size_t)dbs, sizeof(WT_CONNECTION *));
    sessions = dcalloc((size_t)dbs, sizeof(WT_SESSION *));
    cond_reset_orig = dcalloc((size_t)dbs, sizeof(uint64_t));
    cursors = idle ? NULL : dcalloc((size_t)dbs, sizeof(WT_CURSOR *));
    memset(cmd, 0, sizeof(cmd));
    /*
     * Set up all the directory names.
     */
    testutil_work_dir_from_path(home, HOME_SIZE, working_dir);
    testutil_make_work_dir(home);
    __wt_random_init(&rnd);
    for (i = 0; i < dbs; ++i) {
        testutil_check(
          __wt_snprintf(hometmp, HOME_SIZE, "%s%c%s.%d", home, DIR_DELIM, HOME_BASE, i));
        testutil_make_work_dir(hometmp);
        /*
         * Open each database. Rotate different configurations among them. Open a session and
         * statistics cursor. If writing data, create the table and open a data cursor.
         */
        cfg = i % 3;
        if (cfg == 0)
            wt_cfg = WT_CONFIG0;
        else if (cfg == 1)
            wt_cfg = WT_CONFIG1;
        else
            wt_cfg = WT_CONFIG2;
        testutil_check(wiredtiger_open(hometmp, NULL, wt_cfg, &connections[i]));
        testutil_check(connections[i]->open_session(connections[i], NULL, NULL, &sessions[i]));
        if (!idle) {
            testutil_check(sessions[i]->create(sessions[i], uri, "key_format=Q,value_format=u"));
            testutil_check(sessions[i]->open_cursor(sessions[i], uri, NULL, NULL, &cursors[i]));
        }
    }

    sleep(10);

    /*
     * Record original reset setting. There could have been some activity during the creation
     * period.
     */
    for (i = 0; i < dbs; ++i)
        testutil_check(
          get_stat(sessions[i], WT_STAT_CONN_COND_AUTO_WAIT_RESET, &cond_reset_orig[i]));
    for (i = 0; i < MAX_IDLE_TIME; i += IDLE_INCR) {
        if (!idle)
            run_ops(dbs);
        printf("Sleep %d (%d of %d)\n", IDLE_INCR, i, MAX_IDLE_TIME);
        sleep(IDLE_INCR);
    }
    for (i = 0; i < dbs; ++i) {
        testutil_check(get_stat(sessions[i], WT_STAT_CONN_COND_AUTO_WAIT_RESET, &cond_reset));
        testutil_check(get_stat(sessions[i], WT_STAT_CONN_COND_AUTO_WAIT, &cond_wait));
        /*
         * On an idle workload there should be no resets of condition variables during the idle
         * period. Even with a light workload, resets should not be very common. We look for 5%.
         */
        if (idle && cond_reset > cond_reset_orig[i] + CV_RESET_THRESHOLD_IDLE)
            testutil_die(ERANGE, "condition reset on idle connection %d of %" PRIu64 " exceeds %d",
              i, cond_reset, CV_RESET_THRESHOLD_IDLE);
        if (!idle && cond_reset > cond_wait / CV_RESET_THRESHOLD_DENOM)
            testutil_die(ERANGE,
              "connection %d condition reset %" PRIu64 " exceeds %d%% of %" PRIu64, i, cond_reset,
              100 / CV_RESET_THRESHOLD_DENOM, cond_wait);
        testutil_check(connections[i]->close(connections[i], NULL));
    }

    /* Cleanup allocated memory. */
    free(connections);
    free(sessions);
    free(cond_reset_orig);
    free(cursors);

    return (EXIT_SUCCESS);
}
