/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_glue_Debug_h
#define mozilla_glue_Debug_h

#include "mozilla/Attributes.h"  // For MOZ_FORMAT_PRINTF
#include "mozilla/Types.h"       // For MFBT_API

#include <cstdarg>
#include <sstream>

/* This header file intends to supply debugging utilities for use in code
 * that cannot use XPCOM debugging facilities like nsDebug.h.
 * e.g. mozglue, browser/app
 *
 * NB: printf_stderr() is in the global namespace, so include this file with
 * care; avoid including from header files.
 */

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

/**
 * printf_stderr(...) is much like fprintf(stderr, ...), except that:
 *  - on Android and Firefox OS, *instead* of printing to stderr, it
 *    prints to logcat.  (Newlines in the string lead to multiple lines
 *    of logcat, but each function call implicitly completes a line even
 *    if the string does not end with a newline.)
 *  - on Windows, if a debugger is present, it calls OutputDebugString
 *    in *addition* to writing to stderr
 */
MFBT_API void printf_stderr(const char* aFmt, ...) MOZ_FORMAT_PRINTF(1, 2);

/**
 * Same as printf_stderr, but taking va_list instead of varargs
 */
MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs)
    MOZ_FORMAT_PRINTF(1, 0);

/**
 * fprintf_stderr is like fprintf, except that if its file argument
 * is stderr, it invokes printf_stderr instead.
 *
 * This is useful for general debugging code that logs information to a
 * file, but that you would like to be useful on Android and Firefox OS.
 * If you use fprintf_stderr instead of fprintf in such debugging code,
 * then callers can pass stderr to get logging that works on Android and
 * Firefox OS (and also the other side-effects of using printf_stderr).
 *
 * Code that is structured this way needs to be careful not to split a
 * line of output across multiple calls to fprintf_stderr, since doing
 * so will cause it to appear in multiple lines in logcat output.
 * (Producing multiple lines at once is fine.)
 */
MFBT_API void fprintf_stderr(FILE* aFile, const char* aFmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);

/*
 * print_stderr and fprint_stderr are like printf_stderr and fprintf_stderr,
 * except they deal with Android logcat line length limitations. They do this
 * by printing individual lines out of the provided stringstream using separate
 * calls to logcat.
 */
MFBT_API void print_stderr(std::stringstream& aStr);
MFBT_API void fprint_stderr(FILE* aFile, std::stringstream& aStr);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // mozilla_glue_Debug_h
