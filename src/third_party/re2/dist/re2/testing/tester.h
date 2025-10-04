// Copyright 2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef RE2_TESTING_TESTER_H_
#define RE2_TESTING_TESTER_H_

// Comparative tester for regular expression matching.
// Checks all implementations against each other.

#include <vector>

#include "absl/strings/string_view.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"
#include "util/pcre.h"

namespace re2 {

// All the supported regexp engines.
enum Engine {
  kEngineBacktrack = 0,    // Prog::UnsafeSearchBacktrack
  kEngineNFA,              // Prog::SearchNFA
  kEngineDFA,              // Prog::SearchDFA, only ask whether it matched
  kEngineDFA1,             // Prog::SearchDFA, ask for match[0]
  kEngineOnePass,          // Prog::SearchOnePass, if applicable
  kEngineBitState,         // Prog::SearchBitState
  kEngineRE2,              // RE2, all submatches
  kEngineRE2a,             // RE2, only ask for match[0]
  kEngineRE2b,             // RE2, only ask whether it matched
  kEnginePCRE,             // PCRE (util/pcre.h)

  kEngineMax,
};

// Make normal math on the enum preserve the type.
// By default, C++ doesn't define ++ on enum, and e+1 has type int.
static inline void operator++(Engine& e, int unused) {
  e = static_cast<Engine>(e+1);
}

static inline Engine operator+(Engine e, int i) {
  return static_cast<Engine>(static_cast<int>(e)+i);
}

// A TestInstance caches per-regexp state for a given
// regular expression in a given configuration
// (UTF-8 vs Latin1, longest vs first match, etc.).
class TestInstance {
 public:
  struct Result;

  TestInstance(absl::string_view regexp, Prog::MatchKind kind,
               Regexp::ParseFlags flags);
  ~TestInstance();
  Regexp::ParseFlags flags() { return flags_; }
  bool error() { return error_; }

  // Runs a single test case: search in text, which is in context,
  // using the given anchoring.
  bool RunCase(absl::string_view text, absl::string_view context,
               Prog::Anchor anchor);

 private:
  // Runs a single search using the named engine type.
  void RunSearch(Engine type, absl::string_view text, absl::string_view context,
                 Prog::Anchor anchor, Result* result);

  void LogMatch(const char* prefix, Engine e, absl::string_view text,
                absl::string_view context, Prog::Anchor anchor);

  absl::string_view regexp_str_;    // regexp being tested
  Prog::MatchKind kind_;            // kind of match
  Regexp::ParseFlags flags_;        // flags for parsing regexp_str_
  bool error_;                      // error during constructor?

  Regexp* regexp_;                  // parsed regexp
  int num_captures_;                // regexp_->NumCaptures() cached
  Prog* prog_;                      // compiled program
  Prog* rprog_;                     // compiled reverse program
  PCRE* re_;                        // PCRE implementation
  RE2* re2_;                        // RE2 implementation

  TestInstance(const TestInstance&) = delete;
  TestInstance& operator=(const TestInstance&) = delete;
};

// A group of TestInstances for all possible configurations.
class Tester {
 public:
  explicit Tester(absl::string_view regexp);
  ~Tester();

  bool error() { return error_; }

  // Runs a single test case: search in text, which is in context,
  // using the given anchoring.
  bool TestCase(absl::string_view text, absl::string_view context,
                Prog::Anchor anchor);

  // Run TestCase(text, text, anchor) for all anchoring modes.
  bool TestInput(absl::string_view text);

  // Run TestCase(text, context, anchor) for all anchoring modes.
  bool TestInputInContext(absl::string_view text, absl::string_view context);

 private:
  bool error_;
  std::vector<TestInstance*> v_;

  Tester(const Tester&) = delete;
  Tester& operator=(const Tester&) = delete;
};

// Run all possible tests using regexp and text.
bool TestRegexpOnText(absl::string_view regexp, absl::string_view text);

}  // namespace re2

#endif  // RE2_TESTING_TESTER_H_
