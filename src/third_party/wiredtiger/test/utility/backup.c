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
#ifndef _WIN32
#include <dirent.h>
#endif
#include "test_util.h"

/*
 * testutil_backup_create_full --
 *     Perform a full backup. Optionally return the number of files copied through the output
 *     parameter.
 */
void
testutil_backup_create_full(WT_CONNECTION *conn, const char *home_dir, int id, bool consolidate,
  uint32_t granularity_kb, int *p_nfiles)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int nfiles, ret;
    char backup_dir[PATH_MAX], backup_id[32], copy_from[PATH_MAX], copy_to[PATH_MAX];
    char buf[PATH_MAX];
    char *filename;

    nfiles = 0;

    testutil_snprintf(backup_dir, sizeof(backup_dir), BACKUP_BASE "%d", id);
    testutil_snprintf(backup_id, sizeof(backup_id), "ID%d", id);
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

    /* Remember that this was a full backup. */
    testutil_sentinel(backup_dir, "full");

    /* Remember that the backup finished successfully. */
    testutil_sentinel(backup_dir, "done");
}

/*
 * testutil_backup_create_incremental --
 *     Perform an incremental backup. Optionally return the relevant statistics (number of files,
 *     number of ranges, and number of unmodified files) through the corresponding output
 *     parameters.
 */
void
testutil_backup_create_incremental(WT_CONNECTION *conn, const char *home_dir, int backup_id,
  int source_id, bool verbose, int *p_nfiles, int *p_nranges, int *p_nunmodified)
{
    WT_CURSOR *cursor, *file_cursor;
    WT_SESSION *session;
    ssize_t rdsize;
    uint64_t offset, size, type;
    int ret, rfd, wfd, nfiles, nranges, nunmodified;
    char backup_dir[PATH_MAX], copy_from[PATH_MAX], copy_to[PATH_MAX], source_dir[PATH_MAX];
    char buf[4096];
    char *filename;
    bool first_range;
    void *tmp;

    nfiles = 0;
    nranges = 0;
    nunmodified = 0;

    testutil_snprintf(backup_dir, sizeof(backup_dir), BACKUP_BASE "%d", backup_id);
    testutil_snprintf(source_dir, sizeof(source_dir), BACKUP_BASE "%d", source_id);

    /* Prepare the directory. */
    testutil_remove(backup_dir);
    testutil_mkdir(backup_dir);

    /* Open the session. */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the backup cursor to get the list of files to copy. */
    testutil_snprintf(
      buf, sizeof(buf), "incremental=(src_id=ID%d,this_id=ID%d)", source_id, backup_id);
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

                    testutil_snprintf(buf, sizeof(buf), "%s/%s", home_dir, filename);
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
    /* Remember that the backup finished successfully. */
    testutil_sentinel(backup_dir, "done");
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

/*
 * __int_comparator --
 *     "int" comparator.
 */
static int
__int_comparator(const void *a, const void *b)
{
    return (*(int *)a - *(int *)b);
}

/*
 * testutil_delete_old_backups --
 *     Delete old backups, keeping just a few recent ones, so that we don't take too much space for
 *     no good reason.
 */
void
testutil_delete_old_backups(int retain)
{
#ifdef _WIN32
    int ndeleted;

    ndeleted = 0;
    WT_UNUSED(retain);
#else
    struct dirent *dir;
    DIR *d;
    size_t len;
    int count, i, indexes[256], last_full, ndeleted;
    char fromdir[256], todir[256];
    bool done;

    last_full = 0;
    len = strlen(BACKUP_BASE);
    ndeleted = 0;
    printf("Delete_old_backups: Retain %d\n", retain);
    do {
        done = true;
        testutil_assert_errno((d = opendir(".")) != NULL);
        count = 0;
        while ((dir = readdir(d)) != NULL) {
            if (strncmp(dir->d_name, BACKUP_BASE, len) == 0) {
                i = atoi(dir->d_name + len);
                indexes[count++] = i;

                /* If the backup failed to finish, delete it right away. */
                if (!testutil_exists(dir->d_name, "done")) {
                    printf(
                      "Delete_old_backups: Remove %s, backup failed to finish. No done sentinel "
                      "file.\n",
                      dir->d_name);
                    testutil_remove(dir->d_name);
                    ndeleted++;
                }

                /* Check if this is a full backup - we'd like to keep at least one. */
                if (testutil_exists(dir->d_name, "full"))
                    last_full = WT_MAX(last_full, i);

                /* If we have too many backups, finish next time. */
                if (count >= (int)(sizeof(indexes) / sizeof(*indexes))) {
                    done = false;
                    break;
                }
            }
        }
        testutil_check(closedir(d));
        if (count <= retain)
            break;

        __wt_qsort(indexes, (size_t)count, sizeof(*indexes), __int_comparator);
        for (i = 0; i < count - retain; i++) {
            if (indexes[i] == last_full)
                continue;
            testutil_snprintf(fromdir, sizeof(fromdir), BACKUP_BASE "%d", indexes[i]);
            testutil_snprintf(todir, sizeof(todir), BACKUP_OLD "%d", indexes[i]);
            /*
             * First rename the directory so that if a child process is killed during the remove the
             * verify function doesn't attempt to open a partial database.
             */
            printf("Delete_old_backups: Remove %s, renamed to %s\n", fromdir, todir);
            testutil_check(rename(fromdir, todir));
            testutil_remove(todir);
            ndeleted++;
        }
    } while (!done);

#endif
    printf("Deleted %d old backup%s\n", ndeleted, ndeleted == 1 ? "" : "s");
}

/*
 * testutil_create_backup_directory --
 *     TODO: Add a comment describing this function.
 */
void
testutil_create_backup_directory(const char *home)
{
    char buf[512];

    testutil_snprintf(buf, sizeof(buf), "%s" DIR_DELIM_STR "BACKUP", home);
    testutil_remove(buf);
    testutil_mkdir(buf);

    testutil_snprintf(buf, sizeof(buf), "%s" DIR_DELIM_STR "BACKUP.copy", home);
    testutil_remove(buf);
    testutil_mkdir(buf);
}

/*
 * testutil_verify_src_backup --
 *     Verify a backup source home directory against a backup directory for changes to blocks that
 *     are not marked as changed. If an ID is given, then the backup directory is only compared
 *     against that ID, otherwise walk and compare against all IDs.
 */
void
testutil_verify_src_backup(WT_CONNECTION *conn, const char *backup, const char *home, char *srcid)
{
    struct stat sb;
    WT_CURSOR *cursor, *file_cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t cmp_size, offset, prev_offset, size, type;
    int i, j;
    char buf[1024], *filename, *id[WT_BLKINCR_MAX];
    const char *idstr;

    WT_CLEAR(buf);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * If we are given a source ID, use it. Otherwise query the backup and check against all IDs
     * that exist in the system.
     */
    if (srcid == NULL) {
        /*
         * Even if expected, we may not find any backup IDs depending on scheduling of backups,
         * checkpoints and killing of a process. So backup IDs may not have been saved to disk. If
         * opening the backup query cursor gets EINVAL there is nothing to do.
         */
        ret = session->open_cursor(session, "backup:query_id", NULL, buf, &cursor);
        if (ret != 0) {
            if (ret == EINVAL)
                goto done;
            else
                testutil_check(ret);
        }
        i = 0;
        while ((ret = cursor->next(cursor)) == 0) {
            testutil_check(cursor->get_key(cursor, &idstr));
            id[i++] = dstrdup(idstr);
        }
        testutil_check(cursor->close(cursor));
    } else {
        id[0] = srcid;
        id[1] = NULL;
        i = 1;
    }
    testutil_assert(i <= WT_BLKINCR_MAX);

    /* Go through each id and open a backup cursor on it to test incremental values. */
    for (j = 0; j < i; ++j) {
        testutil_snprintf(buf, sizeof(buf), "incremental=(src_id=%s)", id[j]);
        testutil_check(session->open_cursor(session, "backup:", NULL, buf, &cursor));
        while ((ret = cursor->next(cursor)) == 0) {
            testutil_check(cursor->get_key(cursor, &filename));
            testutil_snprintf(buf, sizeof(buf), "incremental=(file=%s)", filename);
            testutil_check(session->open_cursor(session, NULL, cursor, buf, &file_cursor));
            prev_offset = 0;
            while ((ret = file_cursor->next(file_cursor)) == 0) {
                testutil_check(file_cursor->get_key(file_cursor, &offset, &size, &type));
                /* We only want to check ranges for files. So if it is a full file copy, ignore. */
                if (type != WT_BACKUP_RANGE)
                    break;
                testutil_snprintf(buf, sizeof(buf), "%s/%s", backup, filename);
                ret = stat(buf, &sb);
                /*
                 * The file may not exist in the backup directory. If the stat call doesn't succeed
                 * skip this file. If we're skipping changed blocks go to the next one.
                 */
                if (ret != 0)
                    break;
                /*
                 * If the block is changed we cannot check it (for differences, for example). The
                 * source id may be older and we've already copied the block, or not, so we don't
                 * know if it should be different or not. But if a block is indicated as unchanged
                 * then it better be identical.
                 */
                if (offset > prev_offset) {
                    /* Compare the unchanged chunk. */
                    cmp_size = offset - prev_offset;
                    testutil_system("cmp -n %" PRIu64 " %s/%s %s/%s %" PRIu64 " %" PRIu64, cmp_size,
                      home, filename, backup, filename, prev_offset, prev_offset);
                }
                prev_offset = offset + size;
            }
            testutil_check(file_cursor->close(file_cursor));
        }
        testutil_check(cursor->close(cursor));
        if (srcid == NULL)
            free(id[j]);
    }
done:
    testutil_check(session->close(session, NULL));
}

/*
 * testutil_copy_file --
 *     Copy a single file into the backup directories.
 */
void
testutil_copy_file(WT_SESSION *session, const char *name)
{
    size_t len;
    char *first, *second;

    len = strlen("BACKUP") + strlen(name) + 10;
    first = dmalloc(len);
    testutil_snprintf(first, len, "BACKUP/%s", name);
    testutil_check(__wt_copy_and_sync(session, name, first));

    /*
     * Save another copy of the original file to make debugging recovery errors easier.
     */
    len = strlen("BACKUP.copy") + strlen(name) + 10;
    second = dmalloc(len);
    testutil_snprintf(second, len, "BACKUP.copy/%s", name);
    testutil_check(__wt_copy_and_sync(session, first, second));

    free(first);
    free(second);
}
