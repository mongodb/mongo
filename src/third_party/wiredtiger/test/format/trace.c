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

#define TRACE_CONFIG_CMD(cmd, flag)        \
    if ((p = strstr(copy, cmd)) != NULL) { \
        FLD_SET(g.trace_flags, flag);      \
        memset(p, ' ', strlen(cmd));       \
        continue;                          \
    }

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
        TRACE_CONFIG_CMD("all", TRACE_ALL);
        TRACE_CONFIG_CMD("bulk", TRACE_BULK);
        TRACE_CONFIG_CMD("cursor", TRACE_CURSOR);
        TRACE_CONFIG_CMD("mirror_fail", TRACE_MIRROR_FAIL);
        TRACE_CONFIG_CMD("read", TRACE_READ);
        TRACE_CONFIG_CMD("timestamp", TRACE_TIMESTAMP);
        TRACE_CONFIG_CMD("txn", TRACE_TXN);

        if ((p = strstr(copy, "retain=")) != NULL) {
            g.trace_retain = atoi(p + strlen("retain="));
            for (; *p != '='; ++p)
                *p = ' ';
            for (*p++ = ' '; __wt_isdigit((u_char)*p); ++p)
                *p = ' ';
            continue;
        }
        break;
    }

    for (p = copy; *p != '\0'; ++p)
        if (*p != ',' && !__wt_isspace((u_char)*p))
            testutil_assertfmt(0, "unexpected trace configuration \"%s\"\n", config);

    free(copy);

    FLD_SET(g.trace_flags, TRACE);
}

#define TRACE_DIR "OPS.TRACE"
#define TRACE_INIT_CMD "rm -rf %s/" TRACE_DIR " && mkdir %s/" TRACE_DIR

/*
 * trace_init --
 *     Initialize operation tracing.
 */
void
trace_init(void)
{
    int retain;
    char config[256], tracedir[MAX_FORMAT_PATH * 2];

    if (!FLD_ISSET(g.trace_flags, TRACE))
        return;

    /* Retain a minimum of 10 log files. */
    retain = WT_MAX(g.trace_retain, 10);

    /* Create the trace directory. */
    testutil_check(__wt_snprintf(tracedir, sizeof(tracedir), TRACE_INIT_CMD, g.home, g.home));
    testutil_checkfmt(system(tracedir), "%s", "logging directory creation failed");

    /* Configure logging with automatic removal, and keep the last N log files. */
    testutil_check(__wt_snprintf(config, sizeof(config),
      "create,log=(enabled=true,remove=true),debug_mode=(log_retention=%d),statistics=(fast),"
      "statistics_log=(json,on_close,wait=5)",
      retain));
    testutil_check(__wt_snprintf(tracedir, sizeof(tracedir), "%s/%s", g.home, TRACE_DIR));
    testutil_checkfmt(
      wiredtiger_open(tracedir, NULL, config, &g.trace_conn), "%s: %s", tracedir, config);

    /* Open a session and give it a lock. */
    testutil_check(g.trace_conn->open_session(g.trace_conn, NULL, NULL, &g.trace_session));
    testutil_check(
      __wt_spin_init((WT_SESSION_IMPL *)g.trace_session, &g.trace_lock, "format trace lock"));
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

    if (conn != NULL) {
        __wt_spin_destroy((WT_SESSION_IMPL *)g.trace_session, &g.trace_lock);
        testutil_check(conn->close(conn, NULL));
    }
}
