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
#include <test_util.h>

#define MAX_KEYS 100000

static const char *const conn_config =
  "create,cache_size=100MB,log=(archive=false,enabled=true,file_max=100K)";
static const char *const table_config = "key_format=S,value_format=S";

static const char *const full_out = "backup_full";
static const char *const home_full = "WT_HOME_LOG_FULL";
static const char *const home_incr = "WT_HOME_LOG_INCR";
static const char *const home_incr_copy = "WT_HOME_LOG_INCR_COPY";
static const char *const home_live = "WT_HOME_LOG";
static const char *const incr_out = "backup_incr";
static const char *const uri = "table:logtest";
static const char *const wt_tool_paths[] = {
  "./wt",        /* Test is launched from "<build>" folder. */
  "../wt",       /* Test is launched from "<build>/test" folder. */
  "../../wt",    /* Test is launched from "<build>/test/csuite" folder.*/
  "../../../wt", /* Test is launched from "<build>/test/csuite/<test_name>" folder.*/
};

static const char *wt_tool_path = NULL;
static const char *test_root = "./WT_TEST";
static bool preserve_db = false;

static int iterations_num = 5;
static WT_CONNECTION *conn = NULL;
static WT_SESSION *session = NULL;

extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * dump_table --
 *     Dump the table content in to the file in human-readable format.
 */
static void
dump_table(const char *home, const char *table, const char *out_file)
{
    char buf[1024];

    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s -R -h %s/%s dump %s > %s/%s", wt_tool_path,
      test_root, home, table, test_root, out_file));
    testutil_check(system(buf));
}

/*
 * reset_dir --
 *     Recreate the directory.
 */
static void
reset_dir(const char *dir)
{
    char buf[1024];

    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "rm -rf %s/%s && mkdir -p %s/%s", test_root, dir, test_root, dir));
    testutil_check(system(buf));
}

/*
 * remove_dir --
 *     Remove the directory.
 */
static void
remove_dir(const char *dir)
{
    char buf[1024];

    testutil_check(__wt_snprintf(buf, sizeof(buf), "rm -rf %s/%s", test_root, dir));
    testutil_check(system(buf));
}

/*
 * compare_backups --
 *     Compare the full and the incremental backups.
 */
static int
compare_backups(void)
{
    int ret;
    char buf[1024];

    /*
     * We have to copy incremental backup to keep the original database intact. Otherwise we'll get
     * "Incremental backup after running recovery is not allowed".
     */
    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "cp %s/%s/* %s/%s", test_root, home_incr, test_root, home_incr_copy));
    testutil_check(system(buf));

    /* Dump both backups. */
    dump_table(home_full, uri, full_out);
    dump_table(home_incr_copy, uri, incr_out);

    reset_dir(home_incr_copy);

    /* Compare the files. */
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), "cmp %s/%s %s/%s", test_root, full_out, test_root, incr_out));
    if ((ret = system(buf)) != 0) {
        printf(
          "Tables \"%s\" don't match in \"%s\" and \"%s\"!\n See \"%s\" and \"%s\" for details.\n",
          uri, home_full, home_incr_copy, full_out, incr_out);
        exit(1);
    } else {
        /* If they compare successfully, clean up. */
        testutil_check(__wt_snprintf(
          buf, sizeof(buf), "rm %s/%s %s/%s", test_root, full_out, test_root, incr_out));
        testutil_check(system(buf));
        printf("\t Table \"%s\": OK\n", uri);
    }

    return (ret);
}

/*
 * add_work --
 *     Insert some data into the database.
 */
static void
add_work(int iter)
{
    WT_CURSOR *cursor;
    int i;
    char k[32], v[128];

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Perform some operations with individual auto-commit transactions. */
    for (i = 0; i < MAX_KEYS; i++) {
        testutil_check(__wt_snprintf(k, sizeof(k), "key.%d.%d", iter, i));
        testutil_check(__wt_snprintf(v, sizeof(v), "value.%d.%d", iter, i));
        cursor->set_key(cursor, k);
        cursor->set_value(cursor, v);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
}

/*
 * take_full_backup --
 *     Take full backup of the database.
 */
static void
take_full_backup(const char *home, const char *backup_home)
{
    WT_CURSOR *cursor;
    int ret;
    char buf[1024];
    const char *filename;

    testutil_check(session->open_cursor(session, "backup:", NULL, NULL, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        testutil_check(cursor->get_key(cursor, &filename));
        testutil_check(__wt_snprintf(buf, sizeof(buf), "cp %s/%s/%s %s/%s/%s", test_root, home,
          filename, test_root, backup_home, filename));
        testutil_check(system(buf));
    }

    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
}

/*
 * take_incr_backup --
 *     Take incremental log-based backup of the database.
 */
static void
take_incr_backup(const char *backup_home, bool truncate_logs)
{
    WT_CURSOR *cursor;
    int ret;
    char buf[1024];
    const char *filename;

    testutil_check(session->open_cursor(session, "backup:", NULL, "target=(\"log:\")", &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        testutil_check(cursor->get_key(cursor, &filename));

        testutil_check(__wt_snprintf(buf, sizeof(buf), "cp %s/%s/%s %s/%s/%s", test_root, home_live,
          filename, test_root, backup_home, filename));
        testutil_check(system(buf));
    }
    testutil_assert(ret == WT_NOTFOUND);

    if (truncate_logs) {
        /*
         * With an incremental cursor, we want to truncate on the backup cursor to archive the logs.
         * Only do this if the copy process was entirely successful.
         */
        testutil_check(session->truncate(session, "log:", cursor, NULL, NULL));
    }

    testutil_check(cursor->close(cursor));
}

/*
 * prepare_folders --
 *     Prepare all working folders required for the test.
 */
static void
prepare_folders(void)
{
    reset_dir(home_live);
    reset_dir(home_full);
    reset_dir(home_incr);
    reset_dir(home_incr_copy);
}

/*
 * cleanup --
 *     Test's cleanup.
 */
static void
cleanup(bool remove_test_root)
{
    testutil_check(conn->close(conn, NULL));

    if (remove_test_root) {
        /* Remove the test data root directory with all the contents. */
        remove_dir("");
    } else {
        remove_dir(home_full);
        remove_dir(home_incr);
        remove_dir(home_live);
        remove_dir(home_incr_copy);
    }
}

/*
 * reopen_conn --
 *     Close and reopen connection to the database.
 */
static void
reopen_conn(void)
{
    char full_home[1024];

    if (conn != NULL) {
        printf("Reopening connection\n");
        testutil_check(conn->close(conn, NULL));
        conn = NULL;
        session = NULL;
    }

    testutil_check(__wt_snprintf(full_home, sizeof(full_home), "%s/%s", test_root, home_live));
    testutil_check(wiredtiger_open(full_home, NULL, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
}

/*
 * validate --
 *     Validate the database against incremental backup. To do that we need to take a full backup of
 *     the database. Also we have to make a copy of the incremental backup to avoid "Incremental
 *     backup after running recovery is not allowed" error.
 */
static void
validate(bool after_reconnect)
{
    /*
     * The full backup here is only needed for testing and comparison purposes. A normal incremental
     * backup procedure would not include this.
     */
    printf("Taking full backup\n");
    take_full_backup(home_live, home_full);

    /*
     * Taking the incremental backup also calls truncate to archive the log files, if the copies
     * were successful. See that function for details on that call. The truncation only happens
     * after we reconnected to the database.
     */
    printf("Taking incremental backup\n");
    take_incr_backup(home_incr, after_reconnect);

    /*
     * Dump tables from the full backup and incremental backup databases, and compare the dumps.
     */
    printf("Dumping and comparing data\n");
    testutil_check(compare_backups());
    reset_dir(home_full);
}

/*
 * usage --
 *     Print out the command usage message.
 */
static void
usage(void)
{
    fprintf(stderr, "Usage: %s [-h home] [-i number] [-p] [-w path]\n", progname);
    fprintf(stderr,
      "\t-h <home> optional path to the home directory for the test data. Default is "
      "\"./WT_HOME\".\n");
    fprintf(stderr, "\t-i <number> optional number of iterations to run. Default is 5.\n");
    fprintf(stderr, "\t-p optional preserve the database.\n");
    fprintf(stderr, "\t-w <path> optional path to the wt tool. Default is \"./wt\".\n");

    exit(EXIT_FAILURE);
}

/*
 * find_wt_path --
 *     Find wt tool in the current or parent folders. Returns relative path to the tool or NULL in
 *     the case the tool has not been found.
 */
static const char *
find_wt_path(void)
{
    uint32_t i;
    const char *p;

    p = NULL;

    for (i = 0; i < sizeof(wt_tool_paths) / sizeof(wt_tool_paths[0]); i++) {
        if (access(wt_tool_paths[i], F_OK) == 0) {
            p = wt_tool_paths[i];
            break;
        }
    }

    return (p);
}

/*
 * parse_args --
 *     Parse command line arguments.
 */
static void
parse_args(int argc, char *argv[])
{
    int ch;

    while ((ch = __wt_getopt(progname, argc, argv, "h:i:pw:")) != EOF)
        switch (ch) {
        case 'h':
            test_root = __wt_optarg;
            break;
        case 'i':
            iterations_num = atoi(__wt_optarg);
            if (iterations_num <= 0) {
                printf("Invalid iterations number: %s\n", __wt_optarg);
                usage();
            }
            break;
        case 'p':
            preserve_db = true;
            break;
        case 'w':
            wt_tool_path = __wt_optarg;
            if (access(wt_tool_path, F_OK) != 0) {
                printf("Invalid path to WT tool: %s\n", __wt_optarg);
                usage();
            }
            break;
        default:
            break;
        }

    /* If there's no -w parameter, try to find wt tool in the current and the parent folders. */
    if (wt_tool_path == NULL)
        wt_tool_path = find_wt_path();

    if (wt_tool_path == NULL) {
        /*
         * Give up, the path to wt was not provided in the test arguments list and we failed to find
         * it in the current and the parent folders.
         */
        printf("Can't find WT tool.\n");
        usage();
    }
}

/*
 * main --
 *     Test's entry point.
 */
int
main(int argc, char *argv[])
{
    int i;
    bool root_dir_exist;

    (void)testutil_set_progname(argv);
    parse_args(argc, argv);

    root_dir_exist = (access(test_root, F_OK) == 0);
    prepare_folders();

    reopen_conn();
    testutil_check(session->create(session, uri, table_config));

    printf("Taking initial backup into incremental backup folder\n");
    take_full_backup(home_live, home_incr);

    for (i = 1; i <= iterations_num; i++) {
        printf("==================================\n");
        printf("Iteration %d:\n", i);
        printf("==================================\n");

        printf("Adding data\n");
        add_work(i);
        testutil_check(session->checkpoint(session, NULL));

        /* Validate database against incremental backup. Do not truncate logs. */
        validate(false);

        /* Reopen connection. */
        reopen_conn();

        /* Validate database again. Truncate logs. */
        validate(true);
    }

    if (!preserve_db)
        cleanup(!root_dir_exist);

    return (EXIT_SUCCESS);
}
