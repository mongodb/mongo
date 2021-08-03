/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_to_utf16_string --
 *     Convert UTF-8 encoded string to UTF-16.
 */
int
__wt_to_utf16_string(WT_SESSION_IMPL *session, const char *utf8, WT_ITEM **outbuf)
{
    WT_DECL_RET;
    DWORD windows_error;
    int bufferSize;

    bufferSize = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    windows_error = __wt_getlasterror();

    if (bufferSize == 0 && windows_error != ERROR_INSUFFICIENT_BUFFER) {
        ret = __wt_map_windows_error(windows_error);
        __wt_err(
          session, ret, "MultiByteToWideChar: %s", __wt_formatmessage(session, windows_error));
        return (ret);
    }

    WT_RET(__wt_scr_alloc(session, bufferSize * sizeof(wchar_t), outbuf));
    bufferSize = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, (*outbuf)->mem, bufferSize);

    if (bufferSize == 0) {
        windows_error = __wt_getlasterror();
        __wt_scr_free(session, outbuf);
        ret = __wt_map_windows_error(windows_error);
        __wt_err(
          session, ret, "MultiByteToWideChar: %s", __wt_formatmessage(session, windows_error));
        return (ret);
    }

    (*outbuf)->size = bufferSize;
    return (0);
}

/*
 * __wt_to_utf8_string --
 *     Convert UTF-16 encoded string to UTF-8.
 */
int
__wt_to_utf8_string(WT_SESSION_IMPL *session, const wchar_t *wide, WT_ITEM **outbuf)
{
    WT_DECL_RET;
    DWORD windows_error;
    int bufferSize;

    bufferSize = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    windows_error = __wt_getlasterror();

    if (bufferSize == 0 && windows_error != ERROR_INSUFFICIENT_BUFFER) {
        ret = __wt_map_windows_error(windows_error);
        __wt_err(
          session, ret, "WideCharToMultiByte: %s", __wt_formatmessage(session, windows_error));
        return (ret);
    }

    WT_RET(__wt_scr_alloc(session, bufferSize, outbuf));

    bufferSize = WideCharToMultiByte(CP_UTF8, 0, wide, -1, (*outbuf)->mem, bufferSize, NULL, NULL);
    if (bufferSize == 0) {
        windows_error = __wt_getlasterror();
        __wt_scr_free(session, outbuf);
        ret = __wt_map_windows_error(windows_error);
        __wt_err(
          session, ret, "WideCharToMultiByte: %s", __wt_formatmessage(session, windows_error));
        return (ret);
    }

    (*outbuf)->size = bufferSize;
    return (0);
}
