// Copyright (c) 2005, Google Inc.
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
// For now, this unit test does not cover all features of
// gflags.cc

#include "config_for_unittests.h"
#include <gflags/gflags.h>

#include <math.h>       // for isinf() and isnan()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif     // for unlink()
#include <vector>
#include <string>
#include "util.h"
TEST_INIT
EXPECT_DEATH_INIT

// I don't actually use this header file, but #include it under the
// old location to make sure that the include-header-forwarding
// works.  But don't bother on windows; the windows port is so new
// it never had the old location-names.
#ifndef _MSC_VER
#include <google/gflags_completions.h>
void (*unused_fn)() = &GOOGLE_NAMESPACE::HandleCommandLineCompletions;
#endif

using std::string;
using std::vector;
using GOOGLE_NAMESPACE::int32;
using GOOGLE_NAMESPACE::FlagRegisterer;
using GOOGLE_NAMESPACE::StringFromEnv;
using GOOGLE_NAMESPACE::RegisterFlagValidator;
using GOOGLE_NAMESPACE::CommandLineFlagInfo;
using GOOGLE_NAMESPACE::GetAllFlags;

DEFINE_string(test_tmpdir, "/tmp/gflags_unittest", "Dir we use for temp files");
#ifdef _MSC_VER  // in MSVC, we run from the vsprojects directory
DEFINE_string(srcdir, "..\\..",
              "Source-dir root, needed to find gflags_unittest_flagfile");
#else
DEFINE_string(srcdir, StringFromEnv("SRCDIR", "."),
              "Source-dir root, needed to find gflags_unittest_flagfile");
#endif

DECLARE_string(tryfromenv);   // in gflags.cc

DEFINE_bool(test_bool, false, "tests bool-ness");
DEFINE_int32(test_int32, -1, "");
DEFINE_int64(test_int64, -2, "");
DEFINE_uint64(test_uint64, 2, "");
DEFINE_double(test_double, -1.0, "");
DEFINE_string(test_string, "initial", "");

//
// The below ugliness gets some additional code coverage in the -helpxml
// and -helpmatch test cases having to do with string lengths and formatting
//
DEFINE_bool(test_bool_with_quite_quite_quite_quite_quite_quite_quite_quite_quite_quite_quite_quite_quite_quite_long_name,
            false,
            "extremely_extremely_extremely_extremely_extremely_extremely_extremely_extremely_long_meaning");

DEFINE_string(test_str1, "initial", "");
DEFINE_string(test_str2, "initial", "");
DEFINE_string(test_str3, "initial", "");

// This is used to test setting tryfromenv manually
DEFINE_string(test_tryfromenv, "initial", "");

// Don't try this at home!
static int changeable_var = 12;
DEFINE_int32(changeable_var, ++changeable_var, "");

static int changeable_bool_var = 8008;
DEFINE_bool(changeable_bool_var, ++changeable_bool_var == 8009, "");

static int changeable_string_var = 0;
static string ChangeableString() {
  char r[] = {static_cast<char>('0' + ++changeable_string_var), '\0'};
  return r;
}
DEFINE_string(changeable_string_var, ChangeableString(), "");

// These are never used in this unittest, but can be used by
// gflags_unittest.sh when it needs to specify flags
// that are legal for gflags_unittest but don't need to
// be a particular value.
DEFINE_bool(unused_bool, true, "unused bool-ness");
DEFINE_int32(unused_int32, -1001, "");
DEFINE_int64(unused_int64, -2001, "");
DEFINE_uint64(unused_uint64, 2000, "");
DEFINE_double(unused_double, -1000.0, "");
DEFINE_string(unused_string, "unused", "");

// These flags are used by gflags_unittest.sh
DEFINE_bool(changed_bool1, false, "changed");
DEFINE_bool(changed_bool2, false, "changed");
DEFINE_bool(long_helpstring, false,
            "This helpstring goes on forever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever and ever and ever and ever and ever and ever and ever and "
            "ever.  This is the end of a long helpstring");


static bool AlwaysFail(const char* flag, bool value) { return value == false; }
DEFINE_bool(always_fail, false, "will fail to validate when you set it");
namespace {
bool dummy = RegisterFlagValidator(&FLAGS_always_fail, AlwaysFail);
}

// See the comment by GetAllFlags in gflags.h
static bool DeadlockIfCantLockInValidators(const char* flag, bool value) {
  if (!value) {
    return true;
  }
  vector<CommandLineFlagInfo> dummy;
  GetAllFlags(&dummy);
  return true;
}
DEFINE_bool(deadlock_if_cant_lock,
            false,
            "will deadlock if set to true and "
            "if locking of registry in validators fails.");
namespace {
bool dummy1 = RegisterFlagValidator(&FLAGS_deadlock_if_cant_lock,
                                    DeadlockIfCantLockInValidators);
}

#define MAKEFLAG(x) DEFINE_int32(test_flag_num##x, x, "Test flag")

// Define 10 flags
#define MAKEFLAG10(x)                           \
  MAKEFLAG(x##0);                               \
  MAKEFLAG(x##1);                               \
  MAKEFLAG(x##2);                               \
  MAKEFLAG(x##3);                               \
  MAKEFLAG(x##4);                               \
  MAKEFLAG(x##5);                               \
  MAKEFLAG(x##6);                               \
  MAKEFLAG(x##7);                               \
  MAKEFLAG(x##8);                               \
  MAKEFLAG(x##9)

// Define 100 flags
#define MAKEFLAG100(x)                          \
  MAKEFLAG10(x##0);                             \
  MAKEFLAG10(x##1);                             \
  MAKEFLAG10(x##2);                             \
  MAKEFLAG10(x##3);                             \
  MAKEFLAG10(x##4);                             \
  MAKEFLAG10(x##5);                             \
  MAKEFLAG10(x##6);                             \
  MAKEFLAG10(x##7);                             \
  MAKEFLAG10(x##8);                             \
  MAKEFLAG10(x##9)

// Define a bunch of command-line flags.  Each occurrence of the MAKEFLAG100
// macro defines 100 integer flags.  This lets us test the effect of having
// many flags on startup time.
MAKEFLAG100(1);
MAKEFLAG100(2);
MAKEFLAG100(3);
MAKEFLAG100(4);
MAKEFLAG100(5);
MAKEFLAG100(6);
MAKEFLAG100(7);
MAKEFLAG100(8);
MAKEFLAG100(9);
MAKEFLAG100(10);
MAKEFLAG100(11);
MAKEFLAG100(12);
MAKEFLAG100(13);
MAKEFLAG100(14);
MAKEFLAG100(15);

#undef MAKEFLAG100
#undef MAKEFLAG10
#undef MAKEFLAG

// This is a pseudo-flag -- we want to register a flag with a filename
// at the top level, but there is no way to do this except by faking
// the filename.
namespace fLI {
  static const int32 FLAGS_nonotldflag1 = 12;
  int32 FLAGS_tldflag1 = FLAGS_nonotldflag1;
  int32 FLAGS_notldflag1 = FLAGS_nonotldflag1;
  static FlagRegisterer o_tldflag1(
    "tldflag1", "int32",
    "should show up in --helpshort", "gflags_unittest.cc",
    &FLAGS_tldflag1, &FLAGS_notldflag1);
}
using fLI::FLAGS_tldflag1;

namespace fLI {
  static const int32 FLAGS_nonotldflag2 = 23;
  int32 FLAGS_tldflag2 = FLAGS_nonotldflag2;
  int32 FLAGS_notldflag2 = FLAGS_nonotldflag2;
  static FlagRegisterer o_tldflag2(
    "tldflag2", "int32",
    "should show up in --helpshort", "gflags_unittest.",
    &FLAGS_tldflag2, &FLAGS_notldflag2);
}
using fLI::FLAGS_tldflag2;

_START_GOOGLE_NAMESPACE_

namespace {


static string TmpFile(const string& basename) {
#ifdef _MSC_VER
  return FLAGS_test_tmpdir + "\\" + basename;
#else
  return FLAGS_test_tmpdir + "/" + basename;
#endif
}

// Returns the definition of the --flagfile flag to be used in the tests.
// Must be called after ParseCommandLineFlags().
static const char* GetFlagFileFlag() {
#ifdef _MSC_VER
  static const string flagfile = FLAGS_srcdir + "\\src\\gflags_unittest_flagfile";
#else
  static const string flagfile = FLAGS_srcdir + "/src/gflags_unittest_flagfile";
#endif
  static const string flagfile_flag = string("--flagfile=") + flagfile;
  return flagfile_flag.c_str();
}


// Defining a variable of type CompileAssertTypesEqual<T1, T2> will cause a
// compiler error iff T1 and T2 are different types.
template <typename T1, typename T2>
struct CompileAssertTypesEqual;

template <typename T>
struct CompileAssertTypesEqual<T, T> {
};


template <typename Expected, typename Actual>
void AssertIsType(Actual& x) {
  CompileAssertTypesEqual<Expected, Actual>();
}

// Verify all the flags are the right type.
TEST(FlagTypes, FlagTypes) {
  AssertIsType<bool>(FLAGS_test_bool);
  AssertIsType<int32>(FLAGS_test_int32);
  AssertIsType<int64>(FLAGS_test_int64);
  AssertIsType<uint64>(FLAGS_test_uint64);
  AssertIsType<double>(FLAGS_test_double);
  AssertIsType<string>(FLAGS_test_string);
}

#ifdef GTEST_HAS_DEATH_TEST
// Death tests for "help" options.
//
// The help system automatically calls gflags_exitfunc(1) when you specify any of
// the help-related flags ("-helpmatch", "-helpxml") so we can't test
// those mainline.

// Tests that "-helpmatch" causes the process to die.
TEST(ReadFlagsFromStringDeathTest, HelpMatch) {
  EXPECT_DEATH(ReadFlagsFromString("-helpmatch=base", GetArgv0(), true),
               "");
}


// Tests that "-helpxml" causes the process to die.
TEST(ReadFlagsFromStringDeathTest, HelpXml) {
  EXPECT_DEATH(ReadFlagsFromString("-helpxml", GetArgv0(), true),
               "");
}
#endif


// A subroutine needed for testing reading flags from a string.
void TestFlagString(const string& flags,
                    const string& expected_string,
                    bool expected_bool,
                    int32 expected_int32,
                    double expected_double) {
  EXPECT_TRUE(ReadFlagsFromString(flags,
                                  GetArgv0(),
                                  // errors are fatal
                                  true));

  EXPECT_EQ(expected_string, FLAGS_test_string);
  EXPECT_EQ(expected_bool, FLAGS_test_bool);
  EXPECT_EQ(expected_int32, FLAGS_test_int32);
  EXPECT_DOUBLE_EQ(expected_double, FLAGS_test_double);
}


// Tests reading flags from a string.
TEST(FlagFileTest, ReadFlagsFromString) {
  TestFlagString(
      // Flag string
      "-test_string=continued\n"
      "# some comments are in order\n"
      "# some\n"
      "  # comments\n"
      "#are\n"
      "                  #trickier\n"
      "# than others\n"
      "-test_bool=true\n"
      "     -test_int32=1\n"
      "-test_double=0.0\n",
      // Expected values
      "continued",
      true,
      1,
      0.0);

  TestFlagString(
      // Flag string
      "# let's make sure it can update values\n"
      "-test_string=initial\n"
      "-test_bool=false\n"
      "-test_int32=123\n"
      "-test_double=123.0\n",
      // Expected values
      "initial",
      false,
      123,
      123.0);
}

// Tests the filename part of the flagfile
TEST(FlagFileTest, FilenamesOurfileLast) {
  FLAGS_test_string = "initial";
  FLAGS_test_bool = false;
  FLAGS_test_int32 = -1;
  FLAGS_test_double = -1.0;
  TestFlagString(
      // Flag string
      "-test_string=continued\n"
      "# some comments are in order\n"
      "# some\n"
      "  # comments\n"
      "#are\n"
      "                  #trickier\n"
      "# than others\n"
      "not_our_filename\n"
      "-test_bool=true\n"
      "     -test_int32=1\n"
      "gflags_unittest\n"
      "-test_double=1000.0\n",
      // Expected values
      "continued",
      false,
      -1,
      1000.0);
}

TEST(FlagFileTest, FilenamesOurfileFirst) {
  FLAGS_test_string = "initial";
  FLAGS_test_bool = false;
  FLAGS_test_int32 = -1;
  FLAGS_test_double = -1.0;
  TestFlagString(
      // Flag string
      "-test_string=continued\n"
      "# some comments are in order\n"
      "# some\n"
      "  # comments\n"
      "#are\n"
      "                  #trickier\n"
      "# than others\n"
      "gflags_unittest\n"
      "-test_bool=true\n"
      "     -test_int32=1\n"
      "not_our_filename\n"
      "-test_double=1000.0\n",
      // Expected values
      "continued",
      true,
      1,
      -1.0);
}

#ifdef HAVE_FNMATCH_H  // otherwise glob isn't supported
TEST(FlagFileTest, FilenamesOurfileGlob) {
  FLAGS_test_string = "initial";
  FLAGS_test_bool = false;
  FLAGS_test_int32 = -1;
  FLAGS_test_double = -1.0;
  TestFlagString(
      // Flag string
      "-test_string=continued\n"
      "# some comments are in order\n"
      "# some\n"
      "  # comments\n"
      "#are\n"
      "                  #trickier\n"
      "# than others\n"
      "*flags*\n"
      "-test_bool=true\n"
      "     -test_int32=1\n"
      "flags\n"
      "-test_double=1000.0\n",
      // Expected values
      "continued",
      true,
      1,
      -1.0);
}

TEST(FlagFileTest, FilenamesOurfileInBigList) {
  FLAGS_test_string = "initial";
  FLAGS_test_bool = false;
  FLAGS_test_int32 = -1;
  FLAGS_test_double = -1.0;
  TestFlagString(
      // Flag string
      "-test_string=continued\n"
      "# some comments are in order\n"
      "# some\n"
      "  # comments\n"
      "#are\n"
      "                  #trickier\n"
      "# than others\n"
      "*first* *flags* *third*\n"
      "-test_bool=true\n"
      "     -test_int32=1\n"
      "flags\n"
      "-test_double=1000.0\n",
      // Expected values
      "continued",
      true,
      1,
      -1.0);
}
#endif  // ifdef HAVE_FNMATCH_H

// Tests that a failed flag-from-string read keeps flags at default values
TEST(FlagFileTest, FailReadFlagsFromString) {
  FLAGS_test_int32 = 119;
  string flags("# let's make sure it can update values\n"
               "-test_string=non_initial\n"
               "-test_bool=false\n"
               "-test_int32=123\n"
               "-test_double=illegal\n");

  EXPECT_FALSE(ReadFlagsFromString(flags,
                                   GetArgv0(),
                                   // errors are fatal
                                   false));

  EXPECT_EQ(119, FLAGS_test_int32);
  EXPECT_EQ("initial", FLAGS_test_string);
}

// Tests that flags can be set to ordinary values.
TEST(SetFlagValueTest, OrdinaryValues) {
  EXPECT_EQ("initial", FLAGS_test_str1);

  SetCommandLineOptionWithMode("test_str1", "second", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str1);  // set; was default

  SetCommandLineOptionWithMode("test_str1", "third", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str1);  // already set once

  FLAGS_test_str1 = "initial";
  SetCommandLineOptionWithMode("test_str1", "third", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("initial", FLAGS_test_str1);  // still already set before

  SetCommandLineOptionWithMode("test_str1", "third", SET_FLAGS_VALUE);
  EXPECT_EQ("third", FLAGS_test_str1);  // changed value

  SetCommandLineOptionWithMode("test_str1", "fourth", SET_FLAGS_DEFAULT);
  EXPECT_EQ("third", FLAGS_test_str1);
  // value not changed (already set before)

  EXPECT_EQ("initial", FLAGS_test_str2);

  SetCommandLineOptionWithMode("test_str2", "second", SET_FLAGS_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str2);  // changed (was default)

  FLAGS_test_str2 = "extra";
  EXPECT_EQ("extra", FLAGS_test_str2);

  FLAGS_test_str2 = "second";
  SetCommandLineOptionWithMode("test_str2", "third", SET_FLAGS_DEFAULT);
  EXPECT_EQ("third", FLAGS_test_str2);  // still changed (was equal to default)

  SetCommandLineOptionWithMode("test_str2", "fourth", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("fourth", FLAGS_test_str2);  // changed (was default)

  EXPECT_EQ("initial", FLAGS_test_str3);

  SetCommandLineOptionWithMode("test_str3", "second", SET_FLAGS_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str3);  // changed

  FLAGS_test_str3 = "third";
  SetCommandLineOptionWithMode("test_str3", "fourth", SET_FLAGS_DEFAULT);
  EXPECT_EQ("third", FLAGS_test_str3);  // not changed (was set)

  SetCommandLineOptionWithMode("test_str3", "fourth", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("third", FLAGS_test_str3);  // not changed (was set)

  SetCommandLineOptionWithMode("test_str3", "fourth", SET_FLAGS_VALUE);
  EXPECT_EQ("fourth", FLAGS_test_str3);  // changed value
}


// Tests that flags can be set to exceptional values.
// Note: apparently MINGW doesn't parse inf and nan correctly:
//    http://www.mail-archive.com/bug-gnulib@gnu.org/msg09573.html
// This url says FreeBSD also has a problem, but I didn't see that.
TEST(SetFlagValueTest, ExceptionalValues) {
#if defined(isinf) && !defined(__MINGW32__)
  EXPECT_EQ("test_double set to inf\n",
            SetCommandLineOption("test_double", "inf"));
  EXPECT_INF(FLAGS_test_double);

  EXPECT_EQ("test_double set to inf\n",
            SetCommandLineOption("test_double", "INF"));
  EXPECT_INF(FLAGS_test_double);
#endif

  // set some bad values
  EXPECT_EQ("",
            SetCommandLineOption("test_double", "0.1xxx"));
  EXPECT_EQ("",
            SetCommandLineOption("test_double", " "));
  EXPECT_EQ("",
            SetCommandLineOption("test_double", ""));
#if defined(isinf) && !defined(__MINGW32__)
  EXPECT_EQ("test_double set to -inf\n",
            SetCommandLineOption("test_double", "-inf"));
  EXPECT_INF(FLAGS_test_double);
  EXPECT_GT(0, FLAGS_test_double);
#endif

#if defined(isnan) && !defined(__MINGW32__)
  EXPECT_EQ("test_double set to nan\n",
            SetCommandLineOption("test_double", "NaN"));
  EXPECT_NAN(FLAGS_test_double);
#endif
}

// Tests that integer flags can be specified in many ways
TEST(SetFlagValueTest, DifferentRadices) {
  EXPECT_EQ("test_int32 set to 12\n",
            SetCommandLineOption("test_int32", "12"));

  EXPECT_EQ("test_int32 set to 16\n",
            SetCommandLineOption("test_int32", "0x10"));

  EXPECT_EQ("test_int32 set to 34\n",
            SetCommandLineOption("test_int32", "0X22"));

  // Leading 0 is *not* octal; it's still decimal
  EXPECT_EQ("test_int32 set to 10\n",
            SetCommandLineOption("test_int32", "010"));
}

// Tests what happens when you try to set a flag to an illegal value
TEST(SetFlagValueTest, IllegalValues) {
  FLAGS_test_bool = true;
  FLAGS_test_int32 = 119;
  FLAGS_test_int64 = 1191;
  FLAGS_test_uint64 = 11911;

  EXPECT_EQ("",
            SetCommandLineOption("test_bool", "12"));

  EXPECT_EQ("",
            SetCommandLineOption("test_int32", "7000000000000"));

  // TODO(csilvers): uncomment this when we disallow negative numbers for uint64
#if 0
  EXPECT_EQ("",
            SetCommandLineOption("test_uint64", "-1"));
#endif

  EXPECT_EQ("",
            SetCommandLineOption("test_int64", "not a number!"));

  // Test the empty string with each type of input
  EXPECT_EQ("", SetCommandLineOption("test_bool", ""));
  EXPECT_EQ("", SetCommandLineOption("test_int32", ""));
  EXPECT_EQ("", SetCommandLineOption("test_int64", ""));
  EXPECT_EQ("", SetCommandLineOption("test_uint64", ""));
  EXPECT_EQ("", SetCommandLineOption("test_double", ""));
  EXPECT_EQ("test_string set to \n", SetCommandLineOption("test_string", ""));

  EXPECT_TRUE(FLAGS_test_bool);
  EXPECT_EQ(119, FLAGS_test_int32);
  EXPECT_EQ(1191, FLAGS_test_int64);
  EXPECT_EQ(11911, FLAGS_test_uint64);
}


// Tests that we only evaluate macro args once
TEST(MacroArgs, EvaluateOnce) {
  EXPECT_EQ(13, FLAGS_changeable_var);
  // Make sure we don't ++ the value somehow, when evaluating the flag.
  EXPECT_EQ(13, FLAGS_changeable_var);
  // Make sure the macro only evaluated this var once.
  EXPECT_EQ(13, changeable_var);
  // Make sure the actual value and default value are the same
  SetCommandLineOptionWithMode("changeable_var", "21", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ(21, FLAGS_changeable_var);
}

TEST(MacroArgs, EvaluateOnceBool) {
  EXPECT_TRUE(FLAGS_changeable_bool_var);
  EXPECT_TRUE(FLAGS_changeable_bool_var);
  EXPECT_EQ(8009, changeable_bool_var);
  SetCommandLineOptionWithMode("changeable_bool_var", "false",
                               SET_FLAG_IF_DEFAULT);
  EXPECT_FALSE(FLAGS_changeable_bool_var);
}

TEST(MacroArgs, EvaluateOnceStrings) {
  EXPECT_EQ("1", FLAGS_changeable_string_var);
  EXPECT_EQ("1", FLAGS_changeable_string_var);
  EXPECT_EQ(1, changeable_string_var);
  SetCommandLineOptionWithMode("changeable_string_var", "different",
                               SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("different", FLAGS_changeable_string_var);
}

// Tests that the FooFromEnv does the right thing
TEST(FromEnvTest, LegalValues) {
  setenv("BOOL_VAL1", "true", 1);
  setenv("BOOL_VAL2", "false", 1);
  setenv("BOOL_VAL3", "1", 1);
  setenv("BOOL_VAL4", "F", 1);
  EXPECT_TRUE(BoolFromEnv("BOOL_VAL1", false));
  EXPECT_FALSE(BoolFromEnv("BOOL_VAL2", true));
  EXPECT_TRUE(BoolFromEnv("BOOL_VAL3", false));
  EXPECT_FALSE(BoolFromEnv("BOOL_VAL4", true));
  EXPECT_TRUE(BoolFromEnv("BOOL_VAL_UNKNOWN", true));
  EXPECT_FALSE(BoolFromEnv("BOOL_VAL_UNKNOWN", false));

  setenv("INT_VAL1", "1", 1);
  setenv("INT_VAL2", "-1", 1);
  EXPECT_EQ(1, Int32FromEnv("INT_VAL1", 10));
  EXPECT_EQ(-1, Int32FromEnv("INT_VAL2", 10));
  EXPECT_EQ(10, Int32FromEnv("INT_VAL_UNKNOWN", 10));

  setenv("INT_VAL3", "1099511627776", 1);
  EXPECT_EQ(1, Int64FromEnv("INT_VAL1", 20));
  EXPECT_EQ(-1, Int64FromEnv("INT_VAL2", 20));
  EXPECT_EQ(1099511627776LL, Int64FromEnv("INT_VAL3", 20));
  EXPECT_EQ(20, Int64FromEnv("INT_VAL_UNKNOWN", 20));

  EXPECT_EQ(1, Uint64FromEnv("INT_VAL1", 30));
  EXPECT_EQ(1099511627776ULL, Uint64FromEnv("INT_VAL3", 30));
  EXPECT_EQ(30, Uint64FromEnv("INT_VAL_UNKNOWN", 30));

  // I pick values here that can be easily represented exactly in floating-point
  setenv("DOUBLE_VAL1", "0.0", 1);
  setenv("DOUBLE_VAL2", "1.0", 1);
  setenv("DOUBLE_VAL3", "-1.0", 1);
  EXPECT_EQ(0.0, DoubleFromEnv("DOUBLE_VAL1", 40.0));
  EXPECT_EQ(1.0, DoubleFromEnv("DOUBLE_VAL2", 40.0));
  EXPECT_EQ(-1.0, DoubleFromEnv("DOUBLE_VAL3", 40.0));
  EXPECT_EQ(40.0, DoubleFromEnv("DOUBLE_VAL_UNKNOWN", 40.0));

  setenv("STRING_VAL1", "", 1);
  setenv("STRING_VAL2", "my happy string!", 1);
  EXPECT_STREQ("", StringFromEnv("STRING_VAL1", "unknown"));
  EXPECT_STREQ("my happy string!", StringFromEnv("STRING_VAL2", "unknown"));
  EXPECT_STREQ("unknown", StringFromEnv("STRING_VAL_UNKNOWN", "unknown"));
}

#ifdef GTEST_HAS_DEATH_TEST
// Tests that the FooFromEnv dies on parse-error
TEST(FromEnvDeathTest, IllegalValues) {
  setenv("BOOL_BAD1", "so true!", 1);
  setenv("BOOL_BAD2", "", 1);
  EXPECT_DEATH(BoolFromEnv("BOOL_BAD1", false), "error parsing env variable");
  EXPECT_DEATH(BoolFromEnv("BOOL_BAD2", true), "error parsing env variable");

  setenv("INT_BAD1", "one", 1);
  setenv("INT_BAD2", "100000000000000000", 1);
  setenv("INT_BAD3", "0xx10", 1);
  setenv("INT_BAD4", "", 1);
  EXPECT_DEATH(Int32FromEnv("INT_BAD1", 10), "error parsing env variable");
  EXPECT_DEATH(Int32FromEnv("INT_BAD2", 10), "error parsing env variable");
  EXPECT_DEATH(Int32FromEnv("INT_BAD3", 10), "error parsing env variable");
  EXPECT_DEATH(Int32FromEnv("INT_BAD4", 10), "error parsing env variable");

  setenv("BIGINT_BAD1", "18446744073709551616000", 1);
  EXPECT_DEATH(Int64FromEnv("INT_BAD1", 20), "error parsing env variable");
  EXPECT_DEATH(Int64FromEnv("INT_BAD3", 20), "error parsing env variable");
  EXPECT_DEATH(Int64FromEnv("INT_BAD4", 20), "error parsing env variable");
  EXPECT_DEATH(Int64FromEnv("BIGINT_BAD1", 200), "error parsing env variable");

  setenv("BIGINT_BAD2", "-1", 1);
  EXPECT_DEATH(Uint64FromEnv("INT_BAD1", 30), "error parsing env variable");
  EXPECT_DEATH(Uint64FromEnv("INT_BAD3", 30), "error parsing env variable");
  EXPECT_DEATH(Uint64FromEnv("INT_BAD4", 30), "error parsing env variable");
  EXPECT_DEATH(Uint64FromEnv("BIGINT_BAD1", 30), "error parsing env variable");
  // TODO(csilvers): uncomment this when we disallow negative numbers for uint64
#if 0
  EXPECT_DEATH(Uint64FromEnv("BIGINT_BAD2", 30), "error parsing env variable");
#endif

  setenv("DOUBLE_BAD1", "0.0.0", 1);
  setenv("DOUBLE_BAD2", "", 1);
  EXPECT_DEATH(DoubleFromEnv("DOUBLE_BAD1", 40.0), "error parsing env variable");
  EXPECT_DEATH(DoubleFromEnv("DOUBLE_BAD2", 40.0), "error parsing env variable");
}
#endif


// Tests that FlagSaver can save the states of string flags.
TEST(FlagSaverTest, CanSaveStringFlagStates) {
  // 1. Initializes the flags.

  // State of flag test_str1:
  //   default value - "initial"
  //   current value - "initial"
  //   not set       - true

  SetCommandLineOptionWithMode("test_str2", "second", SET_FLAGS_VALUE);
  // State of flag test_str2:
  //   default value - "initial"
  //   current value - "second"
  //   not set       - false

  SetCommandLineOptionWithMode("test_str3", "second", SET_FLAGS_DEFAULT);
  // State of flag test_str3:
  //   default value - "second"
  //   current value - "second"
  //   not set       - true

  // 2. Saves the flag states.

  {
    FlagSaver fs;

    // 3. Modifies the flag states.

    SetCommandLineOptionWithMode("test_str1", "second", SET_FLAGS_VALUE);
    EXPECT_EQ("second", FLAGS_test_str1);
    // State of flag test_str1:
    //   default value - "second"
    //   current value - "second"
    //   not set       - true

    SetCommandLineOptionWithMode("test_str2", "third", SET_FLAGS_DEFAULT);
    EXPECT_EQ("second", FLAGS_test_str2);
    // State of flag test_str2:
    //   default value - "third"
    //   current value - "second"
    //   not set       - false

    SetCommandLineOptionWithMode("test_str3", "third", SET_FLAGS_VALUE);
    EXPECT_EQ("third", FLAGS_test_str3);
    // State of flag test_str1:
    //   default value - "second"
    //   current value - "third"
    //   not set       - false

    // 4. Restores the flag states.
  }

  // 5. Verifies that the states were restored.

  // Verifies that the value of test_str1 was restored.
  EXPECT_EQ("initial", FLAGS_test_str1);
  // Verifies that the "not set" attribute of test_str1 was restored to true.
  SetCommandLineOptionWithMode("test_str1", "second", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str1);

  // Verifies that the value of test_str2 was restored.
  EXPECT_EQ("second", FLAGS_test_str2);
  // Verifies that the "not set" attribute of test_str2 was restored to false.
  SetCommandLineOptionWithMode("test_str2", "fourth", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("second", FLAGS_test_str2);

  // Verifies that the value of test_str3 was restored.
  EXPECT_EQ("second", FLAGS_test_str3);
  // Verifies that the "not set" attribute of test_str3 was restored to true.
  SetCommandLineOptionWithMode("test_str3", "fourth", SET_FLAG_IF_DEFAULT);
  EXPECT_EQ("fourth", FLAGS_test_str3);
}


// Tests that FlagSaver can save the values of various-typed flags.
TEST(FlagSaverTest, CanSaveVariousTypedFlagValues) {
  // Initializes the flags.
  FLAGS_test_bool = false;
  FLAGS_test_int32 = -1;
  FLAGS_test_int64 = -2;
  FLAGS_test_uint64 = 3;
  FLAGS_test_double = 4.0;
  FLAGS_test_string = "good";

  // Saves the flag states.
  {
    FlagSaver fs;

    // Modifies the flags.
    FLAGS_test_bool = true;
    FLAGS_test_int32 = -5;
    FLAGS_test_int64 = -6;
    FLAGS_test_uint64 = 7;
    FLAGS_test_double = 8.0;
    FLAGS_test_string = "bad";

    // Restores the flag states.
  }

  // Verifies the flag values were restored.
  EXPECT_FALSE(FLAGS_test_bool);
  EXPECT_EQ(-1, FLAGS_test_int32);
  EXPECT_EQ(-2, FLAGS_test_int64);
  EXPECT_EQ(3, FLAGS_test_uint64);
  EXPECT_DOUBLE_EQ(4.0, FLAGS_test_double);
  EXPECT_EQ("good", FLAGS_test_string);
}

TEST(GetAllFlagsTest, BaseTest) {
  vector<CommandLineFlagInfo> flags;
  GetAllFlags(&flags);
  bool found_test_bool = false;
  vector<CommandLineFlagInfo>::const_iterator i;
  for (i = flags.begin(); i != flags.end(); ++i) {
    if (i->name == "test_bool") {
      found_test_bool = true;
      EXPECT_EQ(i->type, "bool");
      EXPECT_EQ(i->default_value, "false");
      EXPECT_EQ(i->flag_ptr, &FLAGS_test_bool);
      break;
    }
  }
  EXPECT_TRUE(found_test_bool);
}

TEST(ShowUsageWithFlagsTest, BaseTest) {
  // TODO(csilvers): test this by allowing output other than to stdout.
  // Not urgent since this functionality is tested via
  // gflags_unittest.sh, though only through use of --help.
}

TEST(ShowUsageWithFlagsRestrictTest, BaseTest) {
  // TODO(csilvers): test this by allowing output other than to stdout.
  // Not urgent since this functionality is tested via
  // gflags_unittest.sh, though only through use of --helpmatch.
}

// Note: all these argv-based tests depend on SetArgv being called
// before ParseCommandLineFlags() in main(), below.
TEST(GetArgvsTest, BaseTest) {
  vector<string> argvs = GetArgvs();
  EXPECT_EQ(4, argvs.size());
  EXPECT_EQ("/test/argv/for/gflags_unittest", argvs[0]);
  EXPECT_EQ("argv 2", argvs[1]);
  EXPECT_EQ("3rd argv", argvs[2]);
  EXPECT_EQ("argv #4", argvs[3]);
}

TEST(GetArgvTest, BaseTest) {
  EXPECT_STREQ("/test/argv/for/gflags_unittest "
               "argv 2 3rd argv argv #4", GetArgv());
}

TEST(GetArgv0Test, BaseTest) {
  EXPECT_STREQ("/test/argv/for/gflags_unittest", GetArgv0());
}

TEST(GetArgvSumTest, BaseTest) {
  // This number is just the sum of the ASCII values of all the chars
  // in GetArgv().
  EXPECT_EQ(4904, GetArgvSum());
}

TEST(ProgramInvocationNameTest, BaseTest) {
  EXPECT_STREQ("/test/argv/for/gflags_unittest",
               ProgramInvocationName());
}

TEST(ProgramInvocationShortNameTest, BaseTest) {
  EXPECT_STREQ("gflags_unittest", ProgramInvocationShortName());
}

TEST(ProgramUsageTest, BaseTest) {  // Depends on 1st arg to ParseCommandLineFlags()
  EXPECT_STREQ("/test/argv/for/gflags_unittest: "
               "<useless flag> [...]\nDoes something useless.\n",
               ProgramUsage());
}

TEST(GetCommandLineOptionTest, NameExistsAndIsDefault) {
  string value("will be changed");
  bool r = GetCommandLineOption("test_bool", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("false", value);

  r = GetCommandLineOption("test_int32", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("-1", value);
}

TEST(GetCommandLineOptionTest, NameExistsAndWasAssigned) {
  FLAGS_test_int32 = 400;
  string value("will be changed");
  const bool r = GetCommandLineOption("test_int32", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("400", value);
}

TEST(GetCommandLineOptionTest, NameExistsAndWasSet) {
  SetCommandLineOption("test_int32", "700");
  string value("will be changed");
  const bool r = GetCommandLineOption("test_int32", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("700", value);
}

TEST(GetCommandLineOptionTest, NameExistsAndWasNotSet) {
  // This doesn't set the flag's value, but rather its default value.
  // is_default is still true, but the 'default' value returned has changed!
  SetCommandLineOptionWithMode("test_int32", "800", SET_FLAGS_DEFAULT);
  string value("will be changed");
  const bool r = GetCommandLineOption("test_int32", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("800", value);
  EXPECT_TRUE(GetCommandLineFlagInfoOrDie("test_int32").is_default);
}

TEST(GetCommandLineOptionTest, NameExistsAndWasConditionallySet) {
  SetCommandLineOptionWithMode("test_int32", "900", SET_FLAG_IF_DEFAULT);
  string value("will be changed");
  const bool r = GetCommandLineOption("test_int32", &value);
  EXPECT_TRUE(r);
  EXPECT_EQ("900", value);
}

TEST(GetCommandLineOptionTest, NameDoesNotExist) {
  string value("will not be changed");
  const bool r = GetCommandLineOption("test_int3210", &value);
  EXPECT_FALSE(r);
  EXPECT_EQ("will not be changed", value);
}

TEST(GetCommandLineFlagInfoTest, FlagExists) {
  CommandLineFlagInfo info;
  bool r = GetCommandLineFlagInfo("test_int32", &info);
  EXPECT_TRUE(r);
  EXPECT_EQ("test_int32", info.name);
  EXPECT_EQ("int32", info.type);
  EXPECT_EQ("", info.description);
  EXPECT_EQ("-1", info.current_value);
  EXPECT_EQ("-1", info.default_value);
  EXPECT_TRUE(info.is_default);
  EXPECT_FALSE(info.has_validator_fn);
  EXPECT_EQ(&FLAGS_test_int32, info.flag_ptr);

  FLAGS_test_bool = true;
  r = GetCommandLineFlagInfo("test_bool", &info);
  EXPECT_TRUE(r);
  EXPECT_EQ("test_bool", info.name);
  EXPECT_EQ("bool", info.type);
  EXPECT_EQ("tests bool-ness", info.description);
  EXPECT_EQ("true", info.current_value);
  EXPECT_EQ("false", info.default_value);
  EXPECT_FALSE(info.is_default);
  EXPECT_FALSE(info.has_validator_fn);
  EXPECT_EQ(&FLAGS_test_bool, info.flag_ptr);

  FLAGS_test_bool = false;
  r = GetCommandLineFlagInfo("test_bool", &info);
  EXPECT_TRUE(r);
  EXPECT_EQ("test_bool", info.name);
  EXPECT_EQ("bool", info.type);
  EXPECT_EQ("tests bool-ness", info.description);
  EXPECT_EQ("false", info.current_value);
  EXPECT_EQ("false", info.default_value);
  EXPECT_FALSE(info.is_default);  // value is same, but flag *was* modified
  EXPECT_FALSE(info.has_validator_fn);
  EXPECT_EQ(&FLAGS_test_bool, info.flag_ptr);
}

TEST(GetCommandLineFlagInfoTest, FlagDoesNotExist) {
  CommandLineFlagInfo info;
  // Set to some random values that GetCommandLineFlagInfo should not change
  info.name = "name";
  info.type = "type";
  info.current_value = "curr";
  info.default_value = "def";
  info.filename = "/";
  info.is_default = false;
  info.has_validator_fn = true;
  info.flag_ptr = NULL;
  bool r = GetCommandLineFlagInfo("test_int3210", &info);
  EXPECT_FALSE(r);
  EXPECT_EQ("name", info.name);
  EXPECT_EQ("type", info.type);
  EXPECT_EQ("", info.description);
  EXPECT_EQ("curr", info.current_value);
  EXPECT_EQ("def", info.default_value);
  EXPECT_EQ("/", info.filename);
  EXPECT_FALSE(info.is_default);
  EXPECT_TRUE(info.has_validator_fn);
  EXPECT_EQ(NULL, info.flag_ptr);
}

TEST(GetCommandLineFlagInfoOrDieTest, FlagExistsAndIsDefault) {
  CommandLineFlagInfo info;
  info = GetCommandLineFlagInfoOrDie("test_int32");
  EXPECT_EQ("test_int32", info.name);
  EXPECT_EQ("int32", info.type);
  EXPECT_EQ("", info.description);
  EXPECT_EQ("-1", info.current_value);
  EXPECT_EQ("-1", info.default_value);
  EXPECT_TRUE(info.is_default);
  EXPECT_EQ(&FLAGS_test_int32, info.flag_ptr);
  info = GetCommandLineFlagInfoOrDie("test_bool");
  EXPECT_EQ("test_bool", info.name);
  EXPECT_EQ("bool", info.type);
  EXPECT_EQ("tests bool-ness", info.description);
  EXPECT_EQ("false", info.current_value);
  EXPECT_EQ("false", info.default_value);
  EXPECT_TRUE(info.is_default);
  EXPECT_FALSE(info.has_validator_fn);
  EXPECT_EQ(&FLAGS_test_bool, info.flag_ptr);
}

TEST(GetCommandLineFlagInfoOrDieTest, FlagExistsAndWasAssigned) {
  FLAGS_test_int32 = 400;
  CommandLineFlagInfo info;
  info = GetCommandLineFlagInfoOrDie("test_int32");
  EXPECT_EQ("test_int32", info.name);
  EXPECT_EQ("int32", info.type);
  EXPECT_EQ("", info.description);
  EXPECT_EQ("400", info.current_value);
  EXPECT_EQ("-1", info.default_value);
  EXPECT_FALSE(info.is_default);
  EXPECT_EQ(&FLAGS_test_int32, info.flag_ptr);
  FLAGS_test_bool = true;
  info = GetCommandLineFlagInfoOrDie("test_bool");
  EXPECT_EQ("test_bool", info.name);
  EXPECT_EQ("bool", info.type);
  EXPECT_EQ("tests bool-ness", info.description);
  EXPECT_EQ("true", info.current_value);
  EXPECT_EQ("false", info.default_value);
  EXPECT_FALSE(info.is_default);
  EXPECT_FALSE(info.has_validator_fn);
  EXPECT_EQ(&FLAGS_test_bool, info.flag_ptr);
}

#ifdef GTEST_HAS_DEATH_TEST
TEST(GetCommandLineFlagInfoOrDieDeathTest, FlagDoesNotExist) {
  EXPECT_DEATH(GetCommandLineFlagInfoOrDie("test_int3210"),
               ".*: flag test_int3210 does not exist");
}
#endif


// These are lightly tested because they're deprecated.  Basically,
// the tests are meant to cover how existing users use these functions,
// but not necessarily how new users could use them.
TEST(DeprecatedFunctionsTest, CommandlineFlagsIntoString) {
  string s = CommandlineFlagsIntoString();
  EXPECT_NE(string::npos, s.find("--test_bool="));
}

TEST(DeprecatedFunctionsTest, AppendFlagsIntoFile) {
  FLAGS_test_int32 = 10;     // just to make the test more interesting
  string filename(TmpFile("flagfile"));
  unlink(filename.c_str());  // just to be safe
  const bool r = AppendFlagsIntoFile(filename, "not the real argv0");
  EXPECT_TRUE(r);

  FILE* fp = fopen(filename.c_str(), "r");
  EXPECT_TRUE(fp != NULL);
  char line[8192];
  EXPECT_TRUE(fgets(line, sizeof(line)-1, fp) != NULL);  // get the first line
  // First line should be progname.
  EXPECT_STREQ("not the real argv0\n", line);

  bool found_bool = false, found_int32 = false;
  while (fgets(line, sizeof(line)-1, fp)) {
    line[sizeof(line)-1] = '\0';    // just to be safe
    if (strcmp(line, "--test_bool=false\n") == 0)
      found_bool = true;
    if (strcmp(line, "--test_int32=10\n") == 0)
      found_int32 = true;
  }
  EXPECT_TRUE(found_int32);
  EXPECT_TRUE(found_bool);
  fclose(fp);
}

TEST(DeprecatedFunctionsTest, ReadFromFlagsFile) {
  FLAGS_test_int32 = -10;    // just to make the test more interesting
  string filename(TmpFile("flagfile2"));
  unlink(filename.c_str());  // just to be safe
  bool r = AppendFlagsIntoFile(filename, GetArgv0());
  EXPECT_TRUE(r);

  FLAGS_test_int32 = -11;
  r = ReadFromFlagsFile(filename, GetArgv0(), true);
  EXPECT_TRUE(r);
  EXPECT_EQ(-10, FLAGS_test_int32);
}  // unnamed namespace

TEST(DeprecatedFunctionsTest, ReadFromFlagsFileFailure) {
  FLAGS_test_int32 = -20;
  string filename(TmpFile("flagfile3"));
  FILE* fp = fopen(filename.c_str(), "w");
  EXPECT_TRUE(fp != NULL);
  // Note the error in the bool assignment below...
  fprintf(fp, "%s\n--test_int32=-21\n--test_bool=not_a_bool!\n", GetArgv0());
  fclose(fp);

  FLAGS_test_int32 = -22;
  const bool r = ReadFromFlagsFile(filename, GetArgv0(), false);
  EXPECT_FALSE(r);
  EXPECT_EQ(-22, FLAGS_test_int32);   // the -21 from the flagsfile didn't take
}

TEST(FlagsSetBeforeInitTest, TryFromEnv) {
  EXPECT_EQ("pre-set", FLAGS_test_tryfromenv);
}

// The following test case verifies that ParseCommandLineFlags() and
// ParseCommandLineNonHelpFlags() uses the last definition of a flag
// in case it's defined more than once.

DEFINE_int32(test_flag, -1, "used for testing gflags.cc");

// Parses and returns the --test_flag flag.
// If with_help is true, calls ParseCommandLineFlags; otherwise calls
// ParseCommandLineNonHelpFlags.
int32 ParseTestFlag(bool with_help, int argc, const char** const_argv) {
  FlagSaver fs;  // Restores the flags before returning.

  // Makes a copy of the input array s.t. it can be reused
  // (ParseCommandLineFlags() will alter the array).
  char** const argv_save = new char*[argc + 1];
  char** argv = argv_save;
  memcpy(argv, const_argv, sizeof(*argv)*(argc + 1));

  if (with_help) {
    ParseCommandLineFlags(&argc, &argv, true);
  } else {
    ParseCommandLineNonHelpFlags(&argc, &argv, true);
  }

  delete[] argv_save;
  return FLAGS_test_flag;
}

TEST(ParseCommandLineFlagsUsesLastDefinitionTest,
     WhenFlagIsDefinedTwiceOnCommandLine) {
  const char* argv[] = {
    "my_test",
    "--test_flag=1",
    "--test_flag=2",
    NULL,
  };

  EXPECT_EQ(2, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(2, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsUsesLastDefinitionTest,
     WhenFlagIsDefinedTwiceInFlagFile) {
  const char* argv[] = {
    "my_test",
    GetFlagFileFlag(),
    NULL,
  };

  EXPECT_EQ(2, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(2, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsUsesLastDefinitionTest,
     WhenFlagIsDefinedInCommandLineAndThenFlagFile) {
  const char* argv[] = {
    "my_test",
    "--test_flag=0",
    GetFlagFileFlag(),
    NULL,
  };

  EXPECT_EQ(2, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(2, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsUsesLastDefinitionTest,
     WhenFlagIsDefinedInFlagFileAndThenCommandLine) {
  const char* argv[] = {
    "my_test",
    GetFlagFileFlag(),
    "--test_flag=3",
    NULL,
  };

  EXPECT_EQ(3, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(3, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsUsesLastDefinitionTest,
     WhenFlagIsDefinedInCommandLineAndFlagFileAndThenCommandLine) {
  const char* argv[] = {
    "my_test",
    "--test_flag=0",
    GetFlagFileFlag(),
    "--test_flag=3",
    NULL,
  };

  EXPECT_EQ(3, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(3, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsAndDashArgs, TwoDashArgFirst) {
  const char* argv[] = {
    "my_test",
    "--",
    "--test_flag=0",
    NULL,
  };

  EXPECT_EQ(-1, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(-1, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsAndDashArgs, TwoDashArgMiddle) {
  const char* argv[] = {
    "my_test",
    "--test_flag=7",
    "--",
    "--test_flag=0",
    NULL,
  };

  EXPECT_EQ(7, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(7, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

TEST(ParseCommandLineFlagsAndDashArgs, OneDashArg) {
  const char* argv[] = {
    "my_test",
    "-",
    "--test_flag=0",
    NULL,
  };

  EXPECT_EQ(0, ParseTestFlag(true, arraysize(argv) - 1, argv));
  EXPECT_EQ(0, ParseTestFlag(false, arraysize(argv) - 1, argv));
}

#ifdef GTEST_HAS_DEATH_TEST
TEST(ParseCommandLineFlagsUnknownFlagDeathTest,
     FlagIsCompletelyUnknown) {
  const char* argv[] = {
    "my_test",
    "--this_flag_does_not_exist",
    NULL,
  };

  EXPECT_DEATH(ParseTestFlag(true, arraysize(argv) - 1, argv),
               "unknown command line flag.*");
  EXPECT_DEATH(ParseTestFlag(false, arraysize(argv) - 1, argv),
               "unknown command line flag.*");
}

TEST(ParseCommandLineFlagsUnknownFlagDeathTest,
     BoolFlagIsCompletelyUnknown) {
  const char* argv[] = {
    "my_test",
    "--nothis_flag_does_not_exist",
    NULL,
  };

  EXPECT_DEATH(ParseTestFlag(true, arraysize(argv) - 1, argv),
               "unknown command line flag.*");
  EXPECT_DEATH(ParseTestFlag(false, arraysize(argv) - 1, argv),
               "unknown command line flag.*");
}

TEST(ParseCommandLineFlagsUnknownFlagDeathTest,
     FlagIsNotABool) {
  const char* argv[] = {
    "my_test",
    "--notest_string",
    NULL,
  };

  EXPECT_DEATH(ParseTestFlag(true, arraysize(argv) - 1, argv),
               "boolean value .* specified for .* command line flag");
  EXPECT_DEATH(ParseTestFlag(false, arraysize(argv) - 1, argv),
               "boolean value .* specified for .* command line flag");
}
#endif

TEST(ParseCommandLineFlagsWrongFields,
     DescriptionIsInvalid) {
  // These must not be automatic variables, since command line flags
  // aren't unregistered and gUnit uses FlagSaver to save and restore
  // command line flags' values.  If these are on the stack, then when
  // later tests attempt to save and restore their values, the stack
  // addresses of these variables will be overwritten...  Stack smash!
  static bool current_storage;
  static bool defvalue_storage;
  FlagRegisterer fr("flag_name", "bool", 0, "filename",
                    &current_storage, &defvalue_storage);
  CommandLineFlagInfo fi;
  EXPECT_TRUE(GetCommandLineFlagInfo("flag_name", &fi));
  EXPECT_EQ("", fi.description);
  EXPECT_EQ(&current_storage, fi.flag_ptr);
}

static bool ValidateTestFlagIs5(const char* flagname, int32 flagval) {
  if (flagval == 5)
    return true;
  printf("%s isn't 5!\n", flagname);
  return false;
}

static bool ValidateTestFlagIs10(const char* flagname, int32 flagval) {
  return flagval == 10;
}


TEST(FlagsValidator, ValidFlagViaArgv) {
  const char* argv[] = {
    "my_test",
    "--test_flag=5",
    NULL,
  };
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_EQ(5, ParseTestFlag(true, arraysize(argv) - 1, argv));
  // Undo the flag validator setting
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

TEST(FlagsValidator, ValidFlagViaSetDefault) {
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  // SetCommandLineOptionWithMode returns the empty string on error.
  EXPECT_NE("", SetCommandLineOptionWithMode("test_flag", "5",
                                             SET_FLAG_IF_DEFAULT));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

TEST(FlagsValidator, ValidFlagViaSetValue) {
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  FLAGS_test_flag = 100;   // doesn't trigger the validator
  // SetCommandLineOptionWithMode returns the empty string on error.
  EXPECT_NE("", SetCommandLineOptionWithMode("test_flag", "5",
                                             SET_FLAGS_VALUE));
  EXPECT_NE("", SetCommandLineOptionWithMode("test_flag", "5",
                                             SET_FLAGS_DEFAULT));
  EXPECT_NE("", SetCommandLineOption("test_flag", "5"));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

#ifdef GTEST_HAS_DEATH_TEST
TEST(FlagsValidatorDeathTest, InvalidFlagViaArgv) {
  const char* argv[] = {
    "my_test",
    "--test_flag=50",
    NULL,
  };
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_DEATH(ParseTestFlag(true, arraysize(argv) - 1, argv),
               "ERROR: failed validation of new value '50' for flag 'test_flag'");
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}
#endif

TEST(FlagsValidator, InvalidFlagViaSetDefault) {
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  // SetCommandLineOptionWithMode returns the empty string on error.
  EXPECT_EQ("", SetCommandLineOptionWithMode("test_flag", "50",
                                             SET_FLAG_IF_DEFAULT));
  EXPECT_EQ(-1, FLAGS_test_flag);   // the setting-to-50 should have failed
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

TEST(FlagsValidator, InvalidFlagViaSetValue) {
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  FLAGS_test_flag = 100;   // doesn't trigger the validator
  // SetCommandLineOptionWithMode returns the empty string on error.
  EXPECT_EQ("", SetCommandLineOptionWithMode("test_flag", "50",
                                             SET_FLAGS_VALUE));
  EXPECT_EQ("", SetCommandLineOptionWithMode("test_flag", "50",
                                             SET_FLAGS_DEFAULT));
  EXPECT_EQ("", SetCommandLineOption("test_flag", "50"));
  EXPECT_EQ(100, FLAGS_test_flag);   // the setting-to-50 should have failed
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

#ifdef GTEST_HAS_DEATH_TEST
TEST(FlagsValidatorDeathTest, InvalidFlagNeverSet) {
  // If a flag keeps its default value, and that default value is
  // invalid, we should die at argv-parse time.
  const char* argv[] = {
    "my_test",
    NULL,
  };
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_DEATH(ParseTestFlag(true, arraysize(argv) - 1, argv),
               "ERROR: --test_flag must be set on the commandline");
}
#endif

TEST(FlagsValidator, InvalidFlagPtr) {
  int32 dummy;
  EXPECT_FALSE(RegisterFlagValidator(NULL, &ValidateTestFlagIs5));
  EXPECT_FALSE(RegisterFlagValidator(&dummy, &ValidateTestFlagIs5));
}

TEST(FlagsValidator, RegisterValidatorTwice) {
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_FALSE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs10));
  EXPECT_FALSE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs10));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs10));
  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
}

TEST(FlagsValidator, CommandLineFlagInfo) {
  CommandLineFlagInfo info;
  info = GetCommandLineFlagInfoOrDie("test_flag");
  EXPECT_FALSE(info.has_validator_fn);

  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  info = GetCommandLineFlagInfoOrDie("test_flag");
  EXPECT_TRUE(info.has_validator_fn);

  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
  info = GetCommandLineFlagInfoOrDie("test_flag");
  EXPECT_FALSE(info.has_validator_fn);
}

TEST(FlagsValidator, FlagSaver) {
  {
    FlagSaver fs;
    EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
    EXPECT_EQ("", SetCommandLineOption("test_flag", "50"));  // fails validation
  }
  EXPECT_NE("", SetCommandLineOption("test_flag", "50"));  // validator is gone

  EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, &ValidateTestFlagIs5));
  {
    FlagSaver fs;
    EXPECT_TRUE(RegisterFlagValidator(&FLAGS_test_flag, NULL));
    EXPECT_NE("", SetCommandLineOption("test_flag", "50"));  // no validator
  }
  EXPECT_EQ("", SetCommandLineOption("test_flag", "50"));  // validator is back
}


}  // unnamed namespace

int main(int argc, char **argv) {
  // We need to call SetArgv before parsing flags, so our "test" argv will
  // win out over this executable's real argv.  That makes running this
  // test with a real --help flag kinda annoying, unfortunately.
  const char* test_argv[] = { "/test/argv/for/gflags_unittest",
                              "argv 2", "3rd argv", "argv #4" };
  SetArgv(arraysize(test_argv), test_argv);

  // The first arg is the usage message, also important for testing.
  string usage_message = (string(GetArgv0()) +
                          ": <useless flag> [...]\nDoes something useless.\n");

  // We test setting tryfromenv manually, and making sure
  // ParseCommandLineFlags still evaluates it.
  FLAGS_tryfromenv = "test_tryfromenv";
  setenv("FLAGS_test_tryfromenv", "pre-set", 1);

  // Modify flag values from declared default value in two ways.
  // The recommended way:
  SetCommandLineOptionWithMode("changed_bool1", "true", SET_FLAGS_DEFAULT);

  // The non-recommended way:
  FLAGS_changed_bool2 = true;

  SetUsageMessage(usage_message.c_str());
  SetVersionString("test_version");
  ParseCommandLineFlags(&argc, &argv, true);
  MakeTmpdir(&FLAGS_test_tmpdir);

  const int exit_status = RUN_ALL_TESTS();
  ShutDownCommandLineFlags();
  return exit_status;
}

_END_GOOGLE_NAMESPACE_

int main(int argc, char** argv) {
  return GOOGLE_NAMESPACE::main(argc, argv);
}

