/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTD_FILEIO_COMMON_H
#define ZSTD_FILEIO_COMMON_H

#if defined (__cplusplus)
extern "C" {
#endif

#include "../lib/common/mem.h"     /* U32, U64 */
#include "fileio_types.h"
#include "platform.h"
#include "timefn.h"     /* UTIL_getTime, UTIL_clockSpanMicro */

/*-*************************************
*  Macros
***************************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)
#undef MAX
#define MAX(a,b) ((a)>(b) ? (a) : (b))

extern FIO_display_prefs_t g_display_prefs;

#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYOUT(...)      fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) { if (g_display_prefs.displayLevel>=l) { DISPLAY(__VA_ARGS__); } }

extern UTIL_time_t g_displayClock;

#define REFRESH_RATE  ((U64)(SEC_TO_MICRO / 6))
#define READY_FOR_UPDATE() (UTIL_clockSpanMicro(g_displayClock) > REFRESH_RATE || g_display_prefs.displayLevel >= 4)
#define DELAY_NEXT_UPDATE() { g_displayClock = UTIL_getTime(); }
#define DISPLAYUPDATE(l, ...) {                              \
        if (g_display_prefs.displayLevel>=l && (g_display_prefs.progressSetting != FIO_ps_never)) { \
            if (READY_FOR_UPDATE()) { \
                DELAY_NEXT_UPDATE();                         \
                DISPLAY(__VA_ARGS__);                        \
                if (g_display_prefs.displayLevel>=4) fflush(stderr);       \
    }   }   }

#define SHOULD_DISPLAY_SUMMARY() \
    (g_display_prefs.displayLevel >= 2 || g_display_prefs.progressSetting == FIO_ps_always)
#define SHOULD_DISPLAY_PROGRESS()                       \
    (g_display_prefs.progressSetting != FIO_ps_never && SHOULD_DISPLAY_SUMMARY())
#define DISPLAY_PROGRESS(...) { if (SHOULD_DISPLAY_PROGRESS()) { DISPLAYLEVEL(1, __VA_ARGS__); }}
#define DISPLAYUPDATE_PROGRESS(...) { if (SHOULD_DISPLAY_PROGRESS()) { DISPLAYUPDATE(1, __VA_ARGS__); }}
#define DISPLAY_SUMMARY(...) { if (SHOULD_DISPLAY_SUMMARY()) { DISPLAYLEVEL(1, __VA_ARGS__); } }

#undef MIN  /* in case it would be already defined */
#define MIN(a,b)    ((a) < (b) ? (a) : (b))


#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DISPLAYLEVEL(1, "zstd: ");                                            \
    DISPLAYLEVEL(5, "Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, " \n");                                               \
    exit(error);                                                          \
}

#define CHECK_V(v, f)                                \
    v = f;                                           \
    if (ZSTD_isError(v)) {                           \
        DISPLAYLEVEL(5, "%s \n", #f);                \
        EXM_THROW(11, "%s", ZSTD_getErrorName(v));   \
    }
#define CHECK(f) { size_t err; CHECK_V(err, f); }


/* Avoid fseek()'s 2GiB barrier with MSVC, macOS, *BSD, MinGW */
#if defined(_MSC_VER) && _MSC_VER >= 1400
#   define LONG_SEEK _fseeki64
#   define LONG_TELL _ftelli64
#elif !defined(__64BIT__) && (PLATFORM_POSIX_VERSION >= 200112L) /* No point defining Large file for 64 bit */
#  define LONG_SEEK fseeko
#  define LONG_TELL ftello
#elif defined(__MINGW32__) && !defined(__STRICT_ANSI__) && !defined(__NO_MINGW_LFS) && defined(__MSVCRT__)
#   define LONG_SEEK fseeko64
#   define LONG_TELL ftello64
#elif defined(_WIN32) && !defined(__DJGPP__)
#   include <windows.h>
    static int LONG_SEEK(FILE* file, __int64 offset, int origin) {
        LARGE_INTEGER off;
        DWORD method;
        off.QuadPart = offset;
        if (origin == SEEK_END)
            method = FILE_END;
        else if (origin == SEEK_CUR)
            method = FILE_CURRENT;
        else
            method = FILE_BEGIN;

        if (SetFilePointerEx((HANDLE) _get_osfhandle(_fileno(file)), off, NULL, method))
            return 0;
        else
            return -1;
    }
    static __int64 LONG_TELL(FILE* file) {
        LARGE_INTEGER off, newOff;
        off.QuadPart = 0;
        newOff.QuadPart = 0;
        SetFilePointerEx((HANDLE) _get_osfhandle(_fileno(file)), off, &newOff, FILE_CURRENT);
        return newOff.QuadPart;
    }
#else
#   define LONG_SEEK fseek
#   define LONG_TELL ftell
#endif

#if defined (__cplusplus)
}
#endif
#endif /* ZSTD_FILEIO_COMMON_H */
