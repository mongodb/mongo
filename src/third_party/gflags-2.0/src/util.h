// Copyright (c) 2011, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ---
//
// Some generically useful utility routines that in google-land would
// be their own projects.  We make a shortened version here.

#ifndef GFLAGS_UTIL_H_
#define GFLAGS_UTIL_H_

#include <assert.h>
#include <config.h>
#ifdef HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <stdarg.h>     // for va_*
#ifdef _WIN32
#  include <varargs.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string>
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif   // for mkdir()

_START_GOOGLE_NAMESPACE_

// This is used for unittests for death-testing.  It is defined in gflags.cc.
extern GFLAGS_DLL_DECL void (*gflags_exitfunc)(int);

// Work properly if either strtoll or strtoq is on this system
#ifdef HAVE_STRTOLL
# define strto64  strtoll
# define strtou64  strtoull
#elif HAVE_STRTOQ
# define strto64  strtoq
# define strtou64  strtouq
#else
// Neither strtoll nor strtoq are defined.  I hope strtol works!
# define strto64 strtol
# define strtou64 strtoul
#endif

// If we have inttypes.h, it will have defined PRId32/etc for us.  If
// not, take our best guess.
#ifndef PRId32
# define PRId32 "d"
#endif
#ifndef PRId64
# define PRId64 "lld"
#endif
#ifndef PRIu64
# define PRIu64 "llu"
#endif

typedef signed char int8;
typedef unsigned char uint8;

// -- utility macros ---------------------------------------------------------

template <bool> struct CompileAssert {};
#define COMPILE_ASSERT(expr, msg) \
  typedef CompileAssert<(bool(expr))> msg[bool(expr) ? 1 : -1]

// Returns the number of elements in an array.
#define arraysize(arr) (sizeof(arr)/sizeof(*(arr)))


// -- logging and testing ---------------------------------------------------

// For now, we ignore the level for logging, and don't show *VLOG's at
// all, except by hand-editing the lines below
#define LOG(level)    std::cerr
#define VLOG(level)   if (true) {} else std::cerr
#define DVLOG(level)  if (true) {} else std::cerr

// CHECK dies with a fatal error if condition is not true.  It is *not*
// controlled by NDEBUG, so the check will be executed regardless of
// compilation mode.  Therefore, it is safe to do things like:
//    CHECK(fp->Write(x) == 4)
// We allow stream-like objects after this for debugging, but they're ignored.
#define EXPECT_TRUE(condition)                                  \
  if (true) {                                                   \
    if (!(condition)) {                                         \
      fprintf(stderr, "Check failed: %s\n", #condition);        \
      exit(1);                                                  \
    }                                                           \
  } else std::cerr << ""

#define EXPECT_OP(op, val1, val2)                                       \
  if (true) {                                                           \
    if (!((val1) op (val2))) {                                          \
      fprintf(stderr, "Check failed: %s %s %s\n", #val1, #op, #val2);   \
      exit(1);                                                          \
    }                                                                   \
  } else std::cerr << ""

#define EXPECT_EQ(val1, val2) EXPECT_OP(==, val1, val2)
#define EXPECT_NE(val1, val2) EXPECT_OP(!=, val1, val2)
#define EXPECT_LE(val1, val2) EXPECT_OP(<=, val1, val2)
#define EXPECT_LT(val1, val2) EXPECT_OP(< , val1, val2)
#define EXPECT_GE(val1, val2) EXPECT_OP(>=, val1, val2)
#define EXPECT_GT(val1, val2) EXPECT_OP(> , val1, val2)
#define EXPECT_FALSE(cond)    EXPECT_TRUE(!(cond))

// C99 declares isnan and isinf should be macros, so the #ifdef test
// should be reliable everywhere.  Of course, it's not, but these
// are testing pertty marginal functionality anyway, so it's ok to
// not-run them even in situations they might, with effort, be made to work.
#ifdef isnan  // Some compilers, like sun's for Solaris 10, don't define this
#define EXPECT_NAN(arg)                                         \
  do {                                                          \
    if (!isnan(arg)) {                                          \
      fprintf(stderr, "Check failed: isnan(%s)\n", #arg);       \
      exit(1);                                                  \
    }                                                           \
  } while (0)
#else
#define EXPECT_NAN(arg)
#endif

#ifdef isinf  // Some compilers, like sun's for Solaris 10, don't define this
#define EXPECT_INF(arg)                                         \
  do {                                                          \
    if (!isinf(arg)) {                                          \
      fprintf(stderr, "Check failed: isinf(%s)\n", #arg);       \
      exit(1);                                                  \
    }                                                           \
  } while (0)
#else
#define EXPECT_INF(arg)
#endif

#define EXPECT_DOUBLE_EQ(val1, val2)                                    \
  do {                                                                  \
    if (((val1) < (val2) - 0.001 || (val1) > (val2) + 0.001)) {         \
      fprintf(stderr, "Check failed: %s == %s\n", #val1, #val2);        \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

#define EXPECT_STREQ(val1, val2)                                        \
  do {                                                                  \
    if (strcmp((val1), (val2)) != 0) {                                  \
      fprintf(stderr, "Check failed: streq(%s, %s)\n", #val1, #val2);   \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

// Call this in a .cc file where you will later call RUN_ALL_TESTS in main().
#define TEST_INIT                                                       \
  static std::vector<void (*)()> g_testlist;  /* the tests to run */    \
  static int RUN_ALL_TESTS() {                                          \
    std::vector<void (*)()>::const_iterator it;                         \
    for (it = g_testlist.begin(); it != g_testlist.end(); ++it) {       \
      (*it)();   /* The test will error-exit if there's a problem. */   \
    }                                                                   \
    fprintf(stderr, "\nPassed %d tests\n\nPASS\n",                      \
            static_cast<int>(g_testlist.size()));                       \
    return 0;                                                           \
  }

// Note that this macro uses a FlagSaver to keep tests isolated.
#define TEST(a, b)                                      \
  struct Test_##a##_##b {                               \
    Test_##a##_##b() { g_testlist.push_back(&Run); }    \
    static void Run() {                                 \
      FlagSaver fs;                                     \
      fprintf(stderr, "Running test %s/%s\n", #a, #b);  \
      RunTest();                                        \
    }                                                   \
    static void RunTest();                              \
  };                                                    \
  static Test_##a##_##b g_test_##a##_##b;               \
  void Test_##a##_##b::RunTest()

// This is a dummy class that eases the google->opensource transition.
namespace testing {
class Test {};
}

// Call this in a .cc file where you will later call EXPECT_DEATH
#define EXPECT_DEATH_INIT                               \
  static bool g_called_exit;                            \
  static void CalledExit(int) { g_called_exit = true; }

#define EXPECT_DEATH(fn, msg)                                           \
  do {                                                                  \
    g_called_exit = false;                                              \
    gflags_exitfunc = &CalledExit;                            \
    fn;                                                                 \
    gflags_exitfunc = &exit;    /* set back to its default */ \
    if (!g_called_exit) {                                               \
      fprintf(stderr, "Function didn't die (%s): %s\n", msg, #fn);      \
      exit(1);                                                          \
    }                                                                   \
  } while (0)

#define GTEST_HAS_DEATH_TEST 1

// -- path routines ----------------------------------------------------------

// Tries to create the directory path as a temp-dir.  If it fails,
// changes path to some directory it *can* create.
#if defined(__MINGW32__)
#include <io.h>
inline void MakeTmpdir(std::string* path) {
  // I had trouble creating a directory in /tmp from mingw
  *path = "./gflags_unittest_testdir";
  mkdir(path->c_str());   // mingw has a weird one-arg mkdir
}
#elif defined(_MSC_VER)
#include <direct.h>
#include <windows.h>
inline void MakeTmpdir(std::string* path) {
  char tmppath_buffer[1024];
  int tmppath_len = GetTempPathA(sizeof(tmppath_buffer), tmppath_buffer);
  assert(tmppath_len > 0 && tmppath_len < sizeof(tmppath_buffer));
  assert(tmppath_buffer[tmppath_len - 1] == '\\');   // API guarantees it
  *path = std::string(tmppath_buffer) + "gflags_unittest_testdir";
  _mkdir(path->c_str());
}
#else
inline void MakeTmpdir(std::string* path) {
  mkdir(path->c_str(), 0755);
}
#endif

#ifdef _WIN32
#define va_copy(a,b) (void)((a)=(b))
#endif

// -- string routines --------------------------------------------------------

inline void InternalStringPrintf(std::string* output, const char* format,
                                 va_list ap) {
  char space[128];    // try a small buffer and hope it fits

  // It's possible for methods that use a va_list to invalidate
  // the data in it upon use.  The fix is to make a copy
  // of the structure before using it and use that copy instead.
  va_list backup_ap;
  va_copy(backup_ap, ap);
  int bytes_written = vsnprintf(space, sizeof(space), format, backup_ap);
  va_end(backup_ap);

  if ((bytes_written >= 0) && ((size_t)bytes_written < sizeof(space))) {
    output->append(space, bytes_written);
    return;
  }

  // Repeatedly increase buffer size until it fits.
  int length = sizeof(space);
  while (true) {
    if (bytes_written < 0) {
      // Older snprintf() behavior. :-(  Just try doubling the buffer size
      length *= 2;
    } else {
      // We need exactly "bytes_written+1" characters
      length = bytes_written+1;
    }
    char* buf = new char[length];

    // Restore the va_list before we use it again
    va_copy(backup_ap, ap);
    bytes_written = vsnprintf(buf, length, format, backup_ap);
    va_end(backup_ap);

    if ((bytes_written >= 0) && (bytes_written < length)) {
      output->append(buf, bytes_written);
      delete[] buf;
      return;
    }
    delete[] buf;
  }
}

// Clears output before writing to it.
inline void SStringPrintf(std::string* output, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  output->clear();
  InternalStringPrintf(output, format, ap);
  va_end(ap);
}

inline void StringAppendF(std::string* output, const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  InternalStringPrintf(output, format, ap);
  va_end(ap);
}

inline std::string StringPrintf(const char* format, ...) {
  va_list ap;
  va_start(ap, format);
  std::string output;
  InternalStringPrintf(&output, format, ap);
  va_end(ap);
  return output;
}

_END_GOOGLE_NAMESPACE_

#endif  // GFLAGS_UTIL_H_
