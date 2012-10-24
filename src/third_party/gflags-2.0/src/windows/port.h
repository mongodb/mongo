/* Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ---
 * Author: Craig Silverstein
 *
 * These are some portability typedefs and defines to make it a bit
 * easier to compile this code under VC++.
 *
 * Several of these are taken from glib:
 *    http://developer.gnome.org/doc/API/glib/glib-windows-compatability-functions.html
 */

#ifndef GOOGLE_GFLAGS_WINDOWS_PORT_H_
#define GOOGLE_GFLAGS_WINDOWS_PORT_H_

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  /* We always want minimal includes */
#endif
#include <windows.h>
#include <direct.h>          /* for mkdir */
#include <stdlib.h>          /* for _putenv, getenv */
#include <stdio.h>           /* need this to override stdio's snprintf */
#include <stdarg.h>          /* util.h uses va_copy */
#include <string.h>          /* for _stricmp */

/* We can't just use _vsnprintf and _snprintf as drop-in-replacements,
 * because they don't always NUL-terminate. :-(  We also can't use the
 * name vsnprintf, since windows defines that (but not snprintf (!)).
 */
#if !defined(__MINGW32__) && !defined(__MINGW64__)  /* mingw already defines */
extern GFLAGS_DLL_DECL int snprintf(char *str, size_t size,
                                       const char *format, ...);
extern int GFLAGS_DLL_DECL safe_vsnprintf(char *str, size_t size,
                                             const char *format, va_list ap);
#define vsnprintf(str, size, format, ap)  safe_vsnprintf(str, size, format, ap)
#define va_copy(dst, src)  (dst) = (src)
#endif  /* #if !defined(__MINGW32__) && !defined(__MINGW64__) */

inline void setenv(const char* name, const char* value, int) {
  // In windows, it's impossible to set a variable to the empty string.
  // We handle this by setting it to "0" and the NUL-ing out the \0.
  // That is, we putenv("FOO=0") and then find out where in memory the
  // putenv wrote "FOO=0", and change it in-place to "FOO=\0".
  // c.f. http://svn.apache.org/viewvc/stdcxx/trunk/tests/src/environ.cpp?r1=611451&r2=637508&pathrev=637508
  static const char* const kFakeZero = "0";
  if (*value == '\0')
    value = kFakeZero;
  // Apparently the semantics of putenv() is that the input
  // must live forever, so we leak memory here. :-(
  const int nameval_len = strlen(name) + 1 + strlen(value) + 1;
  char* nameval = reinterpret_cast<char*>(malloc(nameval_len));
  snprintf(nameval, nameval_len, "%s=%s", name, value);
  _putenv(nameval);
  if (value == kFakeZero) {
    nameval[nameval_len - 2] = '\0';   // works when putenv() makes no copy
    if (*getenv(name) != '\0')
      *getenv(name) = '\0';            // works when putenv() copies nameval
  }
}

#define strcasecmp   _stricmp

#define PRId32  "d"
#define PRIu32  "u"
#define PRId64  "I64d"
#define PRIu64  "I64u"

#ifndef __MINGW32__
#define strtoq   _strtoi64
#define strtouq  _strtoui64
#define strtoll  _strtoi64
#define strtoull _strtoui64
#define atoll    _atoi64
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#endif  /* _WIN32 */

#endif  /* GOOGLE_GFLAGS_WINDOWS_PORT_H_ */
