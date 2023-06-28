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
 * uri_path --
 *     Return the path to an object file, and optionally, the object name.
 */
static void
uri_path(TABLE *table, char **object_namep, char *buf, size_t len)
{
    char *p;

    /*
     * It's a little tricky: if the data source is a file, we're looking for the table URI, if the
     * data source is a table, we're looking for the table URI with a trailing ".wt".
     */
    p = strchr(table->uri, ':');
    testutil_assert(p != NULL);
    ++p;

    testutil_check(__wt_snprintf(buf, len, "%s/%s", g.home, p));
    if (object_namep != NULL)
        *object_namep = strrchr(buf, '/') + 1;
    if (!access(buf, F_OK))
        return;
    testutil_check(__wt_snprintf(buf, len, "%s/%s.wt", g.home, p));
    if (object_namep != NULL)
        *object_namep = strrchr(buf, '/') + 1;
    if (!access(buf, F_OK))
        return;
    testutil_die(0, "%s: unable to find file for salvage", table->uri);
}

/*
 * corrupt --
 *     Corrupt the file in a random way.
 */
static void
corrupt(TABLE *table)
{
    struct stat sb;
    FILE *fp;
    wt_off_t offset;
    size_t len, nw;
    int fd;
    char buf[MAX_FORMAT_PATH * 2], *object_name, path[MAX_FORMAT_PATH];
    const char *smash;

    uri_path(table, &object_name, path, sizeof(path));

    fd = open(path, O_RDWR);
    testutil_assert(fd != -1);

    /*
     * Corrupt a chunk of the file at a random spot, including the first bytes of the file and
     * possibly overlapping the end. The length of the corruption is roughly 2% of the file, not
     * exceeding a megabyte (so we aren't just corrupting the whole file).
     */
    testutil_check(fstat(fd, &sb));
    offset = mmrand(&g.data_rnd, 0, (u_int)sb.st_size - 1024);
    len = (size_t)(sb.st_size * 2) / 100;
    len += 4 * 1024;
    len = WT_MIN(len, WT_MEGABYTE);

    /* Log the corruption offset and length. */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "%s/SALVAGE.corrupt", g.home));
    testutil_assert((fp = fopen(buf, "w")) != NULL);
    (void)fprintf(fp, "salvage-corrupt: offset %" PRIuMAX ", length %" WT_SIZET_FMT "\n",
      (uintmax_t)offset, len);
    fclose_and_clear(&fp);

    testutil_assert(lseek(fd, offset, SEEK_SET) != -1);
    smash = "!!! memory corrupted by format to test salvage ";
    for (; len > 0; len -= nw) {
        nw = (size_t)(len > strlen(smash) ? strlen(smash) : len);
        testutil_assert(write(fd, smash, nw) != -1);
    }

    testutil_check(close(fd));

    /* Save a copy of the corrupted file so we can replay the salvage step as necessary.  */
    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "cp %s %s/SALVAGE.copy/%s.corrupted", path, g.home, object_name));
    testutil_check(system(buf));
}

/* Salvage command, save the interesting files so we can replay the salvage command as necessary. */
#define SALVAGE_COPY_CMD \
    "rm -rf %s/SALVAGE.copy && mkdir %s/SALVAGE.copy && cp %s/WiredTiger* %s %s/SALVAGE.copy/"

/*
 * wts_salvage --
 *     Salvage testing.
 */
void
wts_salvage(TABLE *table, void *arg)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    char buf[MAX_FORMAT_PATH * 5], path[MAX_FORMAT_PATH];

    (void)arg; /* unused argument */
    testutil_assert(table != NULL);

    if (GV(OPS_SALVAGE) == 0 || DATASOURCE(table, "lsm"))
        return;

    /* Save a copy of the interesting files so we can replay the salvage step as necessary. */
    uri_path(table, NULL, path, sizeof(path));
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), SALVAGE_COPY_CMD, g.home, g.home, g.home, path, g.home));
    testutil_check(system(buf));

    /* Salvage, then verify. */
    wts_open(g.home, &conn, true);
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, table->track_prefix, &session);
    testutil_check(session->salvage(session, table->uri, "force=true"));
    table_verify(table, conn);
    wts_close(&conn);

    /* Corrupt the file randomly, salvage, then verify. */
    corrupt(table);
    wts_open(g.home, &conn, false);
    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, table->track_prefix, &session);
    testutil_check(session->salvage(session, table->uri, "force=true"));
    table_verify(table, conn);
    wts_close(&conn);
}
