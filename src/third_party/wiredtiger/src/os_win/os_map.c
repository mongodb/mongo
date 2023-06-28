/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_win_map --
 *     Map a file into memory.
 */
int
__wt_win_map(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, void **mapped_regionp,
  size_t *lenp, void **mapped_cookiep)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_WIN *win_fh;
    WT_SESSION_IMPL *session;
    wt_off_t file_size;
    DWORD windows_error;
    size_t len;
    void *map, *mapped_cookie;

    win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
    session = (WT_SESSION_IMPL *)wt_session;

    /*
     * There's no locking here to prevent the underlying file from changing underneath us, our
     * caller needs to ensure consistency of the mapped region vs. any other file activity.
     */
    WT_RET(__wt_win_fs_size(file_handle->file_system, wt_session, file_handle->name, &file_size));
    len = (size_t)file_size;

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: memory-map: %" WT_SIZET_FMT " bytes",
      file_handle->name, len);

    mapped_cookie = CreateFileMappingW(win_fh->filehandle, NULL, PAGE_READONLY, 0, 0, NULL);
    if (mapped_cookie == NULL) {
        windows_error = __wt_getlasterror();
        ret = __wt_map_windows_error(windows_error);
        __wt_err(session, ret, "%s: memory-map: CreateFileMappingW: %s", file_handle->name,
          __wt_formatmessage(session, windows_error));
        return (ret);
    }

    if ((map = MapViewOfFile(mapped_cookie, FILE_MAP_READ, 0, 0, len)) == NULL) {
        /* Retrieve the error before cleaning up. */
        windows_error = __wt_getlasterror();
        ret = __wt_map_windows_error(windows_error);

        (void)CloseHandle(mapped_cookie);

        __wt_err(session, ret, "%s: memory-map: MapViewOfFile: %s", file_handle->name,
          __wt_formatmessage(session, windows_error));
        return (ret);
    }

    *mapped_cookiep = mapped_cookie;
    *mapped_regionp = map;
    *lenp = len;
    return (0);
}

/*
 * __wt_win_unmap --
 *     Remove a memory mapping.
 */
int
__wt_win_unmap(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, void *mapped_region,
  size_t length, void *mapped_cookie)
{
    WT_DECL_RET;
    WT_FILE_HANDLE_WIN *win_fh;
    WT_SESSION_IMPL *session;
    DWORD windows_error;

    win_fh = (WT_FILE_HANDLE_WIN *)file_handle;
    session = (WT_SESSION_IMPL *)wt_session;

    __wt_verbose(session, WT_VERB_HANDLEOPS, "%s: memory-unmap: %" WT_SIZET_FMT " bytes",
      file_handle->name, length);

    if (UnmapViewOfFile(mapped_region) == 0) {
        windows_error = __wt_getlasterror();
        ret = __wt_map_windows_error(windows_error);
        __wt_err(session, ret, "%s: memory-unmap: UnmapViewOfFile: %s", file_handle->name,
          __wt_formatmessage(session, windows_error));
    }

    if (CloseHandle(*(void **)mapped_cookie) == 0) {
        windows_error = __wt_getlasterror();
        ret = __wt_map_windows_error(windows_error);
        __wt_err(session, ret, "%s: memory-unmap: CloseHandle: %s", file_handle->name,
          __wt_formatmessage(session, windows_error));
    }

    return (ret);
}
