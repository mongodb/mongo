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

/*
 * corrupt --
 *     Corrupt the file in a random way.
 */
static int
corrupt(void)
{
    struct stat sb;
    FILE *fp;
    wt_off_t offset;
    size_t len, nw;
    int fd, ret;
    char copycmd[2 * 1024], path[1024];
    const char *smash;

    /*
     * If it's a single Btree file (not LSM), open the file, and corrupt roughly 2% of the file at a
     * random spot, including the beginning of the file and overlapping the end.
     *
     * It's a little tricky: if the data source is a file, we're looking for "wt", if the data
     * source is a table, we're looking for "wt.wt".
     */
    testutil_check(__wt_snprintf(path, sizeof(path), "%s/%s", g.home, WT_NAME));
    if ((fd = open(path, O_RDWR)) != -1) {
        testutil_check(__wt_snprintf(copycmd, sizeof(copycmd),
          "cp %s/%s %s/SALVAGE.copy/%s.corrupted", g.home, WT_NAME, g.home, WT_NAME));
        goto found;
    }
    testutil_check(__wt_snprintf(path, sizeof(path), "%s/%s.wt", g.home, WT_NAME));
    if ((fd = open(path, O_RDWR)) != -1) {
        testutil_check(__wt_snprintf(copycmd, sizeof(copycmd),
          "cp %s/%s.wt %s/SALVAGE.copy/%s.wt.corrupted", g.home, WT_NAME, g.home, WT_NAME));
        goto found;
    }
    return (0);

found:
    if (fstat(fd, &sb) == -1)
        testutil_die(errno, "salvage-corrupt: fstat");

    offset = mmrand(NULL, 0, (u_int)sb.st_size);
    len = (size_t)(20 + (sb.st_size / 100) * 2);
    testutil_check(__wt_snprintf(path, sizeof(path), "%s/SALVAGE.corrupt", g.home));
    if ((fp = fopen(path, "w")) == NULL)
        testutil_die(errno, "salvage-corrupt: open: %s", path);
    (void)fprintf(fp, "salvage-corrupt: offset %" PRIuMAX ", length %" WT_SIZET_FMT "\n",
      (uintmax_t)offset, len);
    fclose_and_clear(&fp);

    if (lseek(fd, offset, SEEK_SET) == -1)
        testutil_die(errno, "salvage-corrupt: lseek");

    smash = "!!! memory corrupted by format to test salvage ";
    for (; len > 0; len -= nw) {
        nw = (size_t)(len > strlen(smash) ? strlen(smash) : len);
        if (write(fd, smash, nw) == -1)
            testutil_die(errno, "salvage-corrupt: write");
    }

    if (close(fd) == -1)
        testutil_die(errno, "salvage-corrupt: close");

    /*
     * Save a copy of the corrupted file so we can replay the salvage step as necessary.
     */
    if ((ret = system(copycmd)) != 0)
        testutil_die(ret, "salvage corrupt copy step failed");

    return (1);
}

/*
 * Salvage command, save the interesting files so we can replay the salvage command as necessary.
 *
 * Redirect the "cd" command to /dev/null so chatty cd implementations don't add the new working
 * directory to our output.
 */
#define SALVAGE_COPY_CMD                            \
    "cd %s > /dev/null && "                         \
    "rm -rf SALVAGE.copy && mkdir SALVAGE.copy && " \
    "cp WiredTiger* wt* SALVAGE.copy/"

/*
 * wts_salvage --
 *     Salvage testing.
 */
void
wts_salvage(void)
{
    WT_CONNECTION *conn;
    WT_DECL_RET;
    WT_SESSION *session;
    size_t len;
    char *cmd;

    if (g.c_salvage == 0)
        return;

    track("salvage", 0ULL, NULL);

    /* Save a copy of the interesting files so we can replay the salvage step as necessary. */
    len = strlen(g.home) + strlen(SALVAGE_COPY_CMD) + 1;
    cmd = dmalloc(len);
    testutil_check(__wt_snprintf(cmd, len, SALVAGE_COPY_CMD, g.home));
    if ((ret = system(cmd)) != 0)
        testutil_die(ret, "salvage copy (\"%s\"), failed", cmd);
    free(cmd);

    /* Salvage, then verify. */
    wts_open(g.home, &conn, &session, true);
    testutil_check(session->salvage(session, g.uri, "force=true"));
    wts_verify(conn, "post-salvage verify");
    wts_close(&conn, &session);

    /* Corrupt the file randomly, salvage, then verify. */
    if (corrupt()) {
        wts_open(g.home, &conn, &session, false);
        testutil_check(session->salvage(session, g.uri, "force=true"));
        wts_verify(conn, "post-corrupt-salvage verify");
        wts_close(&conn, &session);
    }
}
