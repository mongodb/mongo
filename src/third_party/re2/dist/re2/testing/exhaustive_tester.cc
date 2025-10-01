// Copyright 2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Exhaustive testing of regular expression matching.

// Each test picks an alphabet (e.g., "abc"), a maximum string length,
// a maximum regular expression length, and a maximum number of letters
// that can appear in the regular expression.  Given these parameters,
// it tries every possible regular expression and string, verifying that
// the NFA, DFA, and a trivial backtracking implementation agree about
// the location of the match.

#include "re2/testing/exhaustive_tester.h"

#include <stdio.h>

#include <string>
#include <vector>

#include "absl/base/macros.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/testing/regexp_generator.h"
#include "re2/testing/tester.h"

// For target `log' in the Makefile.
#ifndef LOGGING
#define LOGGING 0
#endif

ABSL_FLAG(bool, show_regexps, false, "show regexps during testing");

ABSL_FLAG(int, max_bad_regexp_inputs, 1,
          "Stop testing a regular expression after finding this many "
          "strings that break it.");

namespace re2 {

static char* escape(absl::string_view sp) {
  static char buf[512];
  char* p = buf;
  *p++ = '\"';
  for (size_t i = 0; i < sp.size(); i++) {
    if(p+5 >= buf+sizeof buf)
      ABSL_LOG(FATAL) << "ExhaustiveTester escape: too long";
    if(sp[i] == '\\' || sp[i] == '\"') {
      *p++ = '\\';
      *p++ = sp[i];
    } else if(sp[i] == '\n') {
      *p++ = '\\';
      *p++ = 'n';
    } else {
      *p++ = sp[i];
    }
  }
  *p++ = '\"';
  *p = '\0';
  return buf;
}

static void PrintResult(const RE2& re, absl::string_view input,
                        RE2::Anchor anchor, absl::string_view* m, int n) {
  if (!re.Match(input, 0, input.size(), anchor, m, n)) {
    absl::PrintF("-");
    return;
  }
  for (int i = 0; i < n; i++) {
    if (i > 0)
      absl::PrintF(" ");
    if (m[i].data() == NULL)
      absl::PrintF("-");
    else
      absl::PrintF("%d-%d",
                   BeginPtr(m[i]) - BeginPtr(input),
                   EndPtr(m[i]) - BeginPtr(input));
  }
}

// Processes a single generated regexp.
// Compiles it using Regexp interface and PCRE, and then
// checks that NFA, DFA, and PCRE all return the same results.
void ExhaustiveTester::HandleRegexp(const std::string& const_regexp) {
  regexps_++;
  std::string regexp = const_regexp;
  if (!topwrapper_.empty()) {
    auto fmt = absl::ParsedFormat<'s'>::New(topwrapper_);
    ABSL_CHECK(fmt != nullptr);
    regexp = absl::StrFormat(*fmt, regexp);
  }

  if (absl::GetFlag(FLAGS_show_regexps)) {
    absl::PrintF("\r%s", regexp);
    fflush(stdout);
  }

  if (LOGGING) {
    // Write out test cases and answers for use in testing
    // other implementations, such as Go's regexp package.
    if (randomstrings_)
      ABSL_LOG(ERROR) << "Cannot log with random strings.";
    if (regexps_ == 1) {  // first
      absl::PrintF("strings\n");
      strgen_.Reset();
      while (strgen_.HasNext())
        absl::PrintF("%s\n", escape(strgen_.Next()));
      absl::PrintF("regexps\n");
    }
    absl::PrintF("%s\n", escape(regexp));

    RE2 re(regexp);
    RE2::Options longest;
    longest.set_longest_match(true);
    RE2 relongest(regexp, longest);
    int ngroup = re.NumberOfCapturingGroups()+1;
    absl::string_view* group = new absl::string_view[ngroup];

    strgen_.Reset();
    while (strgen_.HasNext()) {
      absl::string_view input = strgen_.Next();
      PrintResult(re, input, RE2::ANCHOR_BOTH, group, ngroup);
      absl::PrintF(";");
      PrintResult(re, input, RE2::UNANCHORED, group, ngroup);
      absl::PrintF(";");
      PrintResult(relongest, input, RE2::ANCHOR_BOTH, group, ngroup);
      absl::PrintF(";");
      PrintResult(relongest, input, RE2::UNANCHORED, group, ngroup);
      absl::PrintF("\n");
    }
    delete[] group;
    return;
  }

  Tester tester(regexp);
  if (tester.error())
    return;

  strgen_.Reset();
  strgen_.GenerateNULL();
  if (randomstrings_)
    strgen_.Random(stringseed_, stringcount_);
  int bad_inputs = 0;
  while (strgen_.HasNext()) {
    tests_++;
    if (!tester.TestInput(strgen_.Next())) {
      failures_++;
      if (++bad_inputs >= absl::GetFlag(FLAGS_max_bad_regexp_inputs))
        break;
    }
  }
}

// Runs an exhaustive test on the given parameters.
void ExhaustiveTest(int maxatoms, int maxops,
                    const std::vector<std::string>& alphabet,
                    const std::vector<std::string>& ops,
                    int maxstrlen,
                    const std::vector<std::string>& stralphabet,
                    const std::string& wrapper,
                    const std::string& topwrapper) {
  if (RE2_DEBUG_MODE) {
    if (maxatoms > 1)
      maxatoms--;
    if (maxops > 1)
      maxops--;
    if (maxstrlen > 1)
      maxstrlen--;
  }
  ExhaustiveTester t(maxatoms, maxops, alphabet, ops,
                     maxstrlen, stralphabet, wrapper,
                     topwrapper);
  t.Generate();
  if (!LOGGING) {
    absl::PrintF("%d regexps, %d tests, %d failures [%d/%d str]\n",
                 t.regexps(), t.tests(), t.failures(), maxstrlen, stralphabet.size());
  }
  EXPECT_EQ(0, t.failures());
}

// Runs an exhaustive test using the given parameters and
// the basic egrep operators.
void EgrepTest(int maxatoms, int maxops, const std::string& alphabet,
               int maxstrlen, const std::string& stralphabet,
               const std::string& wrapper) {
  const char* tops[] = { "", "^(?:%s)", "(?:%s)$", "^(?:%s)$" };

  for (size_t i = 0; i < ABSL_ARRAYSIZE(tops); i++) {
    ExhaustiveTest(maxatoms, maxops,
                   Split("", alphabet),
                   RegexpGenerator::EgrepOps(),
                   maxstrlen,
                   Split("", stralphabet),
                   wrapper,
                   tops[i]);
  }
}

}  // namespace re2
