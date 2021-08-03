/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __directory_list_worker --
 *     Get a list of files from a directory, MSVC version.
 */
static int
__directory_list_worker(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp, bool single)
{
    DWORD windows_error;
    HANDLE findhandle;
    WIN32_FIND_DATAW finddata;
    WT_DECL_ITEM(pathbuf);
    WT_DECL_ITEM(file_utf8);
    WT_DECL_ITEM(pathbuf_wide);
    WT_DECL_ITEM(prefix_wide);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t dirallocsz, pathlen, prefix_widelen;
    uint32_t count;
    char *dir_copy, **entries;

    session = (WT_SESSION_IMPL *)wt_session;

    *dirlistp = NULL;
    *countp = 0;

    findhandle = INVALID_HANDLE_VALUE;
    dirallocsz = 0;
    entries = NULL;

    WT_ERR(__wt_strdup(session, directory, &dir_copy));
    pathlen = strlen(dir_copy);
    if (dir_copy[pathlen - 1] == '\\')
        dir_copy[pathlen - 1] = '\0';
    WT_ERR(__wt_scr_alloc(session, pathlen + 3, &pathbuf));
    WT_ERR(__wt_buf_fmt(session, pathbuf, "%s\\*", dir_copy));

    WT_ERR(__wt_to_utf16_string(session, pathbuf->data, &pathbuf_wide));
    WT_ERR(__wt_to_utf16_string(session, prefix, &prefix_wide));
    prefix_widelen = wcslen(prefix_wide->data);

    findhandle = FindFirstFileW(pathbuf_wide->data, &finddata);
    if (findhandle == INVALID_HANDLE_VALUE) {
        windows_error = __wt_getlasterror();
        ret = __wt_map_windows_error(windows_error);
        __wt_err(session, ret, "%s: directory-list: FindFirstFile: %s", pathbuf->data,
          __wt_formatmessage(session, windows_error));
        WT_ERR(ret);
    }

    for (count = 0;;) {
        /*
         * Skip . and ..
         */
        if (wcscmp(finddata.cFileName, L".") == 0 || wcscmp(finddata.cFileName, L"..") == 0)
            goto skip;

        /* The list of files is optionally filtered by a prefix. */
        if (prefix != NULL && wcsncmp(finddata.cFileName, prefix_wide->data, prefix_widelen) != 0)
            goto skip;

        WT_ERR(__wt_realloc_def(session, &dirallocsz, count + 1, &entries));
        WT_ERR(__wt_to_utf8_string(session, finddata.cFileName, &file_utf8));
        WT_ERR(__wt_strdup(session, file_utf8->data, &entries[count]));
        ++count;
        __wt_scr_free(session, &file_utf8);

        if (single)
            break;

skip:
        if (FindNextFileW(findhandle, &finddata) != 0)
            continue;
        windows_error = __wt_getlasterror();
        if (windows_error == ERROR_NO_MORE_FILES)
            break;
        ret = __wt_map_windows_error(windows_error);
        __wt_err(session, ret, "%s: directory-list: FindNextFileW: %s", pathbuf->data,
          __wt_formatmessage(session, windows_error));
        WT_ERR(ret);
    }

    *dirlistp = entries;
    *countp = count;

err:
    if (findhandle != INVALID_HANDLE_VALUE)
        if (FindClose(findhandle) == 0) {
            windows_error = __wt_getlasterror();
            if (ret == 0)
                ret = __wt_map_windows_error(windows_error);
            __wt_err(session, ret, "%s: directory-list: FindClose: %s", pathbuf->data,
              __wt_formatmessage(session, windows_error));
        }

    __wt_free(session, dir_copy);
    __wt_scr_free(session, &pathbuf);
    __wt_scr_free(session, &file_utf8);
    __wt_scr_free(session, &pathbuf_wide);
    __wt_scr_free(session, &prefix_wide);

    if (ret == 0)
        return (0);

    WT_TRET(__wt_win_directory_list_free(file_system, wt_session, entries, count));

    WT_RET_MSG(
      session, ret, "%s: directory-list, prefix \"%s\"", directory, prefix == NULL ? "" : prefix);
}

/*
 * __wt_win_directory_list --
 *     Get a list of files from a directory, MSVC version.
 */
int
__wt_win_directory_list(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *directory,
  const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, false));
}

/*
 * __wt_win_directory_list_single --
 *     Get a single file from a directory, MSVC version.
 */
int
__wt_win_directory_list_single(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
  const char *directory, const char *prefix, char ***dirlistp, uint32_t *countp)
{
    return (
      __directory_list_worker(file_system, wt_session, directory, prefix, dirlistp, countp, true));
}

/*
 * __wt_win_directory_list_free --
 *     Free memory returned by __wt_win_directory_list, Windows version.
 */
int
__wt_win_directory_list_free(
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
