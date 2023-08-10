#ifndef MLIB_ERROR_PRIVATE_H
#define MLIB_ERROR_PRIVATE_H

#include "./user-check.h"

#include "./str.h"

#ifdef _WIN32
#include "./windows-lean.h"
#else
#include <errno.h>
#endif

/**
 * @brief Obtain a string containing an error message corresponding to an error
 * code from the host platform.
 *
 * @param errn An error code for the system. (e.g. GetLastError() on Windows)
 * @return mstr A new string containing the resulting error. Must be freed with
 * @ref mstr_free().
 */
static inline mstr merror_system_error_string(int errn) {
#ifdef _WIN32
    wchar_t *wbuffer = NULL;
    DWORD slen =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       (DWORD)errn,
                       0,
                       (LPWSTR)&wbuffer,
                       0,
                       NULL);
    if (slen == 0) {
        return mstr_copy_cstr("[Error while getting error string from FormatMessageW()]");
    }
    mstr_narrow_result narrow = mstr_win32_narrow(wbuffer);
    LocalFree(wbuffer);
    assert(narrow.error == 0);
    // Messages from FormatMessage contain an additional CR+LF
    if (mstr_ends_with(narrow.string.view, mstrv_lit("\r\n"))) {
        mstr_inplace_remove_suffix(&narrow.string, 2);
    }
    return narrow.string;
#else
    errno = 0;
    char *const str = strerror(errn);
    if (errno) {
        return mstr_copy_cstr("[Error while getting error string from strerror()]");
    }
    return mstr_copy_cstr(str);
#endif
}

#endif // MLIB_ERROR_PRIVATE_H