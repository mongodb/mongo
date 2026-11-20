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

/*
 * A read-only connection opens the lock file for writing to detect another process using the
 * database, and ignores the failure when it cannot. Read-only file systems fail that open with
 * EPERM or EROFS rather than EACCES, so use the fail file system to inject both errors.
 */

#ifndef WT_FAIL_FS_LIB
#define WT_FAIL_FS_LIB "ext/test/fail_fs/.libs/libwiredtiger_fail_fs.so"
#endif

#define MAX_KV 100

/*
 * check_lock_open_failure --
 *     Check that a read-only connection opens the database and a read-write connection fails when
 *     opening the lock file returns the given error.
 */
static void
check_lock_open_failure(TEST_OPTS *opts, const char *build_dir, int error, const char *error_name)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int count, ret;
    char config[1024], value[128];

    testutil_snprintf(value, sizeof(value), "%d|WiredTiger.lock", error);
    testutil_check(setenv("WT_FAIL_FS_OPEN_ERROR", value, 1));

    /*
     * A read-only connection must ignore the lock file failure and be able to read the data.
     */
    testutil_snprintf(config, sizeof(config), "readonly=true,extensions=(%s/%s=(early_load=true))",
      build_dir, WT_FAIL_FS_LIB);
    if ((ret = wiredtiger_open(opts->home, NULL, config, &conn)) != 0)
        testutil_die(ret, "read-only open, lock file open failed with %s", error_name);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    count = 0;
    while (cursor->next(cursor) == 0)
        ++count;
    testutil_assert(count == MAX_KV);
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /*
     * A read-write connection must still fail: the error can only be ignored when the connection
     * cannot modify the database. This also confirms the lock file open was failed.
     */
    testutil_snprintf(
      config, sizeof(config), "extensions=(%s/%s=(early_load=true))", build_dir, WT_FAIL_FS_LIB);
    ret = wiredtiger_open(opts->home, NULL, config, &conn);
    if (ret != error)
        testutil_die(ret, "read-write open, expected %s on the lock file", error_name);

    testutil_check(unsetenv("WT_FAIL_FS_OPEN_ERROR"));
}

/*
 * main --
 *     Run the read-only lock file open tests.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int i;
    char flib[1024];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_recreate_dir(opts->home);

    /* Populate a database while the file system is working normally. */
    testutil_check(wiredtiger_open(opts->home, NULL, "create", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, opts->uri, "key_format=Q,value_format=S"));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    for (i = 0; i < MAX_KV; ++i) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, "value");
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    testutil_build_dir(opts, flib, sizeof(flib));
    check_lock_open_failure(opts, flib, EPERM, "EPERM");
    check_lock_open_failure(opts, flib, EROFS, "EROFS");

    testutil_cleanup(opts);
    return (0);
}
