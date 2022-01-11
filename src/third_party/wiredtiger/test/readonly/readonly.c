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

#define HOME_SIZE 512
static char home[HOME_SIZE];       /* Program working dir lock file */
#define HOME_WR_SUFFIX ".WRNOLOCK" /* Writable dir copy no lock file */
static char home_wr[HOME_SIZE + sizeof(HOME_WR_SUFFIX)];
#define HOME_RD_SUFFIX ".RD" /* Read-only dir */
static char home_rd[HOME_SIZE + sizeof(HOME_RD_SUFFIX)];
#define HOME_RD2_SUFFIX ".RDNOLOCK" /* Read-only dir no lock file */
static char home_rd2[HOME_SIZE + sizeof(HOME_RD2_SUFFIX)];

static const char *saved_argv0; /* Program command */
static const char *const uri = "table:main";

#define ENV_CONFIG                                     \
    "create,log=(file_max=10M,archive=false,enabled)," \
    "operation_tracking=(enabled=false),transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_RD "operation_tracking=(enabled=false),readonly=true"
#define ENV_CONFIG_WR "operation_tracking=(enabled=false),readonly=false"
#define MAX_VAL 4096
#define MAX_KV 10000

#define EXPECT_ERR 1
#define EXPECT_SUCCESS 0

#define OP_READ 0
#define OP_WRITE 1

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

/*
 * run_child --
 *     TODO: Add a comment describing this function.
 */
static int
run_child(const char *homedir, int op, int expect)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int i, ret;
    const char *cfg;

    /*
     * We expect the read-only database will allow the second read-only handle to succeed because no
     * one can create or set the lock file.
     */
    if (op == OP_READ)
        cfg = ENV_CONFIG_RD;
    else
        cfg = ENV_CONFIG_WR;
    if ((ret = wiredtiger_open(homedir, NULL, cfg, &conn)) == 0) {
        if (expect == EXPECT_ERR)
            testutil_die(ret, "wiredtiger_open expected error, succeeded");
    } else {
        if (expect == EXPECT_SUCCESS)
            testutil_die(ret, "wiredtiger_open expected success, error");
        /*
         * If we expect an error and got one, we're done.
         */
        return (0);
    }

    /*
     * Make sure we can read the data.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    i = 0;
    while ((ret = cursor->next(cursor)) == 0)
        ++i;
    if (i != MAX_KV)
        testutil_die(ret, "cursor walk");
    testutil_check(conn->close(conn, NULL));
    return (0);
}

/*
 * Child process opens both databases readonly.
 */
static void open_dbs(int, const char *, const char *, const char *, const char *)
  WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
/*
 * open_dbs --
 *     TODO: Add a comment describing this function.
 */
static void
open_dbs(int op, const char *dir, const char *dir_wr, const char *dir_rd, const char *dir_rd2)
{
    int expect, ret;

    /*
     * The parent has an open connection to all directories. We expect opening the writeable homes
     * to return an error. It is a failure if the child successfully opens that.
     */
    expect = EXPECT_ERR;
    if ((ret = run_child(dir, op, expect)) != 0)
        testutil_die(ret, "wiredtiger_open readonly allowed");
    if ((ret = run_child(dir_wr, op, expect)) != 0)
        testutil_die(ret, "wiredtiger_open readonly allowed");

    /*
     * The parent must have a read-only connection open to the read-only databases. If the child is
     * opening read-only too, we expect success. Otherwise an error if the child attempts to open
     * read/write (permission error).
     */
    if (op == OP_READ)
        expect = EXPECT_SUCCESS;
    if ((ret = run_child(dir_rd, op, expect)) != 0)
        testutil_die(ret, "run child 1");
    if ((ret = run_child(dir_rd2, op, expect)) != 0)
        testutil_die(ret, "run child 2");
    exit(EXIT_SUCCESS);
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
    WT_CONNECTION *conn, *conn2, *conn3, *conn4;
    WT_CURSOR *cursor;
    WT_ITEM data;
    WT_SESSION *session;
    uint64_t i;
    uint8_t buf[MAX_VAL];
    int ch, op, ret, status;
    char cmd[512];
    const char *working_dir;
    bool child;

    (void)testutil_set_progname(argv);

    /*
     * Needed unaltered for system command later.
     */
    saved_argv0 = argv[0];

    working_dir = "WT_RD";
    child = false;
    op = OP_READ;
    while ((ch = __wt_getopt(progname, argc, argv, "Rh:W")) != EOF)
        switch (ch) {
        case 'R':
            child = true;
            op = OP_READ;
            break;
        case 'W':
            child = true;
            op = OP_WRITE;
            break;
        case 'h':
            working_dir = __wt_optarg;
            break;
        default:
            usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    /*
     * Set up all the directory names.
     */
    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_check(__wt_snprintf(home_wr, sizeof(home_wr), "%s%s", home, HOME_WR_SUFFIX));
    testutil_check(__wt_snprintf(home_rd, sizeof(home_rd), "%s%s", home, HOME_RD_SUFFIX));
    testutil_check(__wt_snprintf(home_rd2, sizeof(home_rd2), "%s%s", home, HOME_RD2_SUFFIX));
    if (!child) {
        testutil_make_work_dir(home);
        testutil_make_work_dir(home_wr);
        testutil_make_work_dir(home_rd);
        testutil_make_work_dir(home_rd2);
    } else
        /*
         * We are a child process, we just want to call the open_dbs with the directories we have.
         * The child function will exit.
         */
        open_dbs(op, home, home_wr, home_rd, home_rd2);

    /*
     * Parent creates a database and table. Then cleanly shuts down. Then copy database to read-only
     * directory and chmod. Also copy database to read-only directory and remove the lock file. One
     * read-only database will have a lock file in the file system and the other will not. Parent
     * opens all databases with read-only configuration flag. Parent forks off child who tries to
     * also open all databases with the read-only flag. It should error on the writeable directory,
     * but allow it on the read-only directories. The child then confirms it can read all the data.
     */
    /*
     * Run in the home directory and create the table.
     */
    testutil_check(wiredtiger_open(home, NULL, ENV_CONFIG, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=Q,value_format=u"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /*
     * Write data into the table and then cleanly shut down connection.
     */
    memset(buf, 0, sizeof(buf));
    data.data = buf;
    data.size = MAX_VAL;
    for (i = 0; i < MAX_KV; ++i) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, &data);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(conn->close(conn, NULL));

    /*
     * Copy the database. Remove any lock file from one copy and chmod the copies to be read-only
     * permissions.
     */
    testutil_check(__wt_snprintf(
      cmd, sizeof(cmd), "cp -rp %s/* %s; rm -f %s/WiredTiger.lock", home, home_wr, home_wr));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);

    testutil_check(__wt_snprintf(cmd, sizeof(cmd),
      "cp -rp %s/* %s; chmod 0555 %s; chmod -R 0444 %s/*", home, home_rd, home_rd, home_rd));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);

    testutil_check(__wt_snprintf(cmd, sizeof(cmd),
      "cp -rp %s/* %s; rm -f %s/WiredTiger.lock; chmod 0555 %s; chmod -R 0444 %s/*", home, home_rd2,
      home_rd2, home_rd2, home_rd2));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);

    /*
     * Run four scenarios.  Sometimes expect errors, sometimes success.
     * The writable database directories should always fail to allow the
     * child to open due to the lock file.  The read-only ones will only
     * succeed when the child attempts read-only.
     *
     * 1.  Parent has read-only handle to all databases.  Child opens
     *     read-only also.
     * 2.  Parent has read-only handle to all databases.  Child opens
     *     read-write.
     * 3.  Parent has read-write handle to writable databases and
     *     read-only to read-only databases.  Child opens read-only.
     * 4.  Parent has read-write handle to writable databases and
     *     read-only to read-only databases.  Child opens read-write.
     */
    /*
     * Open a connection handle to all databases.
     */
    fprintf(stderr, " *** Expect several error messages from WT ***\n");
    /*
     * Scenario 1.
     */
    if ((ret = wiredtiger_open(home, NULL, ENV_CONFIG_RD, &conn)) != 0)
        testutil_die(ret, "wiredtiger_open original home");
    if ((ret = wiredtiger_open(home_wr, NULL, ENV_CONFIG_RD, &conn2)) != 0)
        testutil_die(ret, "wiredtiger_open write nolock");
    if ((ret = wiredtiger_open(home_rd, NULL, ENV_CONFIG_RD, &conn3)) != 0)
        testutil_die(ret, "wiredtiger_open readonly");
    if ((ret = wiredtiger_open(home_rd2, NULL, ENV_CONFIG_RD, &conn4)) != 0)
        testutil_die(ret, "wiredtiger_open readonly nolock");

    /*
     * Create a child to also open a connection handle to the databases. We cannot use fork here
     * because using fork the child inherits the same memory image. Therefore the WT process
     * structure is set in the child even though it should not be. So use 'system' to spawn an
     * entirely new process.
     *
     * The child will exit with success if its test passes.
     */
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "%s -h %s -R", saved_argv0, working_dir));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    if (WEXITSTATUS(status) != 0)
        testutil_die(WEXITSTATUS(status), "system: %s", cmd);

    /*
     * Scenario 2. Run child with writable config.
     */
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "%s -h %s -W", saved_argv0, working_dir));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    if (WEXITSTATUS(status) != 0)
        testutil_die(WEXITSTATUS(status), "system: %s", cmd);

    /*
     * Reopen the two writable directories and rerun the child.
     */
    testutil_check(conn->close(conn, NULL));
    testutil_check(conn2->close(conn2, NULL));
    if ((ret = wiredtiger_open(home, NULL, ENV_CONFIG_RD, &conn)) != 0)
        testutil_die(ret, "wiredtiger_open original home");
    if ((ret = wiredtiger_open(home_wr, NULL, ENV_CONFIG_RD, &conn2)) != 0)
        testutil_die(ret, "wiredtiger_open write nolock");
    /*
     * Scenario 3. Child read-only.
     */
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "%s -h %s -R", saved_argv0, working_dir));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    if (WEXITSTATUS(status) != 0)
        testutil_die(WEXITSTATUS(status), "system: %s", cmd);

    /*
     * Scenario 4. Run child with writable config.
     */
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "%s -h %s -W", saved_argv0, working_dir));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    if (WEXITSTATUS(status) != 0)
        testutil_die(WEXITSTATUS(status), "system: %s", cmd);

    /*
     * Clean-up.
     */
    testutil_check(conn->close(conn, NULL));
    testutil_check(conn2->close(conn2, NULL));
    testutil_check(conn3->close(conn3, NULL));
    testutil_check(conn4->close(conn4, NULL));
    /*
     * We need to chmod the read-only databases back so that they can be removed by scripts.
     */
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "chmod 0777 %s %s", home_rd, home_rd2));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    testutil_check(__wt_snprintf(cmd, sizeof(cmd), "chmod -R 0666 %s/* %s/*", home_rd, home_rd2));
    if ((status = system(cmd)) < 0)
        testutil_die(status, "system: %s", cmd);
    printf(" *** Readonly test successful ***\n");
    return (EXIT_SUCCESS);
}
