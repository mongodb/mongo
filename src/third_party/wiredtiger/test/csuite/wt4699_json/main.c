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
 * JIRA ticket reference: WT-4699 Test case description: Use a JSON dump cursor on a projection, and
 * overwrite the projection string. Failure mode: On the first retrieval of a JSON key/value, a
 * configure parse error is returned.
 */

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *c;
    WT_SESSION *session;
    char *jsonkey, *jsonvalue;
    char projection[WT_THOUSAND];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    /* Create a single record in a table with two fields in its value. */
    testutil_check(
      session->create(session, opts->uri, "key_format=i,value_format=ii,columns=(k,v0,v1)"));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &c));
    c->set_key(c, 1);
    c->set_value(c, 1, 1);
    testutil_check(c->insert(c));
    testutil_check(c->close(c));

    /*
     * Open a dump JSON cursor on a projection of the table. The fields will be listed in a
     * different order.
     */
    strcpy(projection, opts->uri);
    strcat(projection, "(v1,v0,k)");
    testutil_check(session->open_cursor(session, projection, NULL, "dump=json", &c));
    testutil_check(c->next(c));
    /* Overwrite the projection, with not enough columns */
    strcpy(projection, opts->uri);
    strcat(projection, "(aaa,bbb)");
    testutil_check(c->get_key(c, &jsonkey));

    /*
     * Here's where we would get the parse error. When a JSON dump is performed on a projection, we
     * need to format all the field names and values in the order listed. The implementation uses
     * the projection string from the open_cursor call to determine the field names.
     */
    testutil_check(c->get_value(c, &jsonvalue));
    testutil_assert(strstr(jsonvalue, "aaa") == NULL);
    printf("KEY: %s\n", jsonkey);
    printf("VALUE: %s\n", jsonvalue);
    testutil_assert(c->next(c) == WT_NOTFOUND);
    testutil_check(c->close(c));
    testutil_check(session->close(session, NULL));
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
