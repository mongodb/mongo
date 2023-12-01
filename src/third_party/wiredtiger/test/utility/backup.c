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
 * testutil_backup_create_full --
 *     Perform a full backup. Optionally return the number of files copied through the output
 *     parameter.
 */
void
testutil_backup_create_full(WT_CONNECTION *conn, const char *home_dir, const char *backup_dir,
  const char *backup_id, bool consolidate, uint32_t granularity_kb, int *p_nfiles)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int nfiles, ret;
    char buf[PATH_MAX];
    char copy_from[PATH_MAX], copy_to[PATH_MAX];
    char *filename;

    nfiles = 0;

    /* Prepare the directory. */
    testutil_remove(backup_dir);
    testutil_mkdir(backup_dir);

    /* Open the session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the backup cursor to get the list of files to copy. */
    testutil_snprintf(buf, sizeof(buf),
      "incremental=(granularity=%" PRIu32 "K,enabled=true,consolidate=%s,this_id=%s)",
      granularity_kb, consolidate ? "true" : "false", backup_id);
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    /* Copy the files. */
    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));
        testutil_snprintf(
          copy_from, sizeof(copy_from), "%s" DIR_DELIM_STR "%s", home_dir, filename);
        testutil_snprintf(copy_to, sizeof(copy_to), "%s" DIR_DELIM_STR "%s", backup_dir, filename);
        testutil_copy(copy_from, copy_to);
    }
    testutil_assert(ret == WT_NOTFOUND);

    /* Cleanup */
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));

    if (p_nfiles != NULL)
        *p_nfiles = nfiles;
}

/*
 * testutil_backup_create_incremental --
 *     Perform an incremental backup. Optionally return the relevant statistics (number of files,
 *     number of ranges, and number of unmodified files) through the corresponding output
 *     parameters.
 */
void
testutil_backup_create_incremental(WT_CONNECTION *conn, const char *home_dir,
  const char *backup_dir, const char *backup_id, const char *source_dir, const char *source_id,
  bool verbose, int *p_nfiles, int *p_nranges, int *p_nunmodified)
{
    WT_CURSOR *cursor, *file_cursor;
    WT_SESSION *session;
    ssize_t rdsize;
    uint64_t offset, size, type;
    int ret, rfd, wfd, nfiles, nranges, nunmodified;
    char buf[4096];
    char copy_from[PATH_MAX], copy_to[PATH_MAX];
    char *filename;
    bool first_range;
    void *tmp;

    nfiles = 0;
    nranges = 0;
    nunmodified = 0;

    /* Prepare the directory. */
    testutil_remove(backup_dir);
    testutil_mkdir(backup_dir);

    /* Open the session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the backup cursor to get the list of files to copy. */
    testutil_snprintf(buf, sizeof(buf), "incremental=(src_id=%s,this_id=%s)", source_id, backup_id);
    testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));

    /* Process the files. */
    while ((ret = cursor->next(cursor)) == 0) {
        nfiles++;
        testutil_check(cursor->get_key(cursor, &filename));

        /* Process ranges within each file. */
        testutil_snprintf(buf, sizeof(buf), "incremental=(file=%s)", filename);
        testutil_check(session->open_cursor(session, NULL, cursor, buf, &file_cursor));

        first_range = true;
        rfd = wfd = 0;
        while ((ret = file_cursor->next(file_cursor)) == 0) {
            testutil_check(file_cursor->get_key(file_cursor, &offset, &size, &type));
            testutil_assert(type == WT_BACKUP_FILE || type == WT_BACKUP_RANGE);

            if (type == WT_BACKUP_RANGE) {
                /*
                 * If this is the first range, copy the base file from the source backup. Then open
                 * the source and target files to set up the rest of the copy.
                 */
                if (first_range) {
                    first_range = false;
                    testutil_snprintf(
                      copy_from, sizeof(copy_from), "%s" DIR_DELIM_STR "%s", source_dir, filename);
                    testutil_snprintf(
                      copy_to, sizeof(copy_to), "%s" DIR_DELIM_STR "%s", backup_dir, filename);
                    testutil_copy(copy_from, copy_to);
                    if (verbose) {
                        printf("INCR: cp %s %s\n", copy_from, copy_to);
                        fflush(stdout);
                    }

                    testutil_snprintf(buf, sizeof(buf), "%s/%s", WT_HOME_DIR, filename);
                    testutil_assert((rfd = open(buf, O_RDONLY, 0666)) >= 0);
                    testutil_assert((wfd = open(copy_to, O_WRONLY | O_CREAT, 0666)) >= 0);
                }

                /* Copy the range. */
                tmp = dcalloc(1, size);
                testutil_assert_errno(lseek(rfd, (wt_off_t)offset, SEEK_SET) >= 0);
                rdsize = read(rfd, tmp, (size_t)size);
                testutil_assert(rdsize >= 0);
                testutil_assert_errno(lseek(wfd, (wt_off_t)offset, SEEK_SET) >= 0);
                testutil_assert_errno(write(wfd, tmp, (size_t)rdsize) == rdsize);
                free(tmp);

                nranges++;
            } else {
                /* We are supposed to do the full file copy. */
                testutil_assert(first_range);
                first_range = false;
                testutil_snprintf(
                  copy_from, sizeof(copy_from), "%s" DIR_DELIM_STR "%s", home_dir, filename);
                testutil_snprintf(
                  copy_to, sizeof(copy_to), "%s" DIR_DELIM_STR "%s", backup_dir, filename);
                testutil_copy(copy_from, copy_to);
            }
        }

        if (rfd > 0)
            testutil_assert(close(rfd) == 0);
        if (wfd > 0)
            testutil_assert(close(wfd) == 0);

        if (first_range) {
            /*
             * If we get here and first_range is still true, it means that there were no changes to
             * the file. The duplicate backup cursor did not return any information. Therefore
             * "copy" the file from the source backup (actually, just create a hard link as an
             * optimization).
             */
            testutil_snprintf(
              copy_from, sizeof(copy_from), "%s" DIR_DELIM_STR "%s", source_dir, filename);
            testutil_snprintf(
              copy_to, sizeof(copy_to), "%s" DIR_DELIM_STR "%s", backup_dir, filename);
#ifndef _WIN32
            testutil_assert_errno(link(copy_from, copy_to) == 0);
#else
            testutil_copy(copy_from, copy_to);
#endif
            nunmodified++;
        }

        testutil_assert(ret == WT_NOTFOUND);
        testutil_check(file_cursor->close(file_cursor));
    }
    testutil_assert(ret == WT_NOTFOUND);

    /* Cleanup */
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, NULL));

    if (p_nfiles != NULL)
        *p_nfiles = nfiles;
    if (p_nranges != NULL)
        *p_nranges = nranges;
    if (p_nunmodified != NULL)
        *p_nunmodified = nunmodified;
}

/*
 * testutil_backup_force_stop --
 *     Force-stop incremental backups.
 */
void
testutil_backup_force_stop(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;

    /* Force-stop incremental backups. */
    testutil_check(
      session->open_cursor(session, "backup:", NULL, "incremental=(force_stop=true)", &cursor));
    testutil_check(cursor->close(cursor));

    /* Check that we don't have any backup info. */
    ret = session->open_cursor(session, "backup:query_id", NULL, NULL, &cursor);
    testutil_assert(ret == EINVAL);
}

/*
 * testutil_backup_force_stop_conn --
 *     Force-stop incremental backups.
 */
void
testutil_backup_force_stop_conn(WT_CONNECTION *conn)
{
    WT_SESSION *session;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_backup_force_stop(session);
    testutil_check(session->close(session, NULL));
}
