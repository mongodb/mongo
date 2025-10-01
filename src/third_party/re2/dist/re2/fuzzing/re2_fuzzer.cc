// Copyright 2016 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <fuzzer/FuzzedDataProvider.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "re2/filtered_re2.h"
#include "re2/re2.h"
#include "re2/regexp.h"
#include "re2/set.h"
#include "re2/walker-inl.h"

// NOT static, NOT signed.
uint8_t dummy = 0;

// Walks kRegexpConcat and kRegexpAlternate subexpressions
// to determine their maximum length.
class SubexpressionWalker : public re2::Regexp::Walker<int> {
 public:
  SubexpressionWalker() = default;
  ~SubexpressionWalker() override = default;

  int PostVisit(re2::Regexp* re, int parent_arg, int pre_arg,
                int* child_args, int nchild_args) override {
    switch (re->op()) {
      case re2::kRegexpConcat:
      case re2::kRegexpAlternate: {
        int max = nchild_args;
        for (int i = 0; i < nchild_args; i++)
          max = std::max(max, child_args[i]);
        return max;
      }

      default:
        break;
    }
    return -1;
  }

  // Should never be called: we use Walk(), not WalkExponential().
  int ShortVisit(re2::Regexp* re, int parent_arg) override {
    return parent_arg;
  }

 private:
  SubexpressionWalker(const SubexpressionWalker&) = delete;
  SubexpressionWalker& operator=(const SubexpressionWalker&) = delete;
};

// Walks substrings (i.e. kRegexpLiteralString subexpressions)
// to determine their maximum length... in runes, but avoiding
// overheads due to UTF-8 encoding is worthwhile when fuzzing.
class SubstringWalker : public re2::Regexp::Walker<int> {
 public:
  SubstringWalker() = default;
  ~SubstringWalker() override = default;

  int PostVisit(re2::Regexp* re, int parent_arg, int pre_arg,
                int* child_args, int nchild_args) override {
    switch (re->op()) {
      case re2::kRegexpConcat:
      case re2::kRegexpAlternate:
      case re2::kRegexpStar:
      case re2::kRegexpPlus:
      case re2::kRegexpQuest:
      case re2::kRegexpRepeat:
      case re2::kRegexpCapture: {
        int max = -1;
        for (int i = 0; i < nchild_args; i++)
          max = std::max(max, child_args[i]);
        return max;
      }

      case re2::kRegexpLiteralString:
        return re->nrunes();

      default:
        break;
    }
    return -1;
  }

  // Should never be called: we use Walk(), not WalkExponential().
  int ShortVisit(re2::Regexp* re, int parent_arg) override {
    return parent_arg;
  }

 private:
  SubstringWalker(const SubstringWalker&) = delete;
  SubstringWalker& operator=(const SubstringWalker&) = delete;
};

void TestOneInput(absl::string_view pattern, const RE2::Options& options,
                  RE2::Anchor anchor, absl::string_view text) {
  // Crudely limit the use of ., \p, \P, \d, \D, \s, \S, \w and \W.
  // Otherwise, we will waste time on inputs that have long runs of various
  // character classes. The fuzzer has shown itself to be easily capable of
  // generating such patterns that fall within the other limits, but result
  // in timeouts nonetheless. The marginal cost is high - even more so when
  // counted repetition is involved - whereas the marginal benefit is zero.
  // Crudely limit the use of 'k', 'K', 's' and 'S' too because they become
  // three-element character classes when case-insensitive and using UTF-8.
  // TODO(junyer): Handle [[:alnum:]] et al. when they start to cause pain.
  int char_class = 0;
  int backslash_p = 0;  // very expensive, so handle specially
  for (size_t i = 0; i < pattern.size(); i++) {
    if (pattern[i] == '.' ||
        pattern[i] == 'k' || pattern[i] == 'K' ||
        pattern[i] == 's' || pattern[i] == 'S')
      char_class++;
    if (pattern[i] != '\\')
      continue;
    i++;
    if (i >= pattern.size())
      break;
    if (pattern[i] == 'p' || pattern[i] == 'P' ||
        pattern[i] == 'd' || pattern[i] == 'D' ||
        pattern[i] == 's' || pattern[i] == 'S' ||
        pattern[i] == 'w' || pattern[i] == 'W')
      char_class++;
    if (pattern[i] == 'p' || pattern[i] == 'P')
      backslash_p++;
  }
  if (char_class > 9)
    return;
  if (backslash_p > 1)
    return;

  // Iterate just once when fuzzing. Otherwise, we easily get bogged down
  // and coverage is unlikely to improve despite significant expense.
  RE2::FUZZING_ONLY_set_maximum_global_replace_count(1);
  // The default is 1000. Even 100 turned out to be too generous
  // for fuzzing, empirically speaking, so let's try 10 instead.
  re2::Regexp::FUZZING_ONLY_set_maximum_repeat_count(10);

  RE2 re(pattern, options);
  if (!re.ok())
    return;

  // Don't waste time fuzzing programs with large subexpressions.
  // They can cause bug reports due to fuzzer timeouts. And they
  // aren't interesting for fuzzing purposes.
  if (SubexpressionWalker().Walk(re.Regexp(), -1) > 9)
    return;

  // Don't waste time fuzzing programs with large substrings.
  // They can cause bug reports due to fuzzer timeouts when they
  // are repetitions (e.g. hundreds of NUL bytes) and matching is
  // unanchored. And they aren't interesting for fuzzing purposes.
  if (SubstringWalker().Walk(re.Regexp(), -1) > 9)
    return;

  // Don't waste time fuzzing high-size programs.
  // They can cause bug reports due to fuzzer timeouts.
  int size = re.ProgramSize();
  if (size > 9999)
    return;
  int rsize = re.ReverseProgramSize();
  if (rsize > 9999)
    return;

  // Don't waste time fuzzing high-fanout programs.
  // They can cause bug reports due to fuzzer timeouts.
  std::vector<int> histogram;
  int fanout = re.ProgramFanout(&histogram);
  if (fanout > 9)
    return;
  int rfanout = re.ReverseProgramFanout(&histogram);
  if (rfanout > 9)
    return;

  if (re.NumberOfCapturingGroups() == 0) {
    // Avoid early return due to too many arguments.
    absl::string_view sp = text;
    RE2::FullMatch(sp, re);
    RE2::PartialMatch(sp, re);
    RE2::Consume(&sp, re);
    sp = text;  // Reset.
    RE2::FindAndConsume(&sp, re);
  } else {
    // Okay, we have at least one capturing group...
    // Try conversion for variously typed arguments.
    absl::string_view sp = text;
    short s;
    RE2::FullMatch(sp, re, &s);
    long l;
    RE2::PartialMatch(sp, re, &l);
    float f;
    RE2::Consume(&sp, re, &f);
    sp = text;  // Reset.
    double d;
    RE2::FindAndConsume(&sp, re, &d);
  }

  std::string s = std::string(text);
  RE2::Replace(&s, re, "");
  s = std::string(text);  // Reset.
  RE2::GlobalReplace(&s, re, "");

  std::string min, max;
  re.PossibleMatchRange(&min, &max, /*maxlen=*/9);

  // Exercise some other API functionality.
  dummy += re.NamedCapturingGroups().size();
  dummy += re.CapturingGroupNames().size();
  dummy += RE2::QuoteMeta(pattern).size();
  dummy += re.Regexp()->ToString().size();

  RE2::Set set(options, anchor);
  int index = set.Add(pattern, /*error=*/NULL);  // -1 on error
  if (index != -1 && set.Compile()) {
    std::vector<int> matches;
    set.Match(text, &matches);
  }

  re2::FilteredRE2 filter;
  index = -1;  // not clobbered on error
  filter.Add(pattern, options, &index);
  if (index != -1) {
    std::vector<std::string> atoms;
    filter.Compile(&atoms);
    // Pretend that all atoms match, which
    // triggers the AND-OR tree maximally.
    std::vector<int> matched_atoms;
    matched_atoms.reserve(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i)
      matched_atoms.push_back(static_cast<int>(i));
    std::vector<int> matches;
    filter.AllMatches(text, matched_atoms, &matches);
  }
}

// Entry point for libFuzzer.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // An input larger than 4 KiB probably isn't interesting. (This limit
  // allows for fdp.ConsumeRandomLengthString()'s backslash behaviour.)
  if (size == 0 || size > 4096)
    return 0;

  FuzzedDataProvider fdp(data, size);

  // The convention here is that fdp.ConsumeBool() returning false sets
  // the default value whereas returning true sets the alternate value:
  // most options default to false and so can be set directly; encoding
  // defaults to UTF-8; case_sensitive defaults to true. We do NOT want
  // to log errors. max_mem is 64 MiB because we can afford to use more
  // RAM in exchange for (hopefully) faster fuzzing.
  RE2::Options options;
  options.set_encoding(fdp.ConsumeBool() ? RE2::Options::EncodingLatin1
                                         : RE2::Options::EncodingUTF8);
  options.set_posix_syntax(fdp.ConsumeBool());
  options.set_longest_match(fdp.ConsumeBool());
  options.set_log_errors(false);
  options.set_max_mem(64 << 20);
  options.set_literal(fdp.ConsumeBool());
  options.set_never_nl(fdp.ConsumeBool());
  options.set_dot_nl(fdp.ConsumeBool());
  options.set_never_capture(fdp.ConsumeBool());
  options.set_case_sensitive(!fdp.ConsumeBool());
  options.set_perl_classes(fdp.ConsumeBool());
  options.set_word_boundary(fdp.ConsumeBool());
  options.set_one_line(fdp.ConsumeBool());

  // ConsumeEnum<RE2::Anchor>() would require RE2::Anchor to specify
  // kMaxValue, so just use PickValueInArray<RE2::Anchor>() instead.
  RE2::Anchor anchor = fdp.PickValueInArray<RE2::Anchor>({
      RE2::UNANCHORED,
      RE2::ANCHOR_START,
      RE2::ANCHOR_BOTH,
  });

  std::string pattern = fdp.ConsumeRandomLengthString(999);
  std::string text = fdp.ConsumeRandomLengthString(999);

  TestOneInput(pattern, options, anchor, text);
  return 0;
}
