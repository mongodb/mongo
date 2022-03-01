/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "util/CompleteFile.h"

#include <cstring>     // std::strcmp
#include <stdio.h>     // FILE, fileno, fopen, getc, getc_unlocked, _getc_nolock
#include <sys/stat.h>  // stat, fstat

#ifdef __wasi__
#  include "js/Vector.h"
#endif  // __wasi__

#include "jsapi.h"  // JS_ReportErrorNumberLatin1

#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_CANT_OPEN

bool js::ReadCompleteFile(JSContext* cx, FILE* fp, FileContents& buffer) {
  /* Get the complete length of the file, if possible. */
  struct stat st;
  int ok = fstat(fileno(fp), &st);
  if (ok != 0) {
    // Use the Latin1 variant here (and below), because the encoding of
    // strerror() is platform-dependent.
    JS_ReportErrorLatin1(cx, "error reading file: %s", strerror(errno));
    errno = 0;
    return false;
  }
  if ((st.st_mode & S_IFDIR) != 0) {
    JS_ReportErrorLatin1(cx, "error reading file: %s", strerror(EISDIR));
    return false;
  }

  if (st.st_size > 0) {
    if (!buffer.reserve(st.st_size)) {
      return false;
    }
  }

  /* Use the fastest available getc. */
  auto fast_getc =
#if defined(HAVE_GETC_UNLOCKED)
      getc_unlocked
#elif defined(HAVE__GETC_NOLOCK)
      _getc_nolock
#else
      getc
#endif
      ;

  // Read in the whole file. Note that we can't assume the data's length
  // is actually st.st_size, because 1) some files lie about their size
  // (/dev/zero and /dev/random), and 2) reading files in text mode on
  // Windows collapses "\r\n" pairs to single \n characters.
  for (;;) {
    int c = fast_getc(fp);
    if (c == EOF) {
      break;
    }
    if (!buffer.append(c)) {
      return false;
    }
  }

  if (ferror(fp)) {
    // getc failed
    JS_ReportErrorLatin1(cx, "error reading file: %s", strerror(errno));
    errno = 0;
    return false;
  }

  return true;
}

#ifdef __wasi__
static bool NormalizeWASIPath(const char* filename,
                              js::Vector<char>* normalized, JSContext* cx) {
  // On WASI, we need to collapse ".." path components for the capabilities
  // that we pass to our unit tests to be reasonable; otherwise we need to
  // grant "tests/script1.js/../lib.js" and "tests/script2.js/../lib.js"
  // separately (because the check appears to be a prefix only).
  for (const char* cur = filename; *cur; ++cur) {
    if (std::strncmp(cur, "/../", 4) == 0) {
      do {
        if (normalized->empty()) {
          JS_ReportErrorASCII(cx, "Path processing error");
          return false;
        }
      } while (normalized->popCopy() != '/');
      cur += 2;
      continue;
    }
    if (!normalized->append(*cur)) {
      return false;
    }
  }
  if (!normalized->append('\0')) {
    return false;
  }
  return true;
}
#endif

/*
 * Open a source file for reading. Supports "-" and nullptr to mean stdin. The
 * return value must be fclosed unless it is stdin.
 */
bool js::AutoFile::open(JSContext* cx, const char* filename) {
  if (!filename || std::strcmp(filename, "-") == 0) {
    fp_ = stdin;
  } else {
#ifdef __wasi__
    js::Vector<char> normalized(cx);
    if (!NormalizeWASIPath(filename, &normalized, cx)) {
      return false;
    }
    fp_ = fopen(normalized.begin(), "r");
#else
    fp_ = fopen(filename, "r");
#endif
    if (!fp_) {
      /*
       * Use Latin1 variant here because the encoding of filename is
       * platform dependent.
       */
      JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_CANT_OPEN,
                                 filename, "No such file or directory");
      return false;
    }
  }
  return true;
}
