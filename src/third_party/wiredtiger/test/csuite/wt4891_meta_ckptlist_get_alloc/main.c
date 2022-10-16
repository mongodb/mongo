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
#define CHECKPOINT_COUNT 10

/*
 * JIRA ticket reference: WT-4891 Test case description: Test wt_meta_ckptlist_get by creating a
 * number of checkpoints and then running __wt_verify. Failure mode: If the bug still exists then
 * this test will cause an error in address sanitized builds.
 */

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *cursor, *cursor_ckpt;
    WT_SESSION *session;
    int i;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->create(session, opts->uri, "key_format=S,value_format=i"));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));

    /*
     * Create checkpoints and keep them active by around by opening a checkpoint cursor for each
     * one.
     */
    for (i = 0; i < CHECKPOINT_COUNT; ++i) {
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));
        cursor->set_key(cursor, "key1");
        cursor->set_value(cursor, i);
        testutil_check(cursor->update(cursor));
        testutil_check(session->commit_transaction(session, NULL));
        testutil_check(session->checkpoint(session, NULL));
        testutil_check(session->open_cursor(
          session, opts->uri, NULL, "checkpoint=WiredTigerCheckpoint", &cursor_ckpt));
    }

    testutil_check(session->close(session, NULL));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->verify(session, opts->uri, NULL));

    testutil_cleanup(opts);

    return (0);
}
