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

#include <sys/stat.h>
#include <sys/wait.h>

static char home[1024]; /* Program working dir */
static const char *const uri = "table:main";

#define ENV_CONFIG \
    "create,log=(enabled,file_max=100K,remove=false),transaction_sync=(enabled,method=none)"
#define ENV_CONFIG_ADD_REC "log=(recover=on)"

/*
 * A log file starts with a header record followed by a system record, each one allocation unit of
 * 128 bytes. Truncating the first log file to the end of its system record leaves a complete record
 * boundary, but ends the file well before the position the second log file continues from.
 */
#define LOG_SYSTEM_END (2 * 128)

/*
 * The end of the header and system record is also the offset of the first real record: a log file
 * emptied to this size is a valid, empty log file.
 */
#define LOG_FIRST_RECORD (2 * 128)

#define LOG_FILE_1 "WiredTigerLog.0000000001"
#define LOG_FILE_2 "WiredTigerLog.0000000002"

#define K_SIZE 16
#define V_SIZE 256
#define MAX_KEYS 5000

static void fill_db(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * fill_db --
 *     Child process creates the database and table, then writes data until the log rolls over into
 *     a second file. Killing the process without closing the connection leaves recovery to find the
 *     second log file's system record pointing at the end of the first.
 */
static void
fill_db(void)
{
    struct stat sb;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    uint32_t i;
    char k[K_SIZE], v[V_SIZE];

    /*
     * Run in the home directory so the log files are visible to the parent.
     */
    if (chdir(home) != 0)
        testutil_die(errno, "chdir: %s", home);
    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, "key_format=S,value_format=S"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    for (i = 0; i < MAX_KEYS; ++i) {
        testutil_snprintf(k, sizeof(k), "key%03" PRIu32, i);
        testutil_snprintf(
          v, sizeof(v), "value%0*" PRIu32, (int)(V_SIZE - (strlen("value") + 1)), i);
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        testutil_check(cursor->insert(cursor));

        /*
         * The second log file appears when a record no longer fits in the first one. Its system
         * record names the position the first log file had reached at that point.
         */
        if (stat(LOG_FILE_2, &sb) == 0)
            break;
        if (errno != ENOENT)
            testutil_die(errno, "stat: %s", LOG_FILE_2);
    }
    if (i == MAX_KEYS)
        testutil_die(EINVAL, "did not roll over into a second log file");
    _exit(EXIT_SUCCESS);
}

extern int __wt_optind;
extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print usage and exit.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir] [-p]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * main --
 *     Run the log scan boundary test.
 */
int
main(int argc, char *argv[])
{
    struct stat sb;
    WT_CONNECTION *conn;
    pid_t pid;
    int ch, status;
    const char *working_dir;
    bool preserve;

    preserve = false;
    (void)testutil_set_progname(argv);

    working_dir = "WT_TEST.wt18959_log_scan_truncate";
    while ((ch = __wt_getopt(progname, argc, argv, "h:p")) != EOF)
        switch (ch) {
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
    testutil_recreate_dir(home);

    /*
     * Fork a child to write the database, then let it die without shutting the connection down. A
     * clean shutdown checkpoints and removes the older log files, and recovery needs them.
     */
    testutil_assert_errno((pid = fork()) >= 0);
    if (pid == 0) { /* child */
        fill_db();
        /* NOTREACHED */
    }
    testutil_assert_errno(waitpid(pid, &status, 0) != -1);
    testutil_assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (chdir(home) != 0)
        testutil_die(errno, "chdir: %s", home);

    /*
     * Cut the first log file back to the end of its system record. Its records are still valid, but
     * it now ends before the point the second log file continues from, which means recovery must
     * stop at the file boundary.
     */
    printf("Parent: truncate %s to %d bytes\n", LOG_FILE_1, LOG_SYSTEM_END);
    if (truncate(LOG_FILE_1, LOG_SYSTEM_END) != 0)
        testutil_die(errno, "truncate: %s", LOG_FILE_1);

    /*
     * Recovery detects the boundary and ends the log at the last file it actually scanned. The file
     * it stopped at must be left as a valid, truncated log file, not emptied to zero bytes.
     */
    testutil_check(wiredtiger_open(NULL, NULL, ENV_CONFIG_ADD_REC, &conn));
    testutil_assert_errno(stat(LOG_FILE_2, &sb) == 0);
    testutil_check(conn->close(conn, NULL));

    printf("Parent: %s is %" PRId64 " bytes after recovery\n", LOG_FILE_2, (int64_t)sb.st_size);
    testutil_assertfmt(sb.st_size == LOG_FIRST_RECORD,
      "%s is %" PRId64 " bytes after recovery, expected %d", LOG_FILE_2, (int64_t)sb.st_size,
      LOG_FIRST_RECORD);

    if (!preserve) {
        /* At this point $PATH is inside `home`, which we intend to delete. cd to the parent dir. */
        if (chdir("../") != 0)
            testutil_die(errno, "root chdir: %s", home);
        testutil_remove(home);
    }

    return (EXIT_SUCCESS);
}
