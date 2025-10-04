// Copyright 2006 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Dump the regexp into a string showing structure.
// Tested by parse_unittest.cc

// This function traverses the regexp recursively,
// meaning that on inputs like Regexp::Simplify of
// a{100}{100}{100}{100}{100}{100}{100}{100}{100}{100},
// it takes time and space exponential in the size of the
// original regular expression.  It can also use stack space
// linear in the size of the regular expression for inputs
// like ((((((((((((((((a*)*)*)*)*)*)*)*)*)*)*)*)*)*)*)*)*.
// IT IS NOT SAFE TO CALL FROM PRODUCTION CODE.
// As a result, Dump is provided only in the testing
// library (see BUILD).

#include <string>

#include "absl/base/macros.h"
#include "absl/log/absl_check.h"
#include "absl/log/absl_log.h"
#include "absl/strings/str_format.h"
#include "gtest/gtest.h"
#include "re2/regexp.h"
#include "util/utf.h"

namespace re2 {

static const char* kOpcodeNames[] = {
  "bad",
  "no",
  "emp",
  "lit",
  "str",
  "cat",
  "alt",
  "star",
  "plus",
  "que",
  "rep",
  "cap",
  "dot",
  "byte",
  "bol",
  "eol",
  "wb",   // kRegexpWordBoundary
  "nwb",  // kRegexpNoWordBoundary
  "bot",
  "eot",
  "cc",
  "match",
};

// Create string representation of regexp with explicit structure.
// Nothing pretty, just for testing.
static void DumpRegexpAppending(Regexp* re, std::string* s) {
  if (re->op() < 0 || re->op() >= ABSL_ARRAYSIZE(kOpcodeNames)) {
    *s += absl::StrFormat("op%d", re->op());
  } else {
    switch (re->op()) {
      default:
        break;
      case kRegexpStar:
      case kRegexpPlus:
      case kRegexpQuest:
      case kRegexpRepeat:
        if (re->parse_flags() & Regexp::NonGreedy)
          s->append("n");
        break;
    }
    s->append(kOpcodeNames[re->op()]);
    if (re->op() == kRegexpLiteral && (re->parse_flags() & Regexp::FoldCase)) {
      Rune r = re->rune();
      if ('a' <= r && r <= 'z')
        s->append("fold");
    }
    if (re->op() == kRegexpLiteralString && (re->parse_flags() & Regexp::FoldCase)) {
      for (int i = 0; i < re->nrunes(); i++) {
        Rune r = re->runes()[i];
        if ('a' <= r && r <= 'z') {
          s->append("fold");
          break;
        }
      }
    }
  }
  s->append("{");
  switch (re->op()) {
    default:
      break;
    case kRegexpEndText:
      if (!(re->parse_flags() & Regexp::WasDollar)) {
        s->append("\\z");
      }
      break;
    case kRegexpLiteral: {
      Rune r = re->rune();
      if (re->parse_flags() & Regexp::Latin1) {
        s->push_back(r);
      } else {
        char buf[UTFmax+1];
        buf[runetochar(buf, &r)] = 0;
        s->append(buf);
      }
      break;
    }
    case kRegexpLiteralString:
      for (int i = 0; i < re->nrunes(); i++) {
        Rune r = re->runes()[i];
        if (re->parse_flags() & Regexp::Latin1) {
          s->push_back(r);
        } else {
          char buf[UTFmax+1];
          buf[runetochar(buf, &r)] = 0;
          s->append(buf);
        }
      }
      break;
    case kRegexpConcat:
    case kRegexpAlternate:
      for (int i = 0; i < re->nsub(); i++)
        DumpRegexpAppending(re->sub()[i], s);
      break;
    case kRegexpStar:
    case kRegexpPlus:
    case kRegexpQuest:
      DumpRegexpAppending(re->sub()[0], s);
      break;
    case kRegexpCapture:
      if (re->cap() == 0)
        ABSL_LOG(DFATAL) << "kRegexpCapture cap() == 0";
      if (re->name()) {
        s->append(*re->name());
        s->append(":");
      }
      DumpRegexpAppending(re->sub()[0], s);
      break;
    case kRegexpRepeat:
      s->append(absl::StrFormat("%d,%d ", re->min(), re->max()));
      DumpRegexpAppending(re->sub()[0], s);
      break;
    case kRegexpCharClass: {
      std::string sep;
      for (CharClass::iterator it = re->cc()->begin();
           it != re->cc()->end(); ++it) {
        RuneRange rr = *it;
        s->append(sep);
        if (rr.lo == rr.hi)
          s->append(absl::StrFormat("%#x", rr.lo));
        else
          s->append(absl::StrFormat("%#x-%#x", rr.lo, rr.hi));
        sep = " ";
      }
      break;
    }
  }
  s->append("}");
}

std::string Regexp::Dump() {
  // Make sure that we are being called from a unit test.
  // Should cause a link error if used outside of testing.
  ABSL_CHECK(!::testing::TempDir().empty());

  std::string s;
  DumpRegexpAppending(this, &s);
  return s;
}

}  // namespace re2
