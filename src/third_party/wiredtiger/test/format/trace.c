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

#include "format.h"

#define TRACE_DIR "OPS.TRACE"
#define TRACE_INIT_CMD "rm -rf %s/" TRACE_DIR " && mkdir %s/" TRACE_DIR

/*
 * trace_config --
 *     Configure operation tracing.
 */
void
trace_config(const char *config)
{
    char *copy, *p;

    copy = dstrdup(config);
    for (;;) {
        if ((p = strstr(copy, "all")) != NULL) {
            g.trace_all = true;
            memset(p, ' ', strlen("all"));
            continue;
        }
        if ((p = strstr(copy, "local")) != NULL) {
            g.trace_local = true;
            memset(p, ' ', strlen("local"));
            continue;
        }
        break;
    }

    for (p = copy; *p != '\0'; ++p)
        if (*p != ',' && !__wt_isspace((u_char)*p))
            testutil_assertfmt(0, "unexpected trace configuration \"%s\"\n", config);

    free(copy);
}

/*
 * trace_init --
 *     Initialize operation tracing.
 */
void
trace_init(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    size_t len;
    char *p;
    const char *config;

    if (!g.trace)
        return;

    /* Write traces to a separate database by default, optionally write traces to the primary. */
    if (g.trace_local) {
        if (!GV(LOGGING))
            testutil_die(EINVAL,
              "operation logging to the primary database requires logging be configured for that "
              "database");

        conn = g.wts_conn;

        /* Keep the last N log files. */
        testutil_check(conn->reconfigure(conn, "debug_mode=(log_retention=10)"));
    } else {
        len = strlen(g.home) * 2 + strlen(TRACE_INIT_CMD) + 10;
        p = dmalloc(len);
        testutil_check(__wt_snprintf(p, len, TRACE_INIT_CMD, g.home, g.home));
        testutil_checkfmt(system(p), "%s", "logging directory creation failed");
        free(p);

        /* Configure logging with archival, and keep the last N log files. */
        len = strlen(g.home) * strlen(TRACE_DIR) + 10;
        p = dmalloc(len);
        testutil_check(__wt_snprintf(p, len, "%s/%s", g.home, TRACE_DIR));
        config = "create,log=(enabled,archive),debug_mode=(log_retention=10)";
        testutil_checkfmt(wiredtiger_open(p, NULL, config, &conn), "%s: %s", p, config);
        free(p);
    }

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    g.trace_conn = conn;
    g.trace_session = session;
}

/*
 * trace_teardown --
 *     Close operation tracing.
 */
void
trace_teardown(void)
{
    WT_CONNECTION *conn;

    conn = g.trace_conn;
    g.trace_conn = NULL;

    if (conn != NULL)
        testutil_check(conn->close(conn, NULL));
}

/*
 * trace_ops_init --
 *     Per thread operation tracing setup.
 */
void
trace_ops_init(TINFO *tinfo)
{
    WT_SESSION *session;

    if (!g.trace)
        return;

    testutil_check(g.trace_conn->open_session(g.trace_conn, NULL, NULL, &session));
    tinfo->trace = session;
}
