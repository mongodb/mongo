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
 * JIRA ticket reference: WT-3120 Test case description: A simple file system extension built into a
 * shared library. Failure mode: Loading the file system and closing the connection is enough to
 * evoke the failure. This test does slightly more than that.
 */

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char *kstr, *vstr, buf[1024], config[1024];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

#ifndef WT_FAIL_FS_LIB
#define WT_FAIL_FS_LIB "ext/test/fail_fs/.libs/libwiredtiger_fail_fs.so"
#endif
    testutil_build_dir(opts, buf, 1024);
    testutil_check(__wt_snprintf(
      config, sizeof(config), "create,extensions=(%s/%s=(early_load=true))", buf, WT_FAIL_FS_LIB));
    testutil_check(wiredtiger_open(opts->home, NULL, config, &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->create(session, opts->uri, "key_format=S,value_format=S"));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    cursor->set_key(cursor, "a");
    cursor->set_value(cursor, "0");
    testutil_check(cursor->insert(cursor));
    cursor->set_key(cursor, "b");
    cursor->set_value(cursor, "1");
    testutil_check(cursor->insert(cursor));
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));

    /* Force to disk and re-open. */
    testutil_check(opts->conn->close(opts->conn, NULL));
    testutil_check(wiredtiger_open(opts->home, NULL, NULL, &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));
    testutil_check(cursor->next(cursor));
    testutil_check(cursor->get_key(cursor, &kstr));
    testutil_check(cursor->get_value(cursor, &vstr));
    testutil_assert(strcmp(kstr, "a") == 0);
    testutil_assert(strcmp(vstr, "0") == 0);
    testutil_check(cursor->next(cursor));
    testutil_check(cursor->get_key(cursor, &kstr));
    testutil_check(cursor->get_value(cursor, &vstr));
    testutil_assert(strcmp(kstr, "b") == 0);
    testutil_assert(strcmp(vstr, "1") == 0);
    testutil_assert(cursor->next(cursor) == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));
    printf("Success\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
