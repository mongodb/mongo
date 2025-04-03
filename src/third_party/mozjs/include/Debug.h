/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_glue_Debug_h
#define mozilla_glue_Debug_h

/* This header file intends to supply debugging utilities for use in code
 * that cannot use XPCOM debugging facilities like nsDebug.h.
 * e.g. mozglue, browser/app
 *
 * NB: printf_stderr() is in the global namespace, so include this file with
 * care; avoid including from header files.
 */

#include <io.h>
#if defined(XP_WIN)
#  include <windows.h>
#endif  // defined(XP_WIN)
#include "mozilla/Attributes.h"
#include "mozilla/Sprintf.h"

#if defined(MOZILLA_INTERNAL_API)
#  error Do not include this file from XUL sources.
#endif

// Though this is a separate implementation than nsDebug's, we want to make the
// declarations compatible to avoid confusing the linker if both headers are
// included.
#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

void printf_stderr(const char* fmt, ...) MOZ_FORMAT_PRINTF(1, 2);
inline void printf_stderr(const char* fmt, ...) {
#if defined(XP_WIN)
  if (IsDebuggerPresent()) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    VsprintfLiteral(buf, fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
  }
#endif  // defined(XP_WIN)

  // stderr is unbuffered by default so we open a new FILE (which is buffered)
  // so that calls to printf_stderr are not as likely to get mixed together.
  int fd = _fileno(stderr);
  if (fd == -2) return;

  FILE* fp = _fdopen(_dup(fd), "a");
  if (!fp) return;

  va_list args;
  va_start(args, fmt);
  vfprintf(fp, fmt, args);
  va_end(args);

  fclose(fp);
}

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // mozilla_glue_Debug_h
