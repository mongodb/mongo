/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
/* I'm sure we need to config this */
#include <dirent.h>

/*
 * __directory_list_worker --
 *     Get a list of files from a directory, POSIX version.
 */
static int
__directory_list_worker(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp, bool single)
{
    struct dirent *dp;
    DIR *dirp;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t dirallocsz;
    uint32_t count;
    int tret;
    char **entries;

    *dirlistp = NULL;
    *countp = 0;

    session = (WT_SESSION_IMPL *)wt_session;
    dirp = NULL;
    dirallocsz = 0;
    entries = NULL;

    /*
     * If opendir fails, we should have a NULL pointer with an error value, but various static
     * analysis programs remain unconvinced, check both.
     */
    WT_SYSCALL_RETRY(((dirp = opendir(directory)) == NULL ? -1 : 0), ret);
    if (dirp == NULL || ret != 0) {
        if (ret == 0)
            ret = EINVAL;
        WT_RET_MSG(session, ret, "%s: directory-list: opendir", directory);
    }

    for (count = 0; (dp = readdir(dirp)) != NULL;) {
        /*
         * Skip . and ..
         */
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
            continue;

        /* The list of files is optionally filtered by a prefix. */
        if (prefix != NULL && !WT_PREFIX_MATCH(dp->d_name, prefix))
            continue;

        WT_ERR(__wt_realloc_def(session, &dirallocsz, count + 1, &entries));
        WT_ERR(__wt_strdup(session, dp->d_name, &entries[count]));
        ++count;

        if (single)
            break;
    }

    *dirlistp = entries;
    *countp = count;

err:
    WT_SYSCALL(closedir(dirp), tret);
    if (tret != 0) {
        __wt_err(session, tret, "%s: directory-list: closedir", directory);
        if (ret == 0)
            ret = tret;
    }

    if (ret == 0)
        return (0);

    WT_TRET(__wt_posix_directory_list_free(file_system, wt_session, entries, count));

    WT_RET_MSG(
      session, ret, "%s: directory-list, prefix \"%s\"", directory, prefix == NULL ? "" : prefix);
}

/*
 * __wt_posix_directory_list --
 *     Get a list of files from a directory, POSIX version.
 */
int
__wt_posix_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, false));
}

/*
 * __wt_posix_directory_list_single --
 *     Get one file from a directory, POSIX version.
 */
int
__wt_posix_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, true));
}

/*
 * __wt_posix_directory_list_free --
 *     Free memory returned by __wt_posix_directory_list.
 */
int
__wt_posix_directory_list_free(
  WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, char **dirlist, uint32_t count)
{
    WT_SESSION_IMPL *session;

    WT_UNUSED(file_system);

    session = (WT_SESSION_IMPL *)wt_session;

    if (dirlist != NULL) {
        while (count > 0)
            __wt_free(session, dirlist[--count]);
        __wt_free(session, dirlist);
    }
    return (0);
}
