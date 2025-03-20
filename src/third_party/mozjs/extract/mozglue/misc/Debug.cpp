/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/glue/Debug.h"
#include "mozilla/Fuzzing.h"
#include "mozilla/Sprintf.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef XP_WIN
#  include <io.h>
#  include <windows.h>
#endif

#ifdef ANDROID
#  include <android/log.h>
#endif

#ifndef ANDROID
static void vprintf_stderr_buffered(const char* aFmt, va_list aArgs) {
  // Avoid interleaving by writing to an on-stack buffer and then writing in one
  // go with fputs, as long as the output fits into the buffer.
  char buffer[1024];
  va_list argsCpy;
  va_copy(argsCpy, aArgs);
  int n = VsprintfLiteral(buffer, aFmt, aArgs);
  if (n < int(sizeof(buffer))) {
    fputs(buffer, stderr);
  } else {
    // Message too long for buffer. Just print it, not worrying about
    // interleaving. (We could malloc, but the underlying write() syscall could
    // get interleaved if the output is too big anyway.)
    vfprintf(stderr, aFmt, argsCpy);
  }
  va_end(argsCpy);
  fflush(stderr);
}
#endif

#if defined(XP_WIN)
MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs) {
  if (IsDebuggerPresent()) {
    int lengthNeeded = _vscprintf(aFmt, aArgs);
    if (lengthNeeded) {
      lengthNeeded++;
      auto buf = mozilla::MakeUnique<char[]>(lengthNeeded);
      if (buf) {
        va_list argsCpy;
        va_copy(argsCpy, aArgs);
        vsnprintf(buf.get(), lengthNeeded, aFmt, argsCpy);
        buf[lengthNeeded - 1] = '\0';
        va_end(argsCpy);
        OutputDebugStringA(buf.get());
      }
    }
  }

  vprintf_stderr_buffered(aFmt, aArgs);
}

#elif defined(ANDROID)
MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs) {
  __android_log_vprint(ANDROID_LOG_INFO, "Gecko", aFmt, aArgs);
}
#elif defined(FUZZING_SNAPSHOT)
MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs) {
  if (nyx_puts) {
    auto msgbuf = mozilla::Vsmprintf(aFmt, aArgs);
    nyx_puts(msgbuf.get());
  } else {
    vprintf_stderr_buffered(aFmt, aArgs);
  }
}
#else
MFBT_API void vprintf_stderr(const char* aFmt, va_list aArgs) {
  vprintf_stderr_buffered(aFmt, aArgs);
}
#endif

MFBT_API void printf_stderr(const char* aFmt, ...) {
  va_list args;
  va_start(args, aFmt);
  vprintf_stderr(aFmt, args);
  va_end(args);
}

MFBT_API void fprintf_stderr(FILE* aFile, const char* aFmt, ...) {
  va_list args;
  va_start(args, aFmt);
  if (aFile == stderr) {
    vprintf_stderr(aFmt, args);
  } else {
    vfprintf(aFile, aFmt, args);
  }
  va_end(args);
}

MFBT_API void print_stderr(std::stringstream& aStr) {
#if defined(ANDROID)
  // On Android logcat output is truncated to 1024 chars per line, and
  // we usually use std::stringstream to build up giant multi-line gobs
  // of output. So to avoid the truncation we find the newlines and
  // print the lines individually.
  std::string line;
  while (std::getline(aStr, line)) {
    printf_stderr("%s\n", line.c_str());
  }
#else
  printf_stderr("%s", aStr.str().c_str());
#endif
}

MFBT_API void fprint_stderr(FILE* aFile, std::stringstream& aStr) {
  if (aFile == stderr) {
    print_stderr(aStr);
  } else {
    fprintf_stderr(aFile, "%s", aStr.str().c_str());
  }
}
