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
    struct timespec ts;
    DIR *dirp;
    WT_DECL_ITEM(closemsg);
    WT_DECL_ITEM(openmsg);
    WT_DECL_ITEM(readerrmsg);
    WT_DECL_ITEM(readmsg);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t dirallocsz;
    uint32_t count;
    int tret;
    char **entries;
    bool err_msg, open_ready, read_ready;

    *dirlistp = NULL;
    *countp = count = 0;

    session = (WT_SESSION_IMPL *)wt_session;
    dirp = NULL;
    dirallocsz = 0;
    entries = NULL;
    err_msg = open_ready = read_ready = false;

    WT_ERR(__wt_scr_alloc(session, 0, &closemsg));
    WT_ERR(__wt_scr_alloc(session, 0, &openmsg));
    WT_ERR(__wt_scr_alloc(session, 0, &readerrmsg));
    WT_ERR(__wt_scr_alloc(session, 0, &readmsg));
    /*
     * If opendir fails, we should have a NULL pointer with an error value, but various static
     * analysis programs remain unconvinced, check both.
     */
    WT_SYSCALL_RETRY(((dirp = opendir(directory)) == NULL ? -1 : 0), ret);
    if (dirp == NULL || ret != 0) {
        if (ret == 0)
            ret = EINVAL;
        WT_ERR_MSG(session, ret, "%s: directory-list: opendir", directory);
    }
    /*
     * There has been a very rare error where calling closedir returns an error indicating a bad
     * file descriptor. Save some state in messages so that if that failure happens we can print the
     * messages out to give some clues.
     */
    __wt_epoch(session, &ts);
    WT_ERR(__wt_buf_fmt(session, openmsg,
      "[%" PRIuMAX ":%" PRIuMAX "] opendir (%s) prefix %s dir fd %d", (uintmax_t)ts.tv_sec,
      (uintmax_t)ts.tv_nsec / WT_THOUSAND, directory, prefix == NULL ? "" : prefix, dirfd(dirp)));
    open_ready = true;

    errno = 0;
    for (count = 0; (dp = readdir(dirp)) != NULL;) {
        /*
         * Skip . and ..
         */
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
            continue;

        __wt_epoch(session, &ts);
        WT_ERR(__wt_buf_fmt(session, readmsg, "[%" PRIuMAX ":%" PRIuMAX "] readdir (%s) dir fd %d",
          (uintmax_t)ts.tv_sec, (uintmax_t)ts.tv_nsec / WT_THOUSAND, dp->d_name, dirfd(dirp)));
        read_ready = true;
        /* The list of files is optionally filtered by a prefix. */
        if (prefix != NULL && !WT_PREFIX_MATCH(dp->d_name, prefix))
            continue;

        WT_ERR(__wt_realloc_def(session, &dirallocsz, count + 1, &entries));
        WT_ERR(__wt_strdup(session, dp->d_name, &entries[count]));
        ++count;

        if (single)
            break;
    }
    /*
     * Reading the directory returns NULL on failure or reaching the end of the list. Record a
     * special message if readdir failed.
     */
    if (errno != 0) {
        ret = errno;
        __wt_epoch(session, &ts);
        WT_ERR(__wt_buf_fmt(session, readerrmsg,
          "[%" PRIuMAX ":%" PRIuMAX "] readdir failed errno %d (%s) dir fd %d",
          (uintmax_t)ts.tv_sec, (uintmax_t)ts.tv_nsec / WT_THOUSAND, ret,
          __wt_strerror(session, ret, NULL, 0), dirfd(dirp)));
        err_msg = true;
    }
    *dirlistp = entries;
    *countp = count;

err:
    if (dirp != NULL) {
        __wt_epoch(session, &ts);
        WT_TRET(__wt_buf_fmt(session, closemsg,
          "[%" PRIuMAX ":%" PRIuMAX "] closedir (%s) ret %d dir fd %d", (uintmax_t)ts.tv_sec,
          (uintmax_t)ts.tv_nsec / WT_THOUSAND, directory, ret, dirfd(dirp)));
        WT_SYSCALL(closedir(dirp), tret);
        if (tret != 0) {
            __wt_err(session, tret, "%s: directory-list: closedir", directory);
            if (ret == 0)
                ret = tret;
            /* If we have an error print information about the run. */
            if (open_ready)
                __wt_errx(session, "%s", (const char *)openmsg->data);
            if (read_ready)
                __wt_errx(session, "%s", (const char *)readmsg->data);
            if (err_msg)
                __wt_errx(session, "%s", (const char *)readerrmsg->data);
            __wt_errx(session, "%s", (const char *)closemsg->data);
        }
    }
    __wt_scr_free(session, &closemsg);
    __wt_scr_free(session, &openmsg);
    __wt_scr_free(session, &readerrmsg);
    __wt_scr_free(session, &readmsg);

    if (ret == 0)
        return (0);

    WT_TRET(__wti_posix_directory_list_free(file_system, wt_session, entries, count));

    WT_RET_MSG(
      session, ret, "%s: directory-list, prefix \"%s\"", directory, prefix == NULL ? "" : prefix);
}

/*
 * __wti_posix_directory_list --
 *     Get a list of files from a directory, POSIX version.
 */
int
__wti_posix_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, false));
}

/*
 * __wti_posix_directory_list_single --
 *     Get one file from a directory, POSIX version.
 */
int
__wti_posix_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, true));
}

/*
 * __wti_posix_directory_list_free --
 *     Free memory returned by __wti_posix_directory_list.
 */
int
__wti_posix_directory_list_free(
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
