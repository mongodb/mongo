/*-
 * Public Domain 2014-2020 MongoDB, Inc.
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

#define REBALANCE_COPY_CMD "../../wt -h %s dump -f %s/REBALANCE.%s %s"
#define REBALANCE_CMP_CMD "cmp %s/REBALANCE.orig %s/REBALANCE.new > /dev/null"

void
wts_rebalance(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    size_t len;
    char *cmd;

    if (g.c_rebalance == 0)
        return;

    track("rebalance", 0ULL, NULL);

    /* Dump the current object */
    len = strlen(g.home) * 2 + strlen(g.uri) + strlen(REBALANCE_COPY_CMD) + 100;
    cmd = dmalloc(len);
    testutil_check(__wt_snprintf(cmd, len, REBALANCE_COPY_CMD, g.home, g.home, "orig", g.uri));
    testutil_checkfmt(system(cmd), "command failed: %s", cmd);

    /* Rebalance, then verify the object. */
    wts_reopen();
    conn = g.wts_conn;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    logop(session, "%s", "=============== rebalance start");

    testutil_checkfmt(session->rebalance(session, g.uri, NULL), "%s", g.uri);

    logop(session, "%s", "=============== rebalance stop");
    testutil_check(session->close(session, NULL));

    wts_verify("post-rebalance verify");
    wts_close();
    testutil_check(__wt_snprintf(cmd, len, REBALANCE_COPY_CMD, g.home, g.home, "new", g.uri));
    testutil_checkfmt(system(cmd), "command failed: %s", cmd);

    /* Compare the old/new versions of the object. */
    testutil_check(__wt_snprintf(cmd, len, REBALANCE_CMP_CMD, g.home, g.home));
    testutil_checkfmt(system(cmd), "command failed: %s", cmd);

    free(cmd);
}
