// Copyright 2008 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Tested by search_test.cc, exhaustive_test.cc, tester.cc
//
// Prog::UnsafeSearchBacktrack is a backtracking regular expression search,
// except that it remembers where it has been, trading a lot of
// memory for a lot of time. It exists only for testing purposes.
//
// Let me repeat that.
//
// THIS CODE SHOULD NEVER BE USED IN PRODUCTION:
//   - It uses a ton of memory.
//   - It uses a ton of stack.
//   - It uses ABSL_CHECK() and ABSL_LOG(FATAL).
//   - It implements unanchored search by repeated anchored search.
//
// On the other hand, it is very simple and a good reference
// implementation for the more complicated regexp packages.
//
// In BUILD, this file is linked into the ":testing" library,
// not the main library, in order to make it harder to pick up
// accidentally.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "absl/base/macros.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/string_view.h"
#include "re2/pod_array.h"
#include "re2/prog.h"
#include "re2/regexp.h"

namespace re2 {

// Backtracker holds the state for a backtracking search.
//
// Excluding the search parameters, the main search state
// is just the "capture registers", which record, for the
// current execution, the string position at which each
// parenthesis was passed.  cap_[0] and cap_[1] are the
// left and right parenthesis in $0, cap_[2] and cap_[3] in $1, etc.
//
// To avoid infinite loops during backtracking on expressions
// like (a*)*, the visited_[] bitmap marks the (state, string-position)
// pairs that have already been explored and are thus not worth
// re-exploring if we get there via another path.  Modern backtracking
// libraries engineer their program representation differently, to make
// such infinite loops possible to avoid without keeping a giant visited_
// bitmap, but visited_ works fine for a reference implementation
// and it has the nice benefit of making the search run in linear time.
class Backtracker {
 public:
  explicit Backtracker(Prog* prog);

  bool Search(absl::string_view text, absl::string_view context, bool anchored,
              bool longest, absl::string_view* submatch, int nsubmatch);

 private:
  // Explores from instruction id at string position p looking for a match.
  // Returns true if found (so that caller can stop trying other possibilities).
  bool Visit(int id, const char* p);

  // Tries instruction id at string position p.
  // Returns true if a match is found.
  bool Try(int id, const char* p);

  // Search parameters
  Prog* prog_;                   // program being run
  absl::string_view text_;       // text being searched
  absl::string_view context_;    // greater context of text being searched
  bool anchored_;                // whether search is anchored at text.begin()
  bool longest_;                 // whether search wants leftmost-longest match
  bool endmatch_;                // whether search must end at text.end()
  absl::string_view* submatch_;  // submatches to fill in
  int nsubmatch_;                // # of submatches to fill in

  // Search state
  const char* cap_[64];         // capture registers
  PODArray<uint32_t> visited_;  // bitmap: (Inst*, char*) pairs visited

  Backtracker(const Backtracker&) = delete;
  Backtracker& operator=(const Backtracker&) = delete;
};

Backtracker::Backtracker(Prog* prog)
  : prog_(prog),
    anchored_(false),
    longest_(false),
    endmatch_(false),
    submatch_(NULL),
    nsubmatch_(0) {
}

// Runs a backtracking search.
bool Backtracker::Search(absl::string_view text, absl::string_view context,
                         bool anchored, bool longest,
                         absl::string_view* submatch, int nsubmatch) {
  text_ = text;
  context_ = context;
  if (context_.data() == NULL)
    context_ = text;
  if (prog_->anchor_start() && BeginPtr(text) > BeginPtr(context_))
    return false;
  if (prog_->anchor_end() && EndPtr(text) < EndPtr(context_))
    return false;
  anchored_ = anchored | prog_->anchor_start();
  longest_ = longest | prog_->anchor_end();
  endmatch_ = prog_->anchor_end();
  submatch_ = submatch;
  nsubmatch_ = nsubmatch;
  ABSL_CHECK_LT(2*nsubmatch_, static_cast<int>(ABSL_ARRAYSIZE(cap_)));
  memset(cap_, 0, sizeof cap_);

  // We use submatch_[0] for our own bookkeeping,
  // so it had better exist.
  absl::string_view sp0;
  if (nsubmatch < 1) {
    submatch_ = &sp0;
    nsubmatch_ = 1;
  }
  submatch_[0] = absl::string_view();

  // Allocate new visited_ bitmap -- size is proportional
  // to text, so have to reallocate on each call to Search.
  int nvisited = prog_->size() * static_cast<int>(text.size()+1);
  nvisited = (nvisited + 31) / 32;
  visited_ = PODArray<uint32_t>(nvisited);
  memset(visited_.data(), 0, nvisited*sizeof visited_[0]);

  // Anchored search must start at text.begin().
  if (anchored_) {
    cap_[0] = text.data();
    return Visit(prog_->start(), text.data());
  }

  // Unanchored search, starting from each possible text position.
  // Notice that we have to try the empty string at the end of
  // the text, so the loop condition is p <= text.end(), not p < text.end().
  for (const char* p = text.data(); p <= text.data() + text.size(); p++) {
    cap_[0] = p;
    if (Visit(prog_->start(), p))  // Match must be leftmost; done.
      return true;
    // Avoid invoking undefined behavior (arithmetic on a null pointer)
    // by simply not continuing the loop.
    if (p == NULL)
      break;
  }
  return false;
}

// Explores from instruction id at string position p looking for a match.
// Return true if found (so that caller can stop trying other possibilities).
bool Backtracker::Visit(int id, const char* p) {
  // Check bitmap.  If we've already explored from here,
  // either it didn't match or it did but we're hoping for a better match.
  // Either way, don't go down that road again.
  ABSL_CHECK(p <= text_.data() + text_.size());
  int n = id * static_cast<int>(text_.size()+1) +
          static_cast<int>(p-text_.data());
  ABSL_CHECK_LT(n/32, visited_.size());
  if (visited_[n/32] & (1 << (n&31)))
    return false;
  visited_[n/32] |= 1 << (n&31);

  Prog::Inst* ip = prog_->inst(id);
  if (Try(id, p)) {
    if (longest_ && !ip->last())
      Visit(id+1, p);
    return true;
  }
  if (!ip->last())
    return Visit(id+1, p);
  return false;
}

// Tries instruction id at string position p.
// Returns true if a match is found.
bool Backtracker::Try(int id, const char* p) {
  // Pick out byte at current position.  If at end of string,
  // have to explore in hope of finishing a match.  Use impossible byte -1.
  int c = -1;
  if (p < text_.data() + text_.size())
    c = *p & 0xFF;

  Prog::Inst* ip = prog_->inst(id);
  switch (ip->opcode()) {
    default:
      ABSL_LOG(FATAL) << "Unexpected opcode: " << ip->opcode();
      return false;  // not reached

    case kInstAltMatch:
      // Ignored.
      return false;

    case kInstByteRange:
      if (ip->Matches(c))
        return Visit(ip->out(), p+1);
      return false;

    case kInstCapture:
      if (0 <= ip->cap() &&
          ip->cap() < static_cast<int>(ABSL_ARRAYSIZE(cap_))) {
        // Capture p to register, but save old value.
        const char* q = cap_[ip->cap()];
        cap_[ip->cap()] = p;
        bool ret = Visit(ip->out(), p);
        // Restore old value as we backtrack.
        cap_[ip->cap()] = q;
        return ret;
      }
      return Visit(ip->out(), p);

    case kInstEmptyWidth:
      if (ip->empty() & ~Prog::EmptyFlags(context_, p))
        return false;
      return Visit(ip->out(), p);

    case kInstNop:
      return Visit(ip->out(), p);

    case kInstMatch:
      // We found a match.  If it's the best so far, record the
      // parameters in the caller's submatch_ array.
      if (endmatch_ && p != context_.data() + context_.size())
        return false;
      cap_[1] = p;
      if (submatch_[0].data() == NULL ||
          (longest_ && p > submatch_[0].data() + submatch_[0].size())) {
        // First match so far - or better match.
        for (int i = 0; i < nsubmatch_; i++)
          submatch_[i] = absl::string_view(
              cap_[2 * i], static_cast<size_t>(cap_[2 * i + 1] - cap_[2 * i]));
      }
      return true;

    case kInstFail:
      return false;
  }
}

// Runs a backtracking search.
bool Prog::UnsafeSearchBacktrack(absl::string_view text,
                                 absl::string_view context, Anchor anchor,
                                 MatchKind kind, absl::string_view* match,
                                 int nmatch) {
  // If full match, we ask for an anchored longest match
  // and then check that match[0] == text.
  // So make sure match[0] exists.
  absl::string_view sp0;
  if (kind == kFullMatch) {
    anchor = kAnchored;
    if (nmatch < 1) {
      match = &sp0;
      nmatch = 1;
    }
  }

  // Run the search.
  Backtracker b(this);
  bool anchored = anchor == kAnchored;
  bool longest = kind != kFirstMatch;
  if (!b.Search(text, context, anchored, longest, match, nmatch))
    return false;
  if (kind == kFullMatch && EndPtr(match[0]) != EndPtr(text))
    return false;
  return true;
}

}  // namespace re2
