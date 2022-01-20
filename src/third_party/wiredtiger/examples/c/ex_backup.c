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
 * ex_backup.c
 * 	demonstrates how to use incremental backup and log files.
 */
#include <test_util.h>

static const char *const home = "WT_HOME_LOG";
static const char *const home_full = "WT_HOME_LOG_FULL";
static const char *const home_incr = "WT_HOME_LOG_INCR";

static const char *const full_out = "./backup_full";
static const char *const incr_out = "./backup_incr";

static const char *const uri = "table:logtest";

#define CONN_CONFIG "create,cache_size=100MB,log=(enabled=true,file_max=100K,remove=false)"
#define MAX_ITERATIONS 5
#define MAX_KEYS 10000

static int
compare_backups(int i)
{
    int ret;
    char buf[1024], msg[32];

    /*
     * We run 'wt dump' on both the full backup directory and the incremental backup directory for
     * this iteration. Since running 'wt' runs recovery and makes both directories "live", we need a
     * new directory for each iteration.
     *
     * If i == 0, we're comparing against the main, original directory with the final incremental
     * directory.
     */
    if (i == 0)
        (void)snprintf(
          buf, sizeof(buf), "../../wt -R -h %s dump logtest > %s.%d", home, full_out, i);
    else
        (void)snprintf(
          buf, sizeof(buf), "../../wt -R -h %s.%d dump logtest > %s.%d", home_full, i, full_out, i);
    error_check(system(buf));
    /*
     * Now run dump on the incremental directory.
     */
    (void)snprintf(
      buf, sizeof(buf), "../../wt -R -h %s.%d dump logtest > %s.%d", home_incr, i, incr_out, i);
    error_check(system(buf));

    /*
     * Compare the files.
     */
    (void)snprintf(buf, sizeof(buf), "cmp %s.%d %s.%d", full_out, i, incr_out, i);
    ret = system(buf);
    if (i == 0)
        (void)snprintf(msg, sizeof(msg), "%s", "MAIN");
    else
        (void)snprintf(msg, sizeof(msg), "%d", i);
    printf("Iteration %s: Tables %s.%d and %s.%d %s\n", msg, full_out, i, incr_out, i,
      ret == 0 ? "identical" : "differ");
    if (ret != 0)
        exit(1);

    /*
     * If they compare successfully, clean up.
     */
    if (i != 0) {
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d %s.%d %s.%d %s.%d", home_full, i, home_incr,
          i, full_out, i, incr_out, i);
        error_check(system(buf));
    }
    return (ret);
}

/*
 * Set up all the directories needed for the test. We have a full backup directory for each
 * iteration and an incremental backup for each iteration. That way we can compare the full and
 * incremental each time through.
 */
static void
setup_directories(void)
{
    int i;
    char buf[1024];

    for (i = 0; i < MAX_ITERATIONS; i++) {
        /*
         * For incremental backups we need 0-N. The 0 incremental directory will compare with the
         * original at the end.
         */
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir %s.%d", home_incr, i, home_incr, i);
        error_check(system(buf));
        if (i == 0)
            continue;
        /*
         * For full backups we need 1-N.
         */
        (void)snprintf(buf, sizeof(buf), "rm -rf %s.%d && mkdir %s.%d", home_full, i, home_full, i);
        error_check(system(buf));
    }
}

static void
add_work(WT_SESSION *session, int iter)
{
    WT_CURSOR *cursor;
    int i;
    char k[32], v[32];

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    /*
     * Perform some operations with individual auto-commit transactions.
     */
    for (i = 0; i < MAX_KEYS; i++) {
        (void)snprintf(k, sizeof(k), "key.%d.%d", iter, i);
        (void)snprintf(v, sizeof(v), "value.%d.%d", iter, i);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        error_check(cursor->insert(cursor));
    }
    error_check(cursor->close(cursor));
}

static void
take_full_backup(WT_SESSION *session, int i)
{
    WT_CURSOR *cursor;
    int j, ret;
    char buf[1024], h[256];
    const char *filename, *hdir;

    /*
     * First time through we take a full backup into the incremental directories. Otherwise only
     * into the appropriate full directory.
     */
    if (i != 0) {
        (void)snprintf(h, sizeof(h), "%s.%d", home_full, i);
        hdir = h;
    } else
        hdir = home_incr;
    error_check(session->open_cursor(session, "backup:", NULL, NULL, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &filename));
        if (i == 0)
            /*
             * Take a full backup into each incremental directory.
             */
            for (j = 0; j < MAX_ITERATIONS; j++) {
                (void)snprintf(h, sizeof(h), "%s.%d", home_incr, j);
                (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
                error_check(system(buf));
            }
        else {
            (void)snprintf(h, sizeof(h), "%s.%d", home_full, i);
            (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, hdir, filename);
            error_check(system(buf));
        }
    }
    scan_end_check(ret == WT_NOTFOUND);
    error_check(cursor->close(cursor));
}

static void
take_incr_backup(WT_SESSION *session, int i)
{
    WT_CURSOR *cursor;
    int j, ret;
    char buf[1024], h[256];
    const char *filename;

    error_check(session->open_cursor(session, "backup:", NULL, "target=(\"log:\")", &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &filename));
        /*
         * Copy into the 0 incremental directory and then each of the incremental directories for
         * this iteration and later.
         */
        (void)snprintf(h, sizeof(h), "%s.0", home_incr);
        (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
        error_check(system(buf));
        for (j = i; j < MAX_ITERATIONS; j++) {
            (void)snprintf(h, sizeof(h), "%s.%d", home_incr, j);
            (void)snprintf(buf, sizeof(buf), "cp %s/%s %s/%s", home, filename, h, filename);
            error_check(system(buf));
        }
    }
    scan_end_check(ret == WT_NOTFOUND);

    /*
     * With an incremental cursor, we want to truncate on the backup cursor to remove the logs. Only
     * do this if the copy process was entirely successful.
     */
    /*! [Truncate a backup cursor] */
    error_check(session->truncate(session, "log:", cursor, NULL, NULL));
    /*! [Truncate a backup cursor] */
    error_check(cursor->close(cursor));
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *wt_conn;
    WT_SESSION *session;
    int i;
    char cmd_buf[256];

    (void)argc; /* Unused variable */
    (void)testutil_set_progname(argv);

    (void)snprintf(cmd_buf, sizeof(cmd_buf), "rm -rf %s && mkdir %s", home, home);
    error_check(system(cmd_buf));
    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &wt_conn));

    setup_directories();
    error_check(wt_conn->open_session(wt_conn, NULL, NULL, &session));
    error_check(session->create(session, uri, "key_format=S,value_format=S"));
    printf("Adding initial data\n");
    add_work(session, 0);

    printf("Taking initial backup\n");
    take_full_backup(session, 0);

    error_check(session->checkpoint(session, NULL));

    for (i = 1; i < MAX_ITERATIONS; i++) {
        printf("Iteration %d: adding data\n", i);
        add_work(session, i);
        error_check(session->checkpoint(session, NULL));
        /*
         * The full backup here is only needed for testing and comparison purposes. A normal
         * incremental backup procedure would not include this.
         */
        printf("Iteration %d: taking full backup\n", i);
        take_full_backup(session, i);
        /*
         * Taking the incremental backup also calls truncate to remove the log files, if the copies
         * were successful. See that function for details on that call.
         */
        printf("Iteration %d: taking incremental backup\n", i);
        take_incr_backup(session, i);

        printf("Iteration %d: dumping and comparing data\n", i);
        error_check(compare_backups(i));
    }

    /*
     * Close the connection. We're done and want to run the final comparison between the incremental
     * and original.
     */
    error_check(wt_conn->close(wt_conn, NULL));

    printf("Final comparison: dumping and comparing data\n");
    error_check(compare_backups(0));

    return (EXIT_SUCCESS);
}
