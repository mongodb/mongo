// Copyright 2006-2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <thread>
#include <vector>

#include "absl/base/macros.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "gtest/gtest.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"
#include "re2/testing/string_generator.h"
#include "util/malloc_counter.h"

static const bool UsingMallocCounter = false;

ABSL_FLAG(int, size, 8, "log2(number of DFA nodes)");
ABSL_FLAG(int, repeat, 2, "Repetition count.");
ABSL_FLAG(int, threads, 4, "number of threads");

namespace re2 {

static int state_cache_resets = 0;
static int search_failures = 0;

struct SetHooks {
  SetHooks() {
    hooks::SetDFAStateCacheResetHook([](const hooks::DFAStateCacheReset&) {
      ++state_cache_resets;
    });
    hooks::SetDFASearchFailureHook([](const hooks::DFASearchFailure&) {
      ++search_failures;
    });
  }
} set_hooks;

// Check that multithreaded access to DFA class works.

// Helper function: builds entire DFA for prog.
static void DoBuild(Prog* prog) {
  ASSERT_TRUE(prog->BuildEntireDFA(Prog::kFirstMatch, nullptr));
}

TEST(Multithreaded, BuildEntireDFA) {
  // Create regexp with 2^FLAGS_size states in DFA.
  std::string s = "a";
  for (int i = 0; i < absl::GetFlag(FLAGS_size); i++)
    s += "[ab]";
  s += "b";
  Regexp* re = Regexp::Parse(s, Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);

  // Check that single-threaded code works.
  {
    Prog* prog = re->CompileToProg(0);
    ASSERT_TRUE(prog != NULL);

    std::thread t(DoBuild, prog);
    t.join();

    delete prog;
  }

  // Build the DFA simultaneously in a bunch of threads.
  for (int i = 0; i < absl::GetFlag(FLAGS_repeat); i++) {
    Prog* prog = re->CompileToProg(0);
    ASSERT_TRUE(prog != NULL);

    std::vector<std::thread> threads;
    for (int j = 0; j < absl::GetFlag(FLAGS_threads); j++)
      threads.emplace_back(DoBuild, prog);
    for (int j = 0; j < absl::GetFlag(FLAGS_threads); j++)
      threads[j].join();

    // One more compile, to make sure everything is okay.
    prog->BuildEntireDFA(Prog::kFirstMatch, nullptr);
    delete prog;
  }

  re->Decref();
}

// Check that DFA size requirements are followed.
// BuildEntireDFA will, like SearchDFA, stop building out
// the DFA once the memory limits are reached.
TEST(SingleThreaded, BuildEntireDFA) {
  // Create regexp with 2^30 states in DFA.
  Regexp* re = Regexp::Parse("a[ab]{30}b", Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);

  for (int i = 17; i < 24; i++) {
    int64_t limit = int64_t{1}<<i;
    int64_t usage;
    //int64_t progusage, dfamem;
    {
      testing::MallocCounter m(testing::MallocCounter::THIS_THREAD_ONLY);
      Prog* prog = re->CompileToProg(limit);
      ASSERT_TRUE(prog != NULL);
      //progusage = m.HeapGrowth();
      //dfamem = prog->dfa_mem();
      prog->BuildEntireDFA(Prog::kFirstMatch, nullptr);
      prog->BuildEntireDFA(Prog::kLongestMatch, nullptr);
      usage = m.HeapGrowth();
      delete prog;
    }
    if (UsingMallocCounter) {
      //ABSL_LOG(INFO) << "limit " << limit << ", "
      //               << "prog usage " << progusage << ", "
      //               << "DFA budget " << dfamem << ", "
      //               << "total " << usage;
      // Tolerate +/- 10%.
      ASSERT_GT(usage, limit*9/10);
      ASSERT_LT(usage, limit*11/10);
    }
  }
  re->Decref();
}

// Test that the DFA gets the right result even if it runs
// out of memory during a search.  The regular expression
// 0[01]{n}$ matches a binary string of 0s and 1s only if
// the (n+1)th-to-last character is a 0.  Matching this in
// a single forward pass (as done by the DFA) requires
// keeping one bit for each of the last n+1 characters
// (whether each was a 0), or 2^(n+1) possible states.
// If we run this regexp to search in a string that contains
// every possible n-character binary string as a substring,
// then it will have to run through at least 2^n states.
// States are big data structures -- certainly more than 1 byte --
// so if the DFA can search correctly while staying within a
// 2^n byte limit, it must be handling out-of-memory conditions
// gracefully.
TEST(SingleThreaded, SearchDFA) {
  // The De Bruijn string is the worst case input for this regexp.
  // By default, the DFA will notice that it is flushing its cache
  // too frequently and will bail out early, so that RE2 can use the
  // NFA implementation instead.  (The DFA loses its speed advantage
  // if it can't get a good cache hit rate.)
  // Tell the DFA to trudge along instead.
  Prog::TESTING_ONLY_set_dfa_should_bail_when_slow(false);
  state_cache_resets = 0;
  search_failures = 0;

  // Choice of n is mostly arbitrary, except that:
  //   * making n too big makes the test run for too long.
  //   * making n too small makes the DFA refuse to run,
  //     because it has so little memory compared to the program size.
  // Empirically, n = 18 is a good compromise between the two.
  const int n = 18;

  Regexp* re = Regexp::Parse(absl::StrFormat("0[01]{%d}$", n),
                             Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);

  // The De Bruijn string for n ends with a 1 followed by n 0s in a row,
  // which is not a match for 0[01]{n}$.  Adding one more 0 is a match.
  std::string no_match = DeBruijnString(n);
  std::string match = no_match + "0";

  int64_t usage;
  int64_t peak_usage;
  {
    testing::MallocCounter m(testing::MallocCounter::THIS_THREAD_ONLY);
    Prog* prog = re->CompileToProg(1<<n);
    ASSERT_TRUE(prog != NULL);
    for (int i = 0; i < 10; i++) {
      bool matched = false;
      bool failed = false;
      matched =
          prog->SearchDFA(match, absl::string_view(), Prog::kUnanchored,
                          Prog::kFirstMatch, NULL, &failed, NULL);
      ASSERT_FALSE(failed);
      ASSERT_TRUE(matched);
      matched =
          prog->SearchDFA(no_match, absl::string_view(), Prog::kUnanchored,
                          Prog::kFirstMatch, NULL, &failed, NULL);
      ASSERT_FALSE(failed);
      ASSERT_FALSE(matched);
    }
    usage = m.HeapGrowth();
    peak_usage = m.PeakHeapGrowth();
    delete prog;
  }
  if (UsingMallocCounter) {
    //ABSL_LOG(INFO) << "usage " << usage << ", "
    //               << "peak usage " << peak_usage;
    ASSERT_LT(usage, 1<<n);
    ASSERT_LT(peak_usage, 1<<n);
  }
  re->Decref();

  // Reset to original behaviour.
  Prog::TESTING_ONLY_set_dfa_should_bail_when_slow(true);
  ASSERT_GT(state_cache_resets, 0);
  ASSERT_EQ(search_failures, 0);
}

// Helper function: searches for match, which should match,
// and no_match, which should not.
static void DoSearch(Prog* prog, absl::string_view match,
                     absl::string_view no_match) {
  for (int i = 0; i < 2; i++) {
    bool matched = false;
    bool failed = false;
    matched =
        prog->SearchDFA(match, absl::string_view(), Prog::kUnanchored,
                        Prog::kFirstMatch, NULL, &failed, NULL);
    ASSERT_FALSE(failed);
    ASSERT_TRUE(matched);
    matched =
        prog->SearchDFA(no_match, absl::string_view(), Prog::kUnanchored,
                        Prog::kFirstMatch, NULL, &failed, NULL);
    ASSERT_FALSE(failed);
    ASSERT_FALSE(matched);
  }
}

TEST(Multithreaded, SearchDFA) {
  Prog::TESTING_ONLY_set_dfa_should_bail_when_slow(false);
  state_cache_resets = 0;
  search_failures = 0;

  // Same as single-threaded test above.
  const int n = 18;
  Regexp* re = Regexp::Parse(absl::StrFormat("0[01]{%d}$", n),
                             Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);
  std::string no_match = DeBruijnString(n);
  std::string match = no_match + "0";

  // Check that single-threaded code works.
  {
    Prog* prog = re->CompileToProg(1<<n);
    ASSERT_TRUE(prog != NULL);

    std::thread t(DoSearch, prog, match, no_match);
    t.join();

    delete prog;
  }

  // Run the search simultaneously in a bunch of threads.
  // Reuse same flags for Multithreaded.BuildDFA above.
  for (int i = 0; i < absl::GetFlag(FLAGS_repeat); i++) {
    Prog* prog = re->CompileToProg(1<<n);
    ASSERT_TRUE(prog != NULL);

    std::vector<std::thread> threads;
    for (int j = 0; j < absl::GetFlag(FLAGS_threads); j++)
      threads.emplace_back(DoSearch, prog, match, no_match);
    for (int j = 0; j < absl::GetFlag(FLAGS_threads); j++)
      threads[j].join();

    delete prog;
  }

  re->Decref();

  // Reset to original behaviour.
  Prog::TESTING_ONLY_set_dfa_should_bail_when_slow(true);
  ASSERT_GT(state_cache_resets, 0);
  ASSERT_EQ(search_failures, 0);
}

struct ReverseTest {
  const char* regexp;
  const char* text;
  bool match;
};

// Test that reverse DFA handles anchored/unanchored correctly.
// It's in the DFA interface but not used by RE2.
ReverseTest reverse_tests[] = {
  { "\\A(a|b)", "abc", true },
  { "(a|b)\\z", "cba", true },
  { "\\A(a|b)", "cba", false },
  { "(a|b)\\z", "abc", false },
};

TEST(DFA, ReverseMatch) {
  int nfail = 0;
  for (size_t i = 0; i < ABSL_ARRAYSIZE(reverse_tests); i++) {
    const ReverseTest& t = reverse_tests[i];
    Regexp* re = Regexp::Parse(t.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL);
    Prog* prog = re->CompileToReverseProg(0);
    ASSERT_TRUE(prog != NULL);
    bool failed = false;
    bool matched =
        prog->SearchDFA(t.text, absl::string_view(), Prog::kUnanchored,
                        Prog::kFirstMatch, NULL, &failed, NULL);
    if (matched != t.match) {
      ABSL_LOG(ERROR) << t.regexp << " on " << t.text << ": want " << t.match;
      nfail++;
    }
    delete prog;
    re->Decref();
  }
  EXPECT_EQ(nfail, 0);
}

struct CallbackTest {
  const char* regexp;
  const char* dump;
};

// Test that DFA::BuildAllStates() builds the expected DFA states
// and issues the expected callbacks. These test cases reflect the
// very compact encoding of the callbacks, but that also makes them
// very difficult to understand, so let's work through "\\Aa\\z".
// There are three slots per DFA state because the bytemap has two
// equivalence classes and there is a third slot for kByteEndText:
//   0: all bytes that are not 'a'
//   1: the byte 'a'
//   2: kByteEndText
// -1 means that there is no transition from that DFA state to any
// other DFA state for that slot. The valid transitions are thus:
//   state 0 --slot 1--> state 1
//   state 1 --slot 2--> state 2
// The double brackets indicate that state 2 is a matching state.
// Putting it together, this means that the DFA must consume the
// byte 'a' and then hit end of text. Q.E.D.
CallbackTest callback_tests[] = {
  { "\\Aa\\z", "[-1,1,-1] [-1,-1,2] [[-1,-1,-1]]" },
  { "\\Aab\\z", "[-1,1,-1,-1] [-1,-1,2,-1] [-1,-1,-1,3] [[-1,-1,-1,-1]]" },
  { "\\Aa*b\\z", "[-1,0,1,-1] [-1,-1,-1,2] [[-1,-1,-1,-1]]" },
  { "\\Aa+b\\z", "[-1,1,-1,-1] [-1,1,2,-1] [-1,-1,-1,3] [[-1,-1,-1,-1]]" },
  { "\\Aa?b\\z", "[-1,1,2,-1] [-1,-1,2,-1] [-1,-1,-1,3] [[-1,-1,-1,-1]]" },
  { "\\Aa\\C*\\z", "[-1,1,-1] [1,1,2] [[-1,-1,-1]]" },
  { "\\Aa\\C*", "[-1,1,-1] [2,2,3] [[2,2,2]] [[-1,-1,-1]]" },
  { "a\\C*", "[0,1,-1] [2,2,3] [[2,2,2]] [[-1,-1,-1]]" },
  { "\\C*", "[1,2] [[1,1]] [[-1,-1]]" },
  { "a", "[0,1,-1] [2,2,2] [[-1,-1,-1]]"} ,
};

TEST(DFA, Callback) {
  int nfail = 0;
  for (size_t i = 0; i < ABSL_ARRAYSIZE(callback_tests); i++) {
    const CallbackTest& t = callback_tests[i];
    Regexp* re = Regexp::Parse(t.regexp, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL);
    Prog* prog = re->CompileToProg(0);
    ASSERT_TRUE(prog != NULL);
    std::string dump;
    prog->BuildEntireDFA(Prog::kLongestMatch, [&](const int* next, bool match) {
      ASSERT_TRUE(next != NULL);
      if (!dump.empty())
        dump += " ";
      dump += match ? "[[" : "[";
      for (int b = 0; b < prog->bytemap_range() + 1; b++)
        dump += absl::StrFormat("%d,", next[b]);
      dump.pop_back();
      dump += match ? "]]" : "]";
    });
    if (dump != t.dump) {
      ABSL_LOG(ERROR) << t.regexp << " bytemap:\n" << prog->DumpByteMap();
      ABSL_LOG(ERROR) << t.regexp << " dump:\n" << "got " << dump << "\n"
                      << "want " << t.dump;
      nfail++;
    }
    delete prog;
    re->Decref();
  }
  EXPECT_EQ(nfail, 0);
}

}  // namespace re2
