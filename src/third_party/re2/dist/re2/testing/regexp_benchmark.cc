// Copyright 2006-2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Benchmarks for regular expression implementations.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "benchmark/benchmark.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"
#include "util/malloc_counter.h"
#include "util/pcre.h"

namespace re2 {
void Test();
void MemoryUsage();
}  // namespace re2

typedef testing::MallocCounter MallocCounter;

namespace re2 {

void Test() {
  Regexp* re = Regexp::Parse("(\\d+)-(\\d+)-(\\d+)", Regexp::LikePerl, NULL);
  ABSL_CHECK(re);
  Prog* prog = re->CompileToProg(0);
  ABSL_CHECK(prog);
  ABSL_CHECK(prog->IsOnePass());
  ABSL_CHECK(prog->CanBitState());
  const char* text = "650-253-0001";
  absl::string_view sp[4];
  ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored, Prog::kFullMatch,
                                 sp, 4));
  ABSL_CHECK_EQ(sp[0], "650-253-0001");
  ABSL_CHECK_EQ(sp[1], "650");
  ABSL_CHECK_EQ(sp[2], "253");
  ABSL_CHECK_EQ(sp[3], "0001");
  delete prog;
  re->Decref();
  ABSL_LOG(INFO) << "test passed\n";
}

void MemoryUsage() {
  const char* regexp = "(\\d+)-(\\d+)-(\\d+)";
  const char* text = "650-253-0001";
  {
    MallocCounter mc(MallocCounter::THIS_THREAD_ONLY);
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    // Can't pass mc.HeapGrowth() and mc.PeakHeapGrowth() to ABSL_LOG(INFO)
    // directly because ABSL_LOG(INFO) might do a big allocation before they
    // get evaluated.
    absl::FPrintF(stderr, "Regexp: %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    mc.Reset();

    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->IsOnePass());
    ABSL_CHECK(prog->CanBitState());
    absl::FPrintF(stderr, "Prog:   %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    mc.Reset();

    absl::string_view sp[4];
    ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored,
                                   Prog::kFullMatch, sp, 4));
    absl::FPrintF(stderr, "Search: %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    delete prog;
    re->Decref();
  }

  {
    MallocCounter mc(MallocCounter::THIS_THREAD_ONLY);

    PCRE re(regexp, PCRE::UTF8);
    absl::FPrintF(stderr, "RE:     %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    PCRE::FullMatch(text, re);
    absl::FPrintF(stderr, "RE:     %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
  }

  {
    MallocCounter mc(MallocCounter::THIS_THREAD_ONLY);

    PCRE* re = new PCRE(regexp, PCRE::UTF8);
    absl::FPrintF(stderr, "PCRE*:  %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    PCRE::FullMatch(text, *re);
    absl::FPrintF(stderr, "PCRE*:  %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    delete re;
  }

  {
    MallocCounter mc(MallocCounter::THIS_THREAD_ONLY);

    RE2 re(regexp);
    absl::FPrintF(stderr, "RE2:    %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
    RE2::FullMatch(text, re);
    absl::FPrintF(stderr, "RE2:    %7d bytes (peak=%d)\n",
                  mc.HeapGrowth(), mc.PeakHeapGrowth());
  }

  absl::FPrintF(stderr, "sizeof: PCRE=%d RE2=%d Prog=%d Inst=%d\n",
                sizeof(PCRE), sizeof(RE2), sizeof(Prog), sizeof(Prog::Inst));
}

int NumCPUs() {
  return static_cast<int>(std::thread::hardware_concurrency());
}

// Regular expression implementation wrappers.
// Defined at bottom of file, but they are repetitive
// and not interesting.

typedef void SearchImpl(benchmark::State& state, const char* regexp,
                        absl::string_view text, Prog::Anchor anchor,
                        bool expect_match);

SearchImpl SearchDFA, SearchNFA, SearchOnePass, SearchBitState, SearchPCRE,
    SearchRE2, SearchCachedDFA, SearchCachedNFA, SearchCachedOnePass,
    SearchCachedBitState, SearchCachedPCRE, SearchCachedRE2;

typedef void ParseImpl(benchmark::State& state, const char* regexp,
                       absl::string_view text);

ParseImpl Parse1NFA, Parse1OnePass, Parse1BitState, Parse1PCRE, Parse1RE2,
    Parse1Backtrack, Parse1CachedNFA, Parse1CachedOnePass, Parse1CachedBitState,
    Parse1CachedPCRE, Parse1CachedRE2, Parse1CachedBacktrack;

ParseImpl Parse3NFA, Parse3OnePass, Parse3BitState, Parse3PCRE, Parse3RE2,
    Parse3Backtrack, Parse3CachedNFA, Parse3CachedOnePass, Parse3CachedBitState,
    Parse3CachedPCRE, Parse3CachedRE2, Parse3CachedBacktrack;

ParseImpl SearchParse2CachedPCRE, SearchParse2CachedRE2;

ParseImpl SearchParse1CachedPCRE, SearchParse1CachedRE2;

// Benchmark: failed search for regexp in random text.

// Generate random text that won't contain the search string,
// to test worst-case search behavior.
std::string RandomText(int64_t nbytes) {
  static const std::string* const text = []() {
    std::string* text = new std::string;
    srand(1);
    text->resize(16<<20);
    for (int64_t i = 0; i < 16<<20; i++) {
      // Generate a one-byte rune that isn't a control character (e.g. '\n').
      // Clipping to 0x20 introduces some bias, but we don't need uniformity.
      int byte = rand() & 0x7F;
      if (byte < 0x20)
        byte = 0x20;
      (*text)[i] = byte;
    }
    return text;
  }();
  ABSL_CHECK_LE(nbytes, 16<<20);
  return text->substr(0, nbytes);
}

// Makes text of size nbytes, then calls run to search
// the text for regexp iters times.
void Search(benchmark::State& state, const char* regexp, SearchImpl* search) {
  std::string s = RandomText(state.range(0));
  search(state, regexp, s, Prog::kUnanchored, false);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// These three are easy because they have prefixes,
// giving the search loop something to prefix accel.
#define EASY0      "ABCDEFGHIJKLMNOPQRSTUVWXYZ$"
#define EASY1      "A[AB]B[BC]C[CD]D[DE]E[EF]F[FG]G[GH]H[HI]I[IJ]J$"
#define EASY2      "(?i)" EASY0

// This is a little harder, since it starts with a character class
// and thus can't be memchr'ed.  Could look for ABC and work backward,
// but no one does that.
#define MEDIUM     "[XYZ]ABCDEFGHIJKLMNOPQRSTUVWXYZ$"

// This is a fair amount harder, because of the leading [ -~]*.
// A bad backtracking implementation will take O(text^2) time to
// figure out there's no match.
#define HARD       "[ -~]*ABCDEFGHIJKLMNOPQRSTUVWXYZ$"

// This has quite a high degree of fanout.
// NFA execution will be particularly slow.
#define FANOUT     "(?:[\\x{80}-\\x{10FFFF}]?){100}[\\x{80}-\\x{10FFFF}]"

// This stresses engines that are trying to track parentheses.
#define PARENS     "([ -~])*(A)(B)(C)(D)(E)(F)(G)(H)(I)(J)(K)(L)(M)" \
                   "(N)(O)(P)(Q)(R)(S)(T)(U)(V)(W)(X)(Y)(Z)$"

void Search_Easy0_CachedDFA(benchmark::State& state)     { Search(state, EASY0, SearchCachedDFA); }
void Search_Easy0_CachedNFA(benchmark::State& state)     { Search(state, EASY0, SearchCachedNFA); }
void Search_Easy0_CachedPCRE(benchmark::State& state)    { Search(state, EASY0, SearchCachedPCRE); }
void Search_Easy0_CachedRE2(benchmark::State& state)     { Search(state, EASY0, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Easy0_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Easy0_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Easy0_CachedPCRE,    8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Easy0_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Easy1_CachedDFA(benchmark::State& state)     { Search(state, EASY1, SearchCachedDFA); }
void Search_Easy1_CachedNFA(benchmark::State& state)     { Search(state, EASY1, SearchCachedNFA); }
void Search_Easy1_CachedPCRE(benchmark::State& state)    { Search(state, EASY1, SearchCachedPCRE); }
void Search_Easy1_CachedRE2(benchmark::State& state)     { Search(state, EASY1, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Easy1_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Easy1_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Easy1_CachedPCRE,    8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Easy1_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Easy2_CachedDFA(benchmark::State& state)     { Search(state, EASY2, SearchCachedDFA); }
void Search_Easy2_CachedNFA(benchmark::State& state)     { Search(state, EASY2, SearchCachedNFA); }
void Search_Easy2_CachedPCRE(benchmark::State& state)    { Search(state, EASY2, SearchCachedPCRE); }
void Search_Easy2_CachedRE2(benchmark::State& state)     { Search(state, EASY2, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Easy2_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Easy2_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Easy2_CachedPCRE,    8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Easy2_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Medium_CachedDFA(benchmark::State& state)     { Search(state, MEDIUM, SearchCachedDFA); }
void Search_Medium_CachedNFA(benchmark::State& state)     { Search(state, MEDIUM, SearchCachedNFA); }
void Search_Medium_CachedPCRE(benchmark::State& state)    { Search(state, MEDIUM, SearchCachedPCRE); }
void Search_Medium_CachedRE2(benchmark::State& state)     { Search(state, MEDIUM, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Medium_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Medium_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Medium_CachedPCRE,    8, 256<<10)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Medium_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Hard_CachedDFA(benchmark::State& state)     { Search(state, HARD, SearchCachedDFA); }
void Search_Hard_CachedNFA(benchmark::State& state)     { Search(state, HARD, SearchCachedNFA); }
void Search_Hard_CachedPCRE(benchmark::State& state)    { Search(state, HARD, SearchCachedPCRE); }
void Search_Hard_CachedRE2(benchmark::State& state)     { Search(state, HARD, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Hard_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Hard_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Hard_CachedPCRE,    8, 4<<10)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Hard_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Fanout_CachedDFA(benchmark::State& state)     { Search(state, FANOUT, SearchCachedDFA); }
void Search_Fanout_CachedNFA(benchmark::State& state)     { Search(state, FANOUT, SearchCachedNFA); }
void Search_Fanout_CachedPCRE(benchmark::State& state)    { Search(state, FANOUT, SearchCachedPCRE); }
void Search_Fanout_CachedRE2(benchmark::State& state)     { Search(state, FANOUT, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Fanout_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Fanout_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Fanout_CachedPCRE,    8, 4<<10)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Fanout_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_Parens_CachedDFA(benchmark::State& state)     { Search(state, PARENS, SearchCachedDFA); }
void Search_Parens_CachedNFA(benchmark::State& state)     { Search(state, PARENS, SearchCachedNFA); }
void Search_Parens_CachedPCRE(benchmark::State& state)    { Search(state, PARENS, SearchCachedPCRE); }
void Search_Parens_CachedRE2(benchmark::State& state)     { Search(state, PARENS, SearchCachedRE2); }

BENCHMARK_RANGE(Search_Parens_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Parens_CachedNFA,     8, 256<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Parens_CachedPCRE,    8, 8)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Parens_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());

void SearchBigFixed(benchmark::State& state, SearchImpl* search) {
  std::string s;
  s.append(state.range(0)/2, 'x');
  std::string regexp = "^" + s + ".*$";
  std::string t = RandomText(state.range(0)/2);
  s += t;
  search(state, regexp.c_str(), s, Prog::kUnanchored, true);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void Search_BigFixed_CachedDFA(benchmark::State& state)     { SearchBigFixed(state, SearchCachedDFA); }
void Search_BigFixed_CachedNFA(benchmark::State& state)     { SearchBigFixed(state, SearchCachedNFA); }
void Search_BigFixed_CachedPCRE(benchmark::State& state)    { SearchBigFixed(state, SearchCachedPCRE); }
void Search_BigFixed_CachedRE2(benchmark::State& state)     { SearchBigFixed(state, SearchCachedRE2); }

BENCHMARK_RANGE(Search_BigFixed_CachedDFA,     8, 1<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_BigFixed_CachedNFA,     8, 32<<10)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_BigFixed_CachedPCRE,    8, 32<<10)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_BigFixed_CachedRE2,     8, 1<<20)->ThreadRange(1, NumCPUs());

// Benchmark: FindAndConsume

void FindAndConsume(benchmark::State& state) {
  std::string s = RandomText(state.range(0));
  s.append("Hello World");
  RE2 re("((Hello World))");
  for (auto _ : state) {
    absl::string_view t = s;
    absl::string_view u;
    ABSL_CHECK(RE2::FindAndConsume(&t, re, &u));
    ABSL_CHECK_EQ(u, "Hello World");
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

BENCHMARK_RANGE(FindAndConsume, 8, 16<<20)->ThreadRange(1, NumCPUs());

// Benchmark: successful anchored search.

void SearchSuccess(benchmark::State& state, const char* regexp,
                   SearchImpl* search) {
  std::string s = RandomText(state.range(0));
  search(state, regexp, s, Prog::kAnchored, true);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

// Unambiguous search (RE2 can use OnePass).

void Search_Success_DFA(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchDFA); }
void Search_Success_NFA(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchNFA); }
void Search_Success_PCRE(benchmark::State& state)    { SearchSuccess(state, ".*$", SearchPCRE); }
void Search_Success_RE2(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchRE2); }
void Search_Success_OnePass(benchmark::State& state) { SearchSuccess(state, ".*$", SearchOnePass); }

BENCHMARK_RANGE(Search_Success_DFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success_NFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Success_PCRE,    8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Success_RE2,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success_OnePass, 8, 2<<20)->ThreadRange(1, NumCPUs());

void Search_Success_CachedDFA(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchCachedDFA); }
void Search_Success_CachedNFA(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchCachedNFA); }
void Search_Success_CachedPCRE(benchmark::State& state)    { SearchSuccess(state, ".*$", SearchCachedPCRE); }
void Search_Success_CachedRE2(benchmark::State& state)     { SearchSuccess(state, ".*$", SearchCachedRE2); }
void Search_Success_CachedOnePass(benchmark::State& state) { SearchSuccess(state, ".*$", SearchCachedOnePass); }

BENCHMARK_RANGE(Search_Success_CachedDFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success_CachedNFA,     8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Success_CachedPCRE,    8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Success_CachedRE2,     8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success_CachedOnePass, 8, 2<<20)->ThreadRange(1, NumCPUs());

// Ambiguous search (RE2 cannot use OnePass).
// Used to be ".*.$", but that is coalesced to ".+$" these days.

void Search_Success1_DFA(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchDFA); }
void Search_Success1_NFA(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchNFA); }
void Search_Success1_PCRE(benchmark::State& state)     { SearchSuccess(state, ".*\\C$", SearchPCRE); }
void Search_Success1_RE2(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchRE2); }
void Search_Success1_BitState(benchmark::State& state) { SearchSuccess(state, ".*\\C$", SearchBitState); }

BENCHMARK_RANGE(Search_Success1_DFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success1_NFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Success1_PCRE,     8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Success1_RE2,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success1_BitState, 8, 2<<20)->ThreadRange(1, NumCPUs());

void Search_Success1_CachedDFA(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchCachedDFA); }
void Search_Success1_CachedNFA(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchCachedNFA); }
void Search_Success1_CachedPCRE(benchmark::State& state)     { SearchSuccess(state, ".*\\C$", SearchCachedPCRE); }
void Search_Success1_CachedRE2(benchmark::State& state)      { SearchSuccess(state, ".*\\C$", SearchCachedRE2); }
void Search_Success1_CachedBitState(benchmark::State& state) { SearchSuccess(state, ".*\\C$", SearchCachedBitState); }

BENCHMARK_RANGE(Search_Success1_CachedDFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success1_CachedNFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_Success1_CachedPCRE,     8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_Success1_CachedRE2,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_Success1_CachedBitState, 8, 2<<20)->ThreadRange(1, NumCPUs());

// Benchmark: AltMatch optimisation (just to verify that it works)
// Note that OnePass doesn't implement it!

void SearchAltMatch(benchmark::State& state, SearchImpl* search) {
  std::string s = RandomText(state.range(0));
  search(state, "\\C*", s, Prog::kAnchored, true);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void Search_AltMatch_DFA(benchmark::State& state)      { SearchAltMatch(state, SearchDFA); }
void Search_AltMatch_NFA(benchmark::State& state)      { SearchAltMatch(state, SearchNFA); }
void Search_AltMatch_OnePass(benchmark::State& state)  { SearchAltMatch(state, SearchOnePass); }
void Search_AltMatch_BitState(benchmark::State& state) { SearchAltMatch(state, SearchBitState); }
void Search_AltMatch_PCRE(benchmark::State& state)     { SearchAltMatch(state, SearchPCRE); }
void Search_AltMatch_RE2(benchmark::State& state)      { SearchAltMatch(state, SearchRE2); }

BENCHMARK_RANGE(Search_AltMatch_DFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_NFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_OnePass,  8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_BitState, 8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_AltMatch_PCRE,     8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_AltMatch_RE2,      8, 16<<20)->ThreadRange(1, NumCPUs());

void Search_AltMatch_CachedDFA(benchmark::State& state)      { SearchAltMatch(state, SearchCachedDFA); }
void Search_AltMatch_CachedNFA(benchmark::State& state)      { SearchAltMatch(state, SearchCachedNFA); }
void Search_AltMatch_CachedOnePass(benchmark::State& state)  { SearchAltMatch(state, SearchCachedOnePass); }
void Search_AltMatch_CachedBitState(benchmark::State& state) { SearchAltMatch(state, SearchCachedBitState); }
void Search_AltMatch_CachedPCRE(benchmark::State& state)     { SearchAltMatch(state, SearchCachedPCRE); }
void Search_AltMatch_CachedRE2(benchmark::State& state)      { SearchAltMatch(state, SearchCachedRE2); }

BENCHMARK_RANGE(Search_AltMatch_CachedDFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_CachedNFA,      8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_CachedOnePass,  8, 16<<20)->ThreadRange(1, NumCPUs());
BENCHMARK_RANGE(Search_AltMatch_CachedBitState, 8, 16<<20)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK_RANGE(Search_AltMatch_CachedPCRE,     8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(Search_AltMatch_CachedRE2,      8, 16<<20)->ThreadRange(1, NumCPUs());

// Benchmark: use regexp to find phone number.

void SearchDigits(benchmark::State& state, SearchImpl* search) {
  absl::string_view s("650-253-0001");
  search(state, "([0-9]+)-([0-9]+)-([0-9]+)", s, Prog::kAnchored, true);
  state.SetItemsProcessed(state.iterations());
}

void Search_Digits_DFA(benchmark::State& state)         { SearchDigits(state, SearchDFA); }
void Search_Digits_NFA(benchmark::State& state)         { SearchDigits(state, SearchNFA); }
void Search_Digits_OnePass(benchmark::State& state)     { SearchDigits(state, SearchOnePass); }
void Search_Digits_PCRE(benchmark::State& state)        { SearchDigits(state, SearchPCRE); }
void Search_Digits_RE2(benchmark::State& state)         { SearchDigits(state, SearchRE2); }
void Search_Digits_BitState(benchmark::State& state)    { SearchDigits(state, SearchBitState); }

BENCHMARK(Search_Digits_DFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Search_Digits_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Search_Digits_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Search_Digits_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Search_Digits_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Search_Digits_BitState)->ThreadRange(1, NumCPUs());

// Benchmark: use regexp to parse digit fields in phone number.

void Parse3Digits(benchmark::State& state,
                  void (*parse3)(benchmark::State&, const char*,
                                 absl::string_view)) {
  parse3(state, "([0-9]+)-([0-9]+)-([0-9]+)", "650-253-0001");
  state.SetItemsProcessed(state.iterations());
}

void Parse_Digits_NFA(benchmark::State& state)         { Parse3Digits(state, Parse3NFA); }
void Parse_Digits_OnePass(benchmark::State& state)     { Parse3Digits(state, Parse3OnePass); }
void Parse_Digits_PCRE(benchmark::State& state)        { Parse3Digits(state, Parse3PCRE); }
void Parse_Digits_RE2(benchmark::State& state)         { Parse3Digits(state, Parse3RE2); }
void Parse_Digits_Backtrack(benchmark::State& state)   { Parse3Digits(state, Parse3Backtrack); }
void Parse_Digits_BitState(benchmark::State& state)    { Parse3Digits(state, Parse3BitState); }

BENCHMARK(Parse_Digits_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_Digits_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_Digits_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_Digits_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_Digits_Backtrack)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_Digits_BitState)->ThreadRange(1, NumCPUs());

void Parse_CachedDigits_NFA(benchmark::State& state)         { Parse3Digits(state, Parse3CachedNFA); }
void Parse_CachedDigits_OnePass(benchmark::State& state)     { Parse3Digits(state, Parse3CachedOnePass); }
void Parse_CachedDigits_PCRE(benchmark::State& state)        { Parse3Digits(state, Parse3CachedPCRE); }
void Parse_CachedDigits_RE2(benchmark::State& state)         { Parse3Digits(state, Parse3CachedRE2); }
void Parse_CachedDigits_Backtrack(benchmark::State& state)   { Parse3Digits(state, Parse3CachedBacktrack); }
void Parse_CachedDigits_BitState(benchmark::State& state)    { Parse3Digits(state, Parse3CachedBitState); }

BENCHMARK(Parse_CachedDigits_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigits_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_CachedDigits_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedDigits_Backtrack)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigits_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigits_BitState)->ThreadRange(1, NumCPUs());

void Parse3DigitDs(benchmark::State& state,
                   void (*parse3)(benchmark::State&, const char*,
                                  absl::string_view)) {
  parse3(state, "(\\d+)-(\\d+)-(\\d+)", "650-253-0001");
  state.SetItemsProcessed(state.iterations());
}

void Parse_DigitDs_NFA(benchmark::State& state)         { Parse3DigitDs(state, Parse3NFA); }
void Parse_DigitDs_OnePass(benchmark::State& state)     { Parse3DigitDs(state, Parse3OnePass); }
void Parse_DigitDs_PCRE(benchmark::State& state)        { Parse3DigitDs(state, Parse3PCRE); }
void Parse_DigitDs_RE2(benchmark::State& state)         { Parse3DigitDs(state, Parse3RE2); }
void Parse_DigitDs_Backtrack(benchmark::State& state)   { Parse3DigitDs(state, Parse3CachedBacktrack); }
void Parse_DigitDs_BitState(benchmark::State& state)    { Parse3DigitDs(state, Parse3CachedBitState); }

BENCHMARK(Parse_DigitDs_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_DigitDs_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_DigitDs_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_DigitDs_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_DigitDs_Backtrack)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_DigitDs_BitState)->ThreadRange(1, NumCPUs());

void Parse_CachedDigitDs_NFA(benchmark::State& state)         { Parse3DigitDs(state, Parse3CachedNFA); }
void Parse_CachedDigitDs_OnePass(benchmark::State& state)     { Parse3DigitDs(state, Parse3CachedOnePass); }
void Parse_CachedDigitDs_PCRE(benchmark::State& state)        { Parse3DigitDs(state, Parse3CachedPCRE); }
void Parse_CachedDigitDs_RE2(benchmark::State& state)         { Parse3DigitDs(state, Parse3CachedRE2); }
void Parse_CachedDigitDs_Backtrack(benchmark::State& state)   { Parse3DigitDs(state, Parse3CachedBacktrack); }
void Parse_CachedDigitDs_BitState(benchmark::State& state)    { Parse3DigitDs(state, Parse3CachedBitState); }

BENCHMARK(Parse_CachedDigitDs_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigitDs_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_CachedDigitDs_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedDigitDs_Backtrack)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigitDs_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedDigitDs_BitState)->ThreadRange(1, NumCPUs());

// Benchmark: splitting off leading number field.

void Parse1Split(benchmark::State& state,
                 void (*parse1)(benchmark::State&, const char*,
                                absl::string_view)) {
  parse1(state, "[0-9]+-(.*)", "650-253-0001");
  state.SetItemsProcessed(state.iterations());
}

void Parse_Split_NFA(benchmark::State& state)         { Parse1Split(state, Parse1NFA); }
void Parse_Split_OnePass(benchmark::State& state)     { Parse1Split(state, Parse1OnePass); }
void Parse_Split_PCRE(benchmark::State& state)        { Parse1Split(state, Parse1PCRE); }
void Parse_Split_RE2(benchmark::State& state)         { Parse1Split(state, Parse1RE2); }
void Parse_Split_BitState(benchmark::State& state)    { Parse1Split(state, Parse1BitState); }

BENCHMARK(Parse_Split_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_Split_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_Split_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_Split_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_Split_BitState)->ThreadRange(1, NumCPUs());

void Parse_CachedSplit_NFA(benchmark::State& state)         { Parse1Split(state, Parse1CachedNFA); }
void Parse_CachedSplit_OnePass(benchmark::State& state)     { Parse1Split(state, Parse1CachedOnePass); }
void Parse_CachedSplit_PCRE(benchmark::State& state)        { Parse1Split(state, Parse1CachedPCRE); }
void Parse_CachedSplit_RE2(benchmark::State& state)         { Parse1Split(state, Parse1CachedRE2); }
void Parse_CachedSplit_BitState(benchmark::State& state)    { Parse1Split(state, Parse1CachedBitState); }

BENCHMARK(Parse_CachedSplit_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedSplit_OnePass)->ThreadRange(1, NumCPUs());
#ifdef USEPCRE
BENCHMARK(Parse_CachedSplit_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedSplit_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedSplit_BitState)->ThreadRange(1, NumCPUs());

// Benchmark: splitting off leading number field but harder (ambiguous regexp).

void Parse1SplitHard(benchmark::State& state,
                     void (*run)(benchmark::State&, const char*,
                                 absl::string_view)) {
  run(state, "[0-9]+.(.*)", "650-253-0001");
  state.SetItemsProcessed(state.iterations());
}

void Parse_SplitHard_NFA(benchmark::State& state)         { Parse1SplitHard(state, Parse1NFA); }
void Parse_SplitHard_PCRE(benchmark::State& state)        { Parse1SplitHard(state, Parse1PCRE); }
void Parse_SplitHard_RE2(benchmark::State& state)         { Parse1SplitHard(state, Parse1RE2); }
void Parse_SplitHard_BitState(benchmark::State& state)    { Parse1SplitHard(state, Parse1BitState); }

#ifdef USEPCRE
BENCHMARK(Parse_SplitHard_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_SplitHard_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_SplitHard_BitState)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_SplitHard_NFA)->ThreadRange(1, NumCPUs());

void Parse_CachedSplitHard_NFA(benchmark::State& state)       { Parse1SplitHard(state, Parse1CachedNFA); }
void Parse_CachedSplitHard_PCRE(benchmark::State& state)      { Parse1SplitHard(state, Parse1CachedPCRE); }
void Parse_CachedSplitHard_RE2(benchmark::State& state)       { Parse1SplitHard(state, Parse1CachedRE2); }
void Parse_CachedSplitHard_BitState(benchmark::State& state)  { Parse1SplitHard(state, Parse1CachedBitState); }
void Parse_CachedSplitHard_Backtrack(benchmark::State& state) { Parse1SplitHard(state, Parse1CachedBacktrack); }

#ifdef USEPCRE
BENCHMARK(Parse_CachedSplitHard_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedSplitHard_RE2)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedSplitHard_BitState)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedSplitHard_NFA)->ThreadRange(1, NumCPUs());
BENCHMARK(Parse_CachedSplitHard_Backtrack)->ThreadRange(1, NumCPUs());

// Benchmark: Parse1SplitHard, big text, small match.

void Parse1SplitBig1(benchmark::State& state,
                     void (*run)(benchmark::State&, const char*,
                                 absl::string_view)) {
  std::string s;
  s.append(100000, 'x');
  s.append("650-253-0001");
  run(state, "[0-9]+.(.*)", s);
  state.SetItemsProcessed(state.iterations());
}

void Parse_CachedSplitBig1_PCRE(benchmark::State& state)      { Parse1SplitBig1(state, SearchParse1CachedPCRE); }
void Parse_CachedSplitBig1_RE2(benchmark::State& state)       { Parse1SplitBig1(state, SearchParse1CachedRE2); }

#ifdef USEPCRE
BENCHMARK(Parse_CachedSplitBig1_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedSplitBig1_RE2)->ThreadRange(1, NumCPUs());

// Benchmark: Parse1SplitHard, big text, big match.

void Parse1SplitBig2(benchmark::State& state,
                     void (*run)(benchmark::State&, const char*,
                                 absl::string_view)) {
  std::string s;
  s.append("650-253-");
  s.append(100000, '0');
  run(state, "[0-9]+.(.*)", s);
  state.SetItemsProcessed(state.iterations());
}

void Parse_CachedSplitBig2_PCRE(benchmark::State& state)      { Parse1SplitBig2(state, SearchParse1CachedPCRE); }
void Parse_CachedSplitBig2_RE2(benchmark::State& state)       { Parse1SplitBig2(state, SearchParse1CachedRE2); }

#ifdef USEPCRE
BENCHMARK(Parse_CachedSplitBig2_PCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(Parse_CachedSplitBig2_RE2)->ThreadRange(1, NumCPUs());

// Benchmark: measure time required to parse (but not execute)
// a simple regular expression.

void ParseRegexp(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    re->Decref();
  }
}

void SimplifyRegexp(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Regexp* sre = re->Simplify();
    ABSL_CHECK(sre);
    sre->Decref();
    re->Decref();
  }
}

void NullWalkRegexp(benchmark::State& state, const std::string& regexp) {
  Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
  ABSL_CHECK(re);
  for (auto _ : state) {
    re->NullWalk();
  }
  re->Decref();
}

void SimplifyCompileRegexp(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Regexp* sre = re->Simplify();
    ABSL_CHECK(sre);
    Prog* prog = sre->CompileToProg(0);
    ABSL_CHECK(prog);
    delete prog;
    sre->Decref();
    re->Decref();
  }
}

void CompileRegexp(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    delete prog;
    re->Decref();
  }
}

void CompileToProg(benchmark::State& state, const std::string& regexp) {
  Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
  ABSL_CHECK(re);
  for (auto _ : state) {
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    delete prog;
  }
  re->Decref();
}

void CompileByteMap(benchmark::State& state, const std::string& regexp) {
  Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
  ABSL_CHECK(re);
  Prog* prog = re->CompileToProg(0);
  ABSL_CHECK(prog);
  for (auto _ : state) {
    prog->ComputeByteMap();
  }
  delete prog;
  re->Decref();
}

void CompilePCRE(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    PCRE re(regexp, PCRE::UTF8);
    ABSL_CHECK_EQ(re.error(), "");
  }
}

void CompileRE2(benchmark::State& state, const std::string& regexp) {
  for (auto _ : state) {
    RE2 re(regexp);
    ABSL_CHECK_EQ(re.error(), "");
  }
}

void RunBuild(benchmark::State& state, const std::string& regexp,
              void (*run)(benchmark::State&, const std::string&)) {
  run(state, regexp);
  state.SetItemsProcessed(state.iterations());
}

}  // namespace re2

ABSL_FLAG(std::string, compile_regexp, "(.*)-(\\d+)-of-(\\d+)",
          "regexp for compile benchmarks");

namespace re2 {

void BM_PCRE_Compile(benchmark::State& state)             { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), CompilePCRE); }
void BM_Regexp_Parse(benchmark::State& state)             { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), ParseRegexp); }
void BM_Regexp_Simplify(benchmark::State& state)          { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), SimplifyRegexp); }
void BM_CompileToProg(benchmark::State& state)            { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), CompileToProg); }
void BM_CompileByteMap(benchmark::State& state)           { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), CompileByteMap); }
void BM_Regexp_Compile(benchmark::State& state)           { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), CompileRegexp); }
void BM_Regexp_SimplifyCompile(benchmark::State& state)   { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), SimplifyCompileRegexp); }
void BM_Regexp_NullWalk(benchmark::State& state)          { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), NullWalkRegexp); }
void BM_RE2_Compile(benchmark::State& state)              { RunBuild(state, absl::GetFlag(FLAGS_compile_regexp), CompileRE2); }

#ifdef USEPCRE
BENCHMARK(BM_PCRE_Compile)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(BM_Regexp_Parse)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_Regexp_Simplify)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_CompileToProg)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_CompileByteMap)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_Regexp_Compile)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_Regexp_SimplifyCompile)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_Regexp_NullWalk)->ThreadRange(1, NumCPUs());
BENCHMARK(BM_RE2_Compile)->ThreadRange(1, NumCPUs());

// Makes text of size nbytes, then calls run to search
// the text for regexp iters times.
void SearchPhone(benchmark::State& state, ParseImpl* search) {
  std::string s = RandomText(state.range(0));
  s.append("(650) 253-0001");
  search(state, "(\\d{3}-|\\(\\d{3}\\)\\s+)(\\d{3}-\\d{4})", s);
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void SearchPhone_CachedPCRE(benchmark::State& state) {
  SearchPhone(state, SearchParse2CachedPCRE);
}

void SearchPhone_CachedRE2(benchmark::State& state) {
  SearchPhone(state, SearchParse2CachedRE2);
}

#ifdef USEPCRE
BENCHMARK_RANGE(SearchPhone_CachedPCRE, 8, 16<<20)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK_RANGE(SearchPhone_CachedRE2, 8, 16<<20)->ThreadRange(1, NumCPUs());

/*
TODO(rsc): Make this work again.
void CacheFill(int iters, int n, SearchImpl *srch) {
  std::string s = DeBruijnString(n+1);
  std::string t;
  for (int i = n+1; i < 20; i++) {
    t = s + s;
    using std::swap;
    swap(s, t);
  }
  srch(iters, StringPrintf("0[01]{%d}$", n).c_str(), s,
       Prog::kUnanchored, true);
  SetBenchmarkBytesProcessed(static_cast<int64_t>(iters)*s.size());
}

void CacheFillPCRE(int i, int n) { CacheFill(i, n, SearchCachedPCRE); }
void CacheFillRE2(int i, int n)  { CacheFill(i, n, SearchCachedRE2); }
void CacheFillNFA(int i, int n)  { CacheFill(i, n, SearchCachedNFA); }
void CacheFillDFA(int i, int n)  { CacheFill(i, n, SearchCachedDFA); }

// BENCHMARK_WITH_ARG uses __LINE__ to generate distinct identifiers
// for the static BenchmarkRegisterer, which makes it unusable inside
// a macro like DO24 below.  MY_BENCHMARK_WITH_ARG uses the argument a
// to make the identifiers distinct (only possible when 'a' is a simple
// expression like 2, not like 1+1).
#define MY_BENCHMARK_WITH_ARG(n, a) \
  bool __benchmark_ ## n ## a =     \
    (new ::testing::Benchmark(#n, NewPermanentCallback(&n)))->ThreadRange(1, NumCPUs());

#define DO24(A, B) \
  A(B, 1);    A(B, 2);    A(B, 3);    A(B, 4);    A(B, 5);    A(B, 6);  \
  A(B, 7);    A(B, 8);    A(B, 9);    A(B, 10);   A(B, 11);   A(B, 12); \
  A(B, 13);   A(B, 14);   A(B, 15);   A(B, 16);   A(B, 17);   A(B, 18); \
  A(B, 19);   A(B, 20);   A(B, 21);   A(B, 22);   A(B, 23);   A(B, 24);

DO24(MY_BENCHMARK_WITH_ARG, CacheFillPCRE)
DO24(MY_BENCHMARK_WITH_ARG, CacheFillNFA)
DO24(MY_BENCHMARK_WITH_ARG, CacheFillRE2)
DO24(MY_BENCHMARK_WITH_ARG, CacheFillDFA)

#undef DO24
#undef MY_BENCHMARK_WITH_ARG
*/

////////////////////////////////////////////////////////////////////////
//
// Implementation routines.  Sad that there are so many,
// but all the interfaces are slightly different.

// Runs implementation to search for regexp in text, iters times.
// Expect_match says whether the regexp should be found.
// Anchored says whether to run an anchored search.

void SearchDFA(benchmark::State& state, const char* regexp,
               absl::string_view text, Prog::Anchor anchor,
               bool expect_match) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    bool failed = false;
    ABSL_CHECK_EQ(prog->SearchDFA(text, absl::string_view(), anchor,
                                  Prog::kFirstMatch, NULL, &failed, NULL),
                  expect_match);
    ABSL_CHECK(!failed);
    delete prog;
    re->Decref();
  }
}

void SearchNFA(benchmark::State& state, const char* regexp,
               absl::string_view text, Prog::Anchor anchor,
               bool expect_match) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK_EQ(prog->SearchNFA(text, absl::string_view(), anchor,
                                  Prog::kFirstMatch, NULL, 0),
                  expect_match);
    delete prog;
    re->Decref();
  }
}

void SearchOnePass(benchmark::State& state, const char* regexp,
                   absl::string_view text, Prog::Anchor anchor,
                   bool expect_match) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->IsOnePass());
    ABSL_CHECK_EQ(
        prog->SearchOnePass(text, text, anchor, Prog::kFirstMatch, NULL, 0),
        expect_match);
    delete prog;
    re->Decref();
  }
}

void SearchBitState(benchmark::State& state, const char* regexp,
                    absl::string_view text, Prog::Anchor anchor,
                    bool expect_match) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->CanBitState());
    ABSL_CHECK_EQ(
        prog->SearchBitState(text, text, anchor, Prog::kFirstMatch, NULL, 0),
        expect_match);
    delete prog;
    re->Decref();
  }
}

void SearchPCRE(benchmark::State& state, const char* regexp,
                absl::string_view text, Prog::Anchor anchor,
                bool expect_match) {
  for (auto _ : state) {
    PCRE re(regexp, PCRE::UTF8);
    ABSL_CHECK_EQ(re.error(), "");
    if (anchor == Prog::kAnchored) {
      ABSL_CHECK_EQ(PCRE::FullMatch(text, re), expect_match);
    } else {
      ABSL_CHECK_EQ(PCRE::PartialMatch(text, re), expect_match);
    }
  }
}

void SearchRE2(benchmark::State& state, const char* regexp,
               absl::string_view text, Prog::Anchor anchor,
               bool expect_match) {
  for (auto _ : state) {
    RE2 re(regexp);
    ABSL_CHECK_EQ(re.error(), "");
    if (anchor == Prog::kAnchored) {
      ABSL_CHECK_EQ(RE2::FullMatch(text, re), expect_match);
    } else {
      ABSL_CHECK_EQ(RE2::PartialMatch(text, re), expect_match);
    }
  }
}

// SearchCachedXXX is like SearchXXX but only does the
// regexp parsing and compiling once.  This lets us measure
// search time without the per-regexp overhead.

Prog* GetCachedProg(const char* regexp) {
  static auto& mutex = *new absl::Mutex;
  absl::MutexLock lock(&mutex);
  static auto& cache = *new absl::flat_hash_map<std::string, Prog*>;
  Prog* prog = cache[regexp];
  if (prog == NULL) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    prog = re->CompileToProg(int64_t{1}<<31);  // mostly for the DFA
    ABSL_CHECK(prog);
    cache[regexp] = prog;
    re->Decref();
    // We must call this here - while we have exclusive access.
    prog->IsOnePass();
  }
  return prog;
}

PCRE* GetCachedPCRE(const char* regexp) {
  static auto& mutex = *new absl::Mutex;
  absl::MutexLock lock(&mutex);
  static auto& cache = *new absl::flat_hash_map<std::string, PCRE*>;
  PCRE* re = cache[regexp];
  if (re == NULL) {
    re = new PCRE(regexp, PCRE::UTF8);
    ABSL_CHECK_EQ(re->error(), "");
    cache[regexp] = re;
  }
  return re;
}

RE2* GetCachedRE2(const char* regexp) {
  static auto& mutex = *new absl::Mutex;
  absl::MutexLock lock(&mutex);
  static auto& cache = *new absl::flat_hash_map<std::string, RE2*>;
  RE2* re = cache[regexp];
  if (re == NULL) {
    re = new RE2(regexp);
    ABSL_CHECK_EQ(re->error(), "");
    cache[regexp] = re;
  }
  return re;
}

void SearchCachedDFA(benchmark::State& state, const char* regexp,
                     absl::string_view text, Prog::Anchor anchor,
                     bool expect_match) {
  Prog* prog = GetCachedProg(regexp);
  for (auto _ : state) {
    bool failed = false;
    ABSL_CHECK_EQ(prog->SearchDFA(text, absl::string_view(), anchor,
                             Prog::kFirstMatch, NULL, &failed, NULL),
             expect_match);
    ABSL_CHECK(!failed);
  }
}

void SearchCachedNFA(benchmark::State& state, const char* regexp,
                     absl::string_view text, Prog::Anchor anchor,
                     bool expect_match) {
  Prog* prog = GetCachedProg(regexp);
  for (auto _ : state) {
    ABSL_CHECK_EQ(prog->SearchNFA(text, absl::string_view(), anchor,
                             Prog::kFirstMatch, NULL, 0),
             expect_match);
  }
}

void SearchCachedOnePass(benchmark::State& state, const char* regexp,
                         absl::string_view text, Prog::Anchor anchor,
                         bool expect_match) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->IsOnePass());
  for (auto _ : state) {
    ABSL_CHECK_EQ(
        prog->SearchOnePass(text, text, anchor, Prog::kFirstMatch, NULL, 0),
        expect_match);
  }
}

void SearchCachedBitState(benchmark::State& state, const char* regexp,
                          absl::string_view text, Prog::Anchor anchor,
                          bool expect_match) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->CanBitState());
  for (auto _ : state) {
    ABSL_CHECK_EQ(
        prog->SearchBitState(text, text, anchor, Prog::kFirstMatch, NULL, 0),
        expect_match);
  }
}

void SearchCachedPCRE(benchmark::State& state, const char* regexp,
                      absl::string_view text, Prog::Anchor anchor,
                      bool expect_match) {
  PCRE& re = *GetCachedPCRE(regexp);
  for (auto _ : state) {
    if (anchor == Prog::kAnchored) {
      ABSL_CHECK_EQ(PCRE::FullMatch(text, re), expect_match);
    } else {
      ABSL_CHECK_EQ(PCRE::PartialMatch(text, re), expect_match);
    }
  }
}

void SearchCachedRE2(benchmark::State& state, const char* regexp,
                     absl::string_view text, Prog::Anchor anchor,
                     bool expect_match) {
  RE2& re = *GetCachedRE2(regexp);
  for (auto _ : state) {
    if (anchor == Prog::kAnchored) {
      ABSL_CHECK_EQ(RE2::FullMatch(text, re), expect_match);
    } else {
      ABSL_CHECK_EQ(RE2::PartialMatch(text, re), expect_match);
    }
  }
}

// Runs implementation to full match regexp against text,
// extracting three submatches.  Expects match always.

void Parse3NFA(benchmark::State& state, const char* regexp,
               absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    absl::string_view sp[4];  // 4 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchNFA(text, absl::string_view(), Prog::kAnchored,
                          Prog::kFullMatch, sp, 4));
    delete prog;
    re->Decref();
  }
}

void Parse3OnePass(benchmark::State& state, const char* regexp,
                   absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->IsOnePass());
    absl::string_view sp[4];  // 4 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored,
                                   Prog::kFullMatch, sp, 4));
    delete prog;
    re->Decref();
  }
}

void Parse3BitState(benchmark::State& state, const char* regexp,
                    absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->CanBitState());
    absl::string_view sp[4];  // 4 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchBitState(text, text, Prog::kAnchored,
                                    Prog::kFullMatch, sp, 4));
    delete prog;
    re->Decref();
  }
}

void Parse3Backtrack(benchmark::State& state, const char* regexp,
                     absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    absl::string_view sp[4];  // 4 because sp[0] is whole match.
    ABSL_CHECK(prog->UnsafeSearchBacktrack(text, text, Prog::kAnchored,
                                           Prog::kFullMatch, sp, 4));
    delete prog;
    re->Decref();
  }
}

void Parse3PCRE(benchmark::State& state, const char* regexp,
                absl::string_view text) {
  for (auto _ : state) {
    PCRE re(regexp, PCRE::UTF8);
    ABSL_CHECK_EQ(re.error(), "");
    absl::string_view sp1, sp2, sp3;
    ABSL_CHECK(PCRE::FullMatch(text, re, &sp1, &sp2, &sp3));
  }
}

void Parse3RE2(benchmark::State& state, const char* regexp,
               absl::string_view text) {
  for (auto _ : state) {
    RE2 re(regexp);
    ABSL_CHECK_EQ(re.error(), "");
    absl::string_view sp1, sp2, sp3;
    ABSL_CHECK(RE2::FullMatch(text, re, &sp1, &sp2, &sp3));
  }
}

void Parse3CachedNFA(benchmark::State& state, const char* regexp,
                     absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  absl::string_view sp[4];  // 4 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchNFA(text, absl::string_view(), Prog::kAnchored,
                          Prog::kFullMatch, sp, 4));
  }
}

void Parse3CachedOnePass(benchmark::State& state, const char* regexp,
                         absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->IsOnePass());
  absl::string_view sp[4];  // 4 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored,
                                   Prog::kFullMatch, sp, 4));
  }
}

void Parse3CachedBitState(benchmark::State& state, const char* regexp,
                          absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->CanBitState());
  absl::string_view sp[4];  // 4 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchBitState(text, text, Prog::kAnchored,
                                    Prog::kFullMatch, sp, 4));
  }
}

void Parse3CachedBacktrack(benchmark::State& state, const char* regexp,
                           absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  absl::string_view sp[4];  // 4 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->UnsafeSearchBacktrack(text, text, Prog::kAnchored,
                                           Prog::kFullMatch, sp, 4));
  }
}

void Parse3CachedPCRE(benchmark::State& state, const char* regexp,
                      absl::string_view text) {
  PCRE& re = *GetCachedPCRE(regexp);
  absl::string_view sp1, sp2, sp3;
  for (auto _ : state) {
    ABSL_CHECK(PCRE::FullMatch(text, re, &sp1, &sp2, &sp3));
  }
}

void Parse3CachedRE2(benchmark::State& state, const char* regexp,
                     absl::string_view text) {
  RE2& re = *GetCachedRE2(regexp);
  absl::string_view sp1, sp2, sp3;
  for (auto _ : state) {
    ABSL_CHECK(RE2::FullMatch(text, re, &sp1, &sp2, &sp3));
  }
}

// Runs implementation to full match regexp against text,
// extracting three submatches.  Expects match always.

void Parse1NFA(benchmark::State& state, const char* regexp,
               absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    absl::string_view sp[2];  // 2 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchNFA(text, absl::string_view(), Prog::kAnchored,
                               Prog::kFullMatch, sp, 2));
    delete prog;
    re->Decref();
  }
}

void Parse1OnePass(benchmark::State& state, const char* regexp,
                   absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->IsOnePass());
    absl::string_view sp[2];  // 2 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored,
                                   Prog::kFullMatch, sp, 2));
    delete prog;
    re->Decref();
  }
}

void Parse1BitState(benchmark::State& state, const char* regexp,
                    absl::string_view text) {
  for (auto _ : state) {
    Regexp* re = Regexp::Parse(regexp, Regexp::LikePerl, NULL);
    ABSL_CHECK(re);
    Prog* prog = re->CompileToProg(0);
    ABSL_CHECK(prog);
    ABSL_CHECK(prog->CanBitState());
    absl::string_view sp[2];  // 2 because sp[0] is whole match.
    ABSL_CHECK(prog->SearchBitState(text, text, Prog::kAnchored,
                                    Prog::kFullMatch, sp, 2));
    delete prog;
    re->Decref();
  }
}

void Parse1PCRE(benchmark::State& state, const char* regexp,
                absl::string_view text) {
  for (auto _ : state) {
    PCRE re(regexp, PCRE::UTF8);
    ABSL_CHECK_EQ(re.error(), "");
    absl::string_view sp1;
    ABSL_CHECK(PCRE::FullMatch(text, re, &sp1));
  }
}

void Parse1RE2(benchmark::State& state, const char* regexp,
               absl::string_view text) {
  for (auto _ : state) {
    RE2 re(regexp);
    ABSL_CHECK_EQ(re.error(), "");
    absl::string_view sp1;
    ABSL_CHECK(RE2::FullMatch(text, re, &sp1));
  }
}

void Parse1CachedNFA(benchmark::State& state, const char* regexp,
                     absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  absl::string_view sp[2];  // 2 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchNFA(text, absl::string_view(), Prog::kAnchored,
                          Prog::kFullMatch, sp, 2));
  }
}

void Parse1CachedOnePass(benchmark::State& state, const char* regexp,
                         absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->IsOnePass());
  absl::string_view sp[2];  // 2 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchOnePass(text, text, Prog::kAnchored,
                                   Prog::kFullMatch, sp, 2));
  }
}

void Parse1CachedBitState(benchmark::State& state, const char* regexp,
                          absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  ABSL_CHECK(prog->CanBitState());
  absl::string_view sp[2];  // 2 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->SearchBitState(text, text, Prog::kAnchored,
                                    Prog::kFullMatch, sp, 2));
  }
}

void Parse1CachedBacktrack(benchmark::State& state, const char* regexp,
                           absl::string_view text) {
  Prog* prog = GetCachedProg(regexp);
  absl::string_view sp[2];  // 2 because sp[0] is whole match.
  for (auto _ : state) {
    ABSL_CHECK(prog->UnsafeSearchBacktrack(text, text, Prog::kAnchored,
                                           Prog::kFullMatch, sp, 2));
  }
}

void Parse1CachedPCRE(benchmark::State& state, const char* regexp,
                      absl::string_view text) {
  PCRE& re = *GetCachedPCRE(regexp);
  absl::string_view sp1;
  for (auto _ : state) {
    ABSL_CHECK(PCRE::FullMatch(text, re, &sp1));
  }
}

void Parse1CachedRE2(benchmark::State& state, const char* regexp,
                     absl::string_view text) {
  RE2& re = *GetCachedRE2(regexp);
  absl::string_view sp1;
  for (auto _ : state) {
    ABSL_CHECK(RE2::FullMatch(text, re, &sp1));
  }
}

void SearchParse2CachedPCRE(benchmark::State& state, const char* regexp,
                            absl::string_view text) {
  PCRE& re = *GetCachedPCRE(regexp);
  for (auto _ : state) {
    absl::string_view sp1, sp2;
    ABSL_CHECK(PCRE::PartialMatch(text, re, &sp1, &sp2));
  }
}

void SearchParse2CachedRE2(benchmark::State& state, const char* regexp,
                           absl::string_view text) {
  RE2& re = *GetCachedRE2(regexp);
  for (auto _ : state) {
    absl::string_view sp1, sp2;
    ABSL_CHECK(RE2::PartialMatch(text, re, &sp1, &sp2));
  }
}

void SearchParse1CachedPCRE(benchmark::State& state, const char* regexp,
                            absl::string_view text) {
  PCRE& re = *GetCachedPCRE(regexp);
  for (auto _ : state) {
    absl::string_view sp1;
    ABSL_CHECK(PCRE::PartialMatch(text, re, &sp1));
  }
}

void SearchParse1CachedRE2(benchmark::State& state, const char* regexp,
                           absl::string_view text) {
  RE2& re = *GetCachedRE2(regexp);
  for (auto _ : state) {
    absl::string_view sp1;
    ABSL_CHECK(RE2::PartialMatch(text, re, &sp1));
  }
}

void EmptyPartialMatchPCRE(benchmark::State& state) {
  PCRE re("");
  for (auto _ : state) {
    PCRE::PartialMatch("", re);
  }
}

void EmptyPartialMatchRE2(benchmark::State& state) {
  RE2 re("");
  for (auto _ : state) {
    RE2::PartialMatch("", re);
  }
}
#ifdef USEPCRE
BENCHMARK(EmptyPartialMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(EmptyPartialMatchRE2)->ThreadRange(1, NumCPUs());

void SimplePartialMatchPCRE(benchmark::State& state) {
  PCRE re("abcdefg");
  for (auto _ : state) {
    PCRE::PartialMatch("abcdefg", re);
  }
}

void SimplePartialMatchRE2(benchmark::State& state) {
  RE2 re("abcdefg");
  for (auto _ : state) {
    RE2::PartialMatch("abcdefg", re);
  }
}
#ifdef USEPCRE
BENCHMARK(SimplePartialMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(SimplePartialMatchRE2)->ThreadRange(1, NumCPUs());

static std::string http_text =
  "GET /asdfhjasdhfasdlfhasdflkjasdfkljasdhflaskdjhf"
  "alksdjfhasdlkfhasdlkjfhasdljkfhadsjklf HTTP/1.1";

void HTTPPartialMatchPCRE(benchmark::State& state) {
  absl::string_view a;
  PCRE re("(?-s)^(?:GET|POST) +([^ ]+) HTTP");
  for (auto _ : state) {
    PCRE::PartialMatch(http_text, re, &a);
  }
}

void HTTPPartialMatchRE2(benchmark::State& state) {
  absl::string_view a;
  RE2 re("(?-s)^(?:GET|POST) +([^ ]+) HTTP");
  for (auto _ : state) {
    RE2::PartialMatch(http_text, re, &a);
  }
}

#ifdef USEPCRE
BENCHMARK(HTTPPartialMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(HTTPPartialMatchRE2)->ThreadRange(1, NumCPUs());

static std::string smallhttp_text =
  "GET /abc HTTP/1.1";

void SmallHTTPPartialMatchPCRE(benchmark::State& state) {
  absl::string_view a;
  PCRE re("(?-s)^(?:GET|POST) +([^ ]+) HTTP");
  for (auto _ : state) {
    PCRE::PartialMatch(smallhttp_text, re, &a);
  }
}

void SmallHTTPPartialMatchRE2(benchmark::State& state) {
  absl::string_view a;
  RE2 re("(?-s)^(?:GET|POST) +([^ ]+) HTTP");
  for (auto _ : state) {
    RE2::PartialMatch(smallhttp_text, re, &a);
  }
}

#ifdef USEPCRE
BENCHMARK(SmallHTTPPartialMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(SmallHTTPPartialMatchRE2)->ThreadRange(1, NumCPUs());

void DotMatchPCRE(benchmark::State& state) {
  absl::string_view a;
  PCRE re("(?-s)^(.+)");
  for (auto _ : state) {
    PCRE::PartialMatch(http_text, re, &a);
  }
}

void DotMatchRE2(benchmark::State& state) {
  absl::string_view a;
  RE2 re("(?-s)^(.+)");
  for (auto _ : state) {
    RE2::PartialMatch(http_text, re, &a);
  }
}

#ifdef USEPCRE
BENCHMARK(DotMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(DotMatchRE2)->ThreadRange(1, NumCPUs());

void ASCIIMatchPCRE(benchmark::State& state) {
  absl::string_view a;
  PCRE re("(?-s)^([ -~]+)");
  for (auto _ : state) {
    PCRE::PartialMatch(http_text, re, &a);
  }
}

void ASCIIMatchRE2(benchmark::State& state) {
  absl::string_view a;
  RE2 re("(?-s)^([ -~]+)");
  for (auto _ : state) {
    RE2::PartialMatch(http_text, re, &a);
  }
}

#ifdef USEPCRE
BENCHMARK(ASCIIMatchPCRE)->ThreadRange(1, NumCPUs());
#endif
BENCHMARK(ASCIIMatchRE2)->ThreadRange(1, NumCPUs());

void FullMatchPCRE(benchmark::State& state, const char *regexp) {
  std::string s = RandomText(state.range(0));
  s += "ABCDEFGHIJ";
  PCRE re(regexp);
  for (auto _ : state) {
    ABSL_CHECK(PCRE::FullMatch(s, re));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void FullMatchRE2(benchmark::State& state, const char *regexp) {
  std::string s = RandomText(state.range(0));
  s += "ABCDEFGHIJ";
  RE2 re(regexp, RE2::Latin1);
  for (auto _ : state) {
    ABSL_CHECK(RE2::FullMatch(s, re));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}

void FullMatch_DotStar_CachedPCRE(benchmark::State& state) {
  FullMatchPCRE(state, "(?s).*");
}
void FullMatch_DotStar_CachedRE2(benchmark::State& state) {
  FullMatchRE2(state, "(?s).*");
}

void FullMatch_DotStarDollar_CachedPCRE(benchmark::State& state) {
  FullMatchPCRE(state, "(?s).*$");
}
void FullMatch_DotStarDollar_CachedRE2(benchmark::State& state) {
  FullMatchRE2(state, "(?s).*$");
}

void FullMatch_DotStarCapture_CachedPCRE(benchmark::State& state) {
  FullMatchPCRE(state, "(?s)((.*)()()($))");
}
void FullMatch_DotStarCapture_CachedRE2(benchmark::State& state) {
  FullMatchRE2(state, "(?s)((.*)()()($))");
}

#ifdef USEPCRE
BENCHMARK_RANGE(FullMatch_DotStar_CachedPCRE, 8, 2<<20);
#endif
BENCHMARK_RANGE(FullMatch_DotStar_CachedRE2,  8, 2<<20);

#ifdef USEPCRE
BENCHMARK_RANGE(FullMatch_DotStarDollar_CachedPCRE, 8, 2<<20);
#endif
BENCHMARK_RANGE(FullMatch_DotStarDollar_CachedRE2,  8, 2<<20);

#ifdef USEPCRE
BENCHMARK_RANGE(FullMatch_DotStarCapture_CachedPCRE, 8, 2<<20);
#endif
BENCHMARK_RANGE(FullMatch_DotStarCapture_CachedRE2,  8, 2<<20);

void PossibleMatchRangeCommon(benchmark::State& state, const char* regexp) {
  RE2 re(regexp);
  std::string min;
  std::string max;
  const int kMaxLen = 16;
  for (auto _ : state) {
    ABSL_CHECK(re.PossibleMatchRange(&min, &max, kMaxLen));
  }
}

void PossibleMatchRange_Trivial(benchmark::State& state) {
  PossibleMatchRangeCommon(state, ".*");
}
void PossibleMatchRange_Complex(benchmark::State& state) {
  PossibleMatchRangeCommon(state, "^abc[def]?[gh]{1,2}.*");
}
void PossibleMatchRange_Prefix(benchmark::State& state) {
  PossibleMatchRangeCommon(state, "^some_random_prefix.*");
}
void PossibleMatchRange_NoProg(benchmark::State& state) {
  PossibleMatchRangeCommon(state, "^some_random_string$");
}

BENCHMARK(PossibleMatchRange_Trivial);
BENCHMARK(PossibleMatchRange_Complex);
BENCHMARK(PossibleMatchRange_Prefix);
BENCHMARK(PossibleMatchRange_NoProg);

}  // namespace re2
