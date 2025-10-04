// Copyright 2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Regular expression engine tester -- test all the implementations against each other.

#include "re2/testing/tester.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "absl/base/macros.h"
#include "absl/flags/flag.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "re2/prog.h"
#include "re2/re2.h"
#include "re2/regexp.h"
#include "util/pcre.h"

ABSL_FLAG(bool, dump_prog, false, "dump regexp program");
ABSL_FLAG(bool, log_okay, false, "log successful runs");
ABSL_FLAG(bool, dump_rprog, false, "dump reversed regexp program");

ABSL_FLAG(int, max_regexp_failures, 100,
          "maximum number of regexp test failures (-1 = unlimited)");

ABSL_FLAG(std::string, regexp_engines, "",
          "pattern to select regexp engines to test");

namespace re2 {

enum {
  kMaxSubmatch = 1+16,  // $0...$16
};

const char* engine_names[kEngineMax] = {
  "Backtrack",
  "NFA",
  "DFA",
  "DFA1",
  "OnePass",
  "BitState",
  "RE2",
  "RE2a",
  "RE2b",
  "PCRE",
};

// Returns the name of the engine.
static const char* EngineName(Engine e) {
  ABSL_CHECK_GE(e, 0);
  ABSL_CHECK_LT(e, ABSL_ARRAYSIZE(engine_names));
  ABSL_CHECK(engine_names[e] != NULL);
  return engine_names[e];
}

// Returns bit mask of engines to use.
static uint32_t Engines() {
  static bool did_parse = false;
  static uint32_t cached_engines = 0;

  if (did_parse)
    return cached_engines;

  if (absl::GetFlag(FLAGS_regexp_engines).empty()) {
    cached_engines = ~0;
  } else {
    for (Engine i = static_cast<Engine>(0); i < kEngineMax; i++)
      if (absl::GetFlag(FLAGS_regexp_engines).find(EngineName(i)) != std::string::npos)
        cached_engines |= 1<<i;
  }

  if (cached_engines == 0)
    ABSL_LOG(INFO) << "Warning: no engines enabled.";
  if (!UsingPCRE)
    cached_engines &= ~(1<<kEnginePCRE);
  for (Engine i = static_cast<Engine>(0); i < kEngineMax; i++) {
    if (cached_engines & (1<<i))
      ABSL_LOG(INFO) << EngineName(i) << " enabled";
  }

  did_parse = true;
  return cached_engines;
}

// The result of running a match.
struct TestInstance::Result {
  Result()
      : skipped(false),
        matched(false),
        untrusted(false),
        have_submatch(false),
        have_submatch0(false) {
    ClearSubmatch();
  }

  void ClearSubmatch() {
    for (int i = 0; i < kMaxSubmatch; i++)
      submatch[i] = absl::string_view();
  }

  bool skipped;         // test skipped: wasn't applicable
  bool matched;         // found a match
  bool untrusted;       // don't really trust the answer
  bool have_submatch;   // computed all submatch info
  bool have_submatch0;  // computed just submatch[0]
  absl::string_view submatch[kMaxSubmatch];
};

typedef TestInstance::Result Result;

// Formats a single capture range s in text in the form (a,b)
// where a and b are the starting and ending offsets of s in text.
static std::string FormatCapture(absl::string_view text,
                                 absl::string_view s) {
  if (s.data() == NULL)
    return "(?,?)";
  return absl::StrFormat("(%d,%d)",
                         BeginPtr(s) - BeginPtr(text),
                         EndPtr(s) - BeginPtr(text));
}

// Returns whether text contains non-ASCII (>= 0x80) bytes.
static bool NonASCII(absl::string_view text) {
  for (size_t i = 0; i < text.size(); i++)
    if ((uint8_t)text[i] >= 0x80)
      return true;
  return false;
}

// Returns string representation of match kind.
static std::string FormatKind(Prog::MatchKind kind) {
  switch (kind) {
    case Prog::kFullMatch:
      return "full match";
    case Prog::kLongestMatch:
      return "longest match";
    case Prog::kFirstMatch:
      return "first match";
    case Prog::kManyMatch:
      return "many match";
  }
  return "???";
}

// Returns string representation of anchor kind.
static std::string FormatAnchor(Prog::Anchor anchor) {
  switch (anchor) {
    case Prog::kAnchored:
      return "anchored";
    case Prog::kUnanchored:
      return "unanchored";
  }
  return "???";
}

struct ParseMode {
  Regexp::ParseFlags parse_flags;
  std::string desc;
};

static const Regexp::ParseFlags single_line =
  Regexp::LikePerl;
static const Regexp::ParseFlags multi_line =
  static_cast<Regexp::ParseFlags>(Regexp::LikePerl & ~Regexp::OneLine);

static ParseMode parse_modes[] = {
  { single_line,                   "single-line"          },
  { single_line|Regexp::Latin1,    "single-line, latin1"  },
  { multi_line,                    "multiline"            },
  { multi_line|Regexp::NonGreedy,  "multiline, nongreedy" },
  { multi_line|Regexp::Latin1,     "multiline, latin1"    },
};

static std::string FormatMode(Regexp::ParseFlags flags) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(parse_modes); i++)
    if (parse_modes[i].parse_flags == flags)
      return parse_modes[i].desc;
  return absl::StrFormat("%#x", static_cast<uint32_t>(flags));
}

// Constructs and saves all the matching engines that
// will be required for the given tests.
TestInstance::TestInstance(absl::string_view regexp_str, Prog::MatchKind kind,
                           Regexp::ParseFlags flags)
  : regexp_str_(regexp_str),
    kind_(kind),
    flags_(flags),
    error_(false),
    regexp_(NULL),
    num_captures_(0),
    prog_(NULL),
    rprog_(NULL),
    re_(NULL),
    re2_(NULL) {

  ABSL_VLOG(1) << absl::CEscape(regexp_str);

  // Compile regexp to prog.
  // Always required - needed for backtracking (reference implementation).
  RegexpStatus status;
  regexp_ = Regexp::Parse(regexp_str, flags, &status);
  if (regexp_ == NULL) {
    ABSL_LOG(INFO) << "Cannot parse: " << absl::CEscape(regexp_str_)
                   << " mode: " << FormatMode(flags);
    error_ = true;
    return;
  }
  num_captures_ = regexp_->NumCaptures();
  prog_ = regexp_->CompileToProg(0);
  if (prog_ == NULL) {
    ABSL_LOG(INFO) << "Cannot compile: " << absl::CEscape(regexp_str_);
    error_ = true;
    return;
  }
  if (absl::GetFlag(FLAGS_dump_prog)) {
    ABSL_LOG(INFO) << "Prog for "
                   << " regexp "
                   << absl::CEscape(regexp_str_)
                   << " (" << FormatKind(kind_)
                   << ", " << FormatMode(flags_)
                   << ")\n"
                   << prog_->Dump();
  }

  // Compile regexp to reversed prog.  Only needed for DFA engines.
  if (Engines() & ((1<<kEngineDFA)|(1<<kEngineDFA1))) {
    rprog_ = regexp_->CompileToReverseProg(0);
    if (rprog_ == NULL) {
      ABSL_LOG(INFO) << "Cannot reverse compile: "
                     << absl::CEscape(regexp_str_);
      error_ = true;
      return;
    }
    if (absl::GetFlag(FLAGS_dump_rprog))
      ABSL_LOG(INFO) << rprog_->Dump();
  }

  // Create re string that will be used for RE and RE2.
  std::string re = std::string(regexp_str);
  // Accomodate flags.
  // Regexp::Latin1 will be accomodated below.
  if (!(flags & Regexp::OneLine))
    re = "(?m)" + re;
  if (flags & Regexp::NonGreedy)
    re = "(?U)" + re;
  if (flags & Regexp::DotNL)
    re = "(?s)" + re;

  // Compile regexp to RE2.
  if (Engines() & ((1<<kEngineRE2)|(1<<kEngineRE2a)|(1<<kEngineRE2b))) {
    RE2::Options options;
    if (flags & Regexp::Latin1)
      options.set_encoding(RE2::Options::EncodingLatin1);
    if (kind_ == Prog::kLongestMatch)
      options.set_longest_match(true);
    re2_ = new RE2(re, options);
    if (!re2_->error().empty()) {
      ABSL_LOG(INFO) << "Cannot RE2: " << absl::CEscape(re);
      error_ = true;
      return;
    }
  }

  // Compile regexp to RE.
  // PCRE as exposed by the RE interface isn't always usable.
  // 1. It disagrees about handling of empty-string reptitions
  //    like matching (a*)* against "b".  PCRE treats the (a*) as
  //    occurring once, while we treat it as occurring not at all.
  // 2. It treats $ as this weird thing meaning end of string
  //    or before the \n at the end of the string.
  // 3. It doesn't implement POSIX leftmost-longest matching.
  // 4. It lets \s match vertical tab.
  // MimicsPCRE() detects 1 and 2.
  if ((Engines() & (1<<kEnginePCRE)) && regexp_->MimicsPCRE() &&
      kind_ != Prog::kLongestMatch) {
    PCRE_Options o;
    o.set_option(PCRE::UTF8);
    if (flags & Regexp::Latin1)
      o.set_option(PCRE::None);
    // PCRE has interface bug keeping us from finding $0, so
    // add one more layer of parens.
    re_ = new PCRE("("+re+")", o);
    if (!re_->error().empty()) {
      ABSL_LOG(INFO) << "Cannot PCRE: " << absl::CEscape(re);
      error_ = true;
      return;
    }
  }
}

TestInstance::~TestInstance() {
  if (regexp_)
    regexp_->Decref();
  delete prog_;
  delete rprog_;
  delete re_;
  delete re2_;
}

// Runs a single search using the named engine type.
// This interface hides all the irregularities of the various
// engine interfaces from the rest of this file.
void TestInstance::RunSearch(Engine type, absl::string_view orig_text,
                             absl::string_view orig_context,
                             Prog::Anchor anchor, Result* result) {
  if (regexp_ == NULL) {
    result->skipped = true;
    return;
  }
  int nsubmatch = 1 + num_captures_;  // NumCaptures doesn't count $0
  if (nsubmatch > kMaxSubmatch)
    nsubmatch = kMaxSubmatch;

  absl::string_view text = orig_text;
  absl::string_view context = orig_context;

  switch (type) {
    default:
      ABSL_LOG(FATAL) << "Bad RunSearch type: " << (int)type;

    case kEngineBacktrack:
      if (prog_ == NULL) {
        result->skipped = true;
        break;
      }
      result->matched =
        prog_->UnsafeSearchBacktrack(text, context, anchor, kind_,
                                     result->submatch, nsubmatch);
      result->have_submatch = true;
      break;

    case kEngineNFA:
      if (prog_ == NULL) {
        result->skipped = true;
        break;
      }
      result->matched =
        prog_->SearchNFA(text, context, anchor, kind_,
                        result->submatch, nsubmatch);
      result->have_submatch = true;
      break;

    case kEngineDFA:
      if (prog_ == NULL) {
        result->skipped = true;
        break;
      }
      result->matched = prog_->SearchDFA(text, context, anchor, kind_, NULL,
                                         &result->skipped, NULL);
      break;

    case kEngineDFA1:
      if (prog_ == NULL || rprog_ == NULL) {
        result->skipped = true;
        break;
      }
      result->matched =
        prog_->SearchDFA(text, context, anchor, kind_, result->submatch,
                         &result->skipped, NULL);
      // If anchored, no need for second run,
      // but do it anyway to find more bugs.
      if (result->matched) {
        if (!rprog_->SearchDFA(result->submatch[0], context,
                               Prog::kAnchored, Prog::kLongestMatch,
                               result->submatch,
                               &result->skipped, NULL)) {
          ABSL_LOG(ERROR) << "Reverse DFA inconsistency: "
                          << absl::CEscape(regexp_str_)
                          << " on " << absl::CEscape(text);
          result->matched = false;
        }
      }
      result->have_submatch0 = true;
      break;

    case kEngineOnePass:
      if (prog_ == NULL ||
          !prog_->IsOnePass() ||
          anchor == Prog::kUnanchored ||
          nsubmatch > Prog::kMaxOnePassCapture) {
        result->skipped = true;
        break;
      }
      result->matched = prog_->SearchOnePass(text, context, anchor, kind_,
                                      result->submatch, nsubmatch);
      result->have_submatch = true;
      break;

    case kEngineBitState:
      if (prog_ == NULL ||
          !prog_->CanBitState()) {
        result->skipped = true;
        break;
      }
      result->matched = prog_->SearchBitState(text, context, anchor, kind_,
                                              result->submatch, nsubmatch);
      result->have_submatch = true;
      break;

    case kEngineRE2:
    case kEngineRE2a:
    case kEngineRE2b: {
      if (!re2_ || EndPtr(text) != EndPtr(context)) {
        result->skipped = true;
        break;
      }

      RE2::Anchor re_anchor;
      if (anchor == Prog::kAnchored)
        re_anchor = RE2::ANCHOR_START;
      else
        re_anchor = RE2::UNANCHORED;
      if (kind_ == Prog::kFullMatch)
        re_anchor = RE2::ANCHOR_BOTH;

      result->matched = re2_->Match(
          context,
          static_cast<size_t>(BeginPtr(text) - BeginPtr(context)),
          static_cast<size_t>(EndPtr(text) - BeginPtr(context)),
          re_anchor,
          result->submatch,
          nsubmatch);
      result->have_submatch = nsubmatch > 0;
      break;
    }

    case kEnginePCRE: {
      if (!re_ || BeginPtr(text) != BeginPtr(context) ||
          EndPtr(text) != EndPtr(context)) {
        result->skipped = true;
        break;
      }

      // In Perl/PCRE, \v matches any character considered vertical
      // whitespace, not just vertical tab. Regexp::MimicsPCRE() is
      // unable to handle all cases of this, unfortunately, so just
      // catch them here. :(
      if (regexp_str_.find("\\v") != absl::string_view::npos &&
          (text.find('\n') != absl::string_view::npos ||
           text.find('\f') != absl::string_view::npos ||
           text.find('\r') != absl::string_view::npos)) {
        result->skipped = true;
        break;
      }

      // PCRE 8.34 or so started allowing vertical tab to match \s,
      // following a change made in Perl 5.18. RE2 does not.
      if ((regexp_str_.find("\\s") != absl::string_view::npos ||
           regexp_str_.find("\\S") != absl::string_view::npos) &&
          text.find('\v') != absl::string_view::npos) {
        result->skipped = true;
        break;
      }

      const PCRE::Arg **argptr = new const PCRE::Arg*[nsubmatch];
      PCRE::Arg *a = new PCRE::Arg[nsubmatch];
      for (int i = 0; i < nsubmatch; i++) {
        a[i] = PCRE::Arg(&result->submatch[i]);
        argptr[i] = &a[i];
      }
      size_t consumed;
      PCRE::Anchor pcre_anchor;
      if (anchor == Prog::kAnchored)
        pcre_anchor = PCRE::ANCHOR_START;
      else
        pcre_anchor = PCRE::UNANCHORED;
      if (kind_ == Prog::kFullMatch)
        pcre_anchor = PCRE::ANCHOR_BOTH;
      re_->ClearHitLimit();
      result->matched =
        re_->DoMatch(text,
                     pcre_anchor,
                     &consumed,
                     argptr, nsubmatch);
      if (re_->HitLimit()) {
        result->untrusted = true;
        delete[] argptr;
        delete[] a;
        break;
      }
      result->have_submatch = true;
      delete[] argptr;
      delete[] a;
      break;
    }
  }

  if (!result->matched)
    result->ClearSubmatch();
}

// Checks whether r is okay given that correct is the right answer.
// Specifically, r's answers have to match (but it doesn't have to
// claim to have all the answers).
static bool ResultOkay(const Result& r, const Result& correct) {
  if (r.skipped)
    return true;
  if (r.matched != correct.matched)
    return false;
  if (r.have_submatch || r.have_submatch0) {
    for (int i = 0; i < kMaxSubmatch; i++) {
      if (correct.submatch[i].data() != r.submatch[i].data() ||
          correct.submatch[i].size() != r.submatch[i].size())
        return false;
      if (!r.have_submatch)
        break;
    }
  }
  return true;
}

// Runs a single test.
bool TestInstance::RunCase(absl::string_view text, absl::string_view context,
                           Prog::Anchor anchor) {
  // Backtracking is the gold standard.
  Result correct;
  RunSearch(kEngineBacktrack, text, context, anchor, &correct);
  if (correct.skipped) {
    if (regexp_ == NULL)
      return true;
    ABSL_LOG(ERROR) << "Skipped backtracking! " << absl::CEscape(regexp_str_)
                    << " " << FormatMode(flags_);
    return false;
  }
  ABSL_VLOG(1) << "Try: regexp " << absl::CEscape(regexp_str_)
               << " text " << absl::CEscape(text)
               << " (" << FormatKind(kind_)
               << ", " << FormatAnchor(anchor)
               << ", " << FormatMode(flags_)
               << ")";

  // Compare the others.
  bool all_okay = true;
  for (Engine i = kEngineBacktrack+1; i < kEngineMax; i++) {
    if (!(Engines() & (1<<i)))
      continue;

    Result r;
    RunSearch(i, text, context, anchor, &r);
    if (ResultOkay(r, correct)) {
      if (absl::GetFlag(FLAGS_log_okay))
        LogMatch(r.skipped ? "Skipped: " : "Okay: ", i, text, context, anchor);
      continue;
    }

    // We disagree with PCRE on the meaning of some Unicode matches.
    // In particular, we treat non-ASCII UTF-8 as non-word characters.
    // We also treat "empty" character sets like [^\w\W] as being
    // impossible to match, while PCRE apparently excludes some code
    // points (e.g., 0x0080) from both \w and \W.
    if (i == kEnginePCRE && NonASCII(text))
      continue;

    if (!r.untrusted)
      all_okay = false;

    LogMatch(r.untrusted ? "(Untrusted) Mismatch: " : "Mismatch: ", i, text,
             context, anchor);
    if (r.matched != correct.matched) {
      if (r.matched) {
        ABSL_LOG(INFO) << "   Should not match (but does).";
      } else {
        ABSL_LOG(INFO) << "   Should match (but does not).";
        continue;
      }
    }
    for (int i = 0; i < 1+num_captures_; i++) {
      if (r.submatch[i].data() != correct.submatch[i].data() ||
          r.submatch[i].size() != correct.submatch[i].size()) {
        ABSL_LOG(INFO) <<
          absl::StrFormat("   $%d: should be %s is %s",
                          i,
                          FormatCapture(text, correct.submatch[i]),
                          FormatCapture(text, r.submatch[i]));
      } else {
        ABSL_LOG(INFO) <<
          absl::StrFormat("   $%d: %s ok", i,
                          FormatCapture(text, r.submatch[i]));
      }
    }
  }

  if (!all_okay) {
    // This will be initialised once (after flags have been initialised)
    // and that is desirable because we want to enforce a global limit.
    static int max_regexp_failures = absl::GetFlag(FLAGS_max_regexp_failures);
    if (max_regexp_failures > 0 && --max_regexp_failures == 0)
      ABSL_LOG(QFATAL) << "Too many regexp failures.";
  }

  return all_okay;
}

void TestInstance::LogMatch(const char* prefix, Engine e,
                            absl::string_view text, absl::string_view context,
                            Prog::Anchor anchor) {
  ABSL_LOG(INFO) << prefix
    << EngineName(e)
    << " regexp "
    << absl::CEscape(regexp_str_)
    << " "
    << absl::CEscape(regexp_->ToString())
    << " text "
    << absl::CEscape(text)
    << " ("
    << BeginPtr(text) - BeginPtr(context)
    << ","
    << EndPtr(text) - BeginPtr(context)
    << ") of context "
    << absl::CEscape(context)
    << " (" << FormatKind(kind_)
    << ", " << FormatAnchor(anchor)
    << ", " << FormatMode(flags_)
    << ")";
}

static Prog::MatchKind kinds[] = {
  Prog::kFirstMatch,
  Prog::kLongestMatch,
  Prog::kFullMatch,
};

// Test all possible match kinds and parse modes.
Tester::Tester(absl::string_view regexp) {
  error_ = false;
  for (size_t i = 0; i < ABSL_ARRAYSIZE(kinds); i++) {
    for (size_t j = 0; j < ABSL_ARRAYSIZE(parse_modes); j++) {
      TestInstance* t = new TestInstance(regexp, kinds[i],
                                         parse_modes[j].parse_flags);
      error_ |= t->error();
      v_.push_back(t);
    }
  }
}

Tester::~Tester() {
  for (size_t i = 0; i < v_.size(); i++)
    delete v_[i];
}

bool Tester::TestCase(absl::string_view text, absl::string_view context,
                      Prog::Anchor anchor) {
  bool okay = true;
  for (size_t i = 0; i < v_.size(); i++)
    okay &= (!v_[i]->error() && v_[i]->RunCase(text, context, anchor));
  return okay;
}

static Prog::Anchor anchors[] = {
  Prog::kAnchored,
  Prog::kUnanchored
};

bool Tester::TestInput(absl::string_view text) {
  bool okay = TestInputInContext(text, text);
  if (!text.empty()) {
    absl::string_view sp;
    sp = text;
    sp.remove_prefix(1);
    okay &= TestInputInContext(sp, text);
    sp = text;
    sp.remove_suffix(1);
    okay &= TestInputInContext(sp, text);
  }
  return okay;
}

bool Tester::TestInputInContext(absl::string_view text,
                                absl::string_view context) {
  bool okay = true;
  for (size_t i = 0; i < ABSL_ARRAYSIZE(anchors); i++)
    okay &= TestCase(text, context, anchors[i]);
  return okay;
}

bool TestRegexpOnText(absl::string_view regexp,
                      absl::string_view text) {
  Tester t(regexp);
  return t.TestInput(text);
}

}  // namespace re2
