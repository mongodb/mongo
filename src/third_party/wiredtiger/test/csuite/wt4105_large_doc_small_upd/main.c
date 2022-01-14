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

#include <signal.h>

static const char *const uri = "table:large";

#define DATASIZE (1024 * 1024)
#define MODIFY_COUNT (1024)
#define NUM_DOCS 2

static void on_alarm(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * on_alarm --
 *     TODO: Add a comment describing this function.
 */
static void
on_alarm(int signo)
{
    (void)signo; /* Unused parameter */
    fprintf(stderr, "cursor->modify timed out \n");
    abort();

    /* NOTREACHED */
}

static int ignore_errors = 0;

/*
 * handle_error --
 *     TODO: Add a comment describing this function.
 */
static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);

    /* Skip the error messages we're expecting to see. */
    if (ignore_errors > 0 &&
      (strstr(message, "requires key be set") != NULL ||
        strstr(message, "requires value be set") != NULL)) {
        --ignore_errors;
        return (0);
    }

    (void)fprintf(stderr, "%s: %s\n", message, session->strerror(session, error));
    return (0);
}

static WT_EVENT_HANDLER event_handler = {handle_error, NULL, NULL, NULL};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *c;
    WT_ITEM value;
    WT_MODIFY modify_entry;
    WT_SESSION *session, *session2;
    uint64_t i, j, offset;
    char *large_doc, tableconf[128];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, &event_handler,
      "create,cache_size=1G,statistics_log=(json,wait=1)", &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=u,leaf_key_max=64M,leaf_value_max=64M,leaf_page_max=32k,memory_"
      "page_max=1M",
      opts->table_type == TABLE_ROW ? "Q" : "r"));
    testutil_check(session->create(session, uri, tableconf));

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &c));

    /* Value is initialized with 'v' and has no significance to it. */
    large_doc = dmalloc(DATASIZE);
    memset(large_doc, 'v', DATASIZE);
    value.data = large_doc;
    value.size = DATASIZE;

    /* Insert records. */
    for (i = 0; i < NUM_DOCS; i++) {
        c->set_key(c, i + 1);
        c->set_value(c, &value);
        testutil_check(c->insert(c));
    }

    testutil_check(c->close(c));
    if (opts->verbose)
        printf("%d documents inserted\n", NUM_DOCS);

    /* Setup Transaction to pin the cache */
    testutil_check(session->begin_transaction(session, "isolation=snapshot"));

    /* Set an alarm so we can debug hangs. */
    (void)signal(SIGALRM, on_alarm);

    /* Start another session to perform small updates. */
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session2));
    testutil_check(session2->open_cursor(session2, uri, NULL, NULL, &c));

    j = offset = 0;
    while (++j < MODIFY_COUNT) {
        for (i = 0; i < NUM_DOCS; i++) {
            /* Position the cursor. */
            testutil_check(session2->begin_transaction(session2, "isolation=snapshot"));
            c->set_key(c, i + 1);
            modify_entry.data.data = "abcdefghijklmnopqrstuvwxyz";
            modify_entry.data.size = strlen(modify_entry.data.data);
            modify_entry.offset = offset;
            modify_entry.size = modify_entry.data.size;
            /* FIXME-WT-6113: extend timeout to pass the test */
            (void)alarm(15);
            testutil_check(c->modify(c, &modify_entry, 1));
            (void)alarm(0);
            testutil_check(session2->commit_transaction(session2, NULL));
        }
        /*
         * Modify operations are done similar to append sequence. This has no bearing on the test
         * outcome.
         */
        offset += modify_entry.data.size;
        offset = offset < DATASIZE ? offset : 0;
        if (opts->verbose)
            printf("modify count %" PRIu64 "\n", j * NUM_DOCS);
    }

    free(large_doc);
    testutil_cleanup(opts);

    return (EXIT_SUCCESS);
}
