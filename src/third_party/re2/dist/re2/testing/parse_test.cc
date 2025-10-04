// Copyright 2006 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Test parse.cc, dump.cc, and tostring.cc.

#include <stddef.h>

#include <string>

#include "absl/base/macros.h"
#include "absl/log/absl_log.h"
#include "gtest/gtest.h"
#include "re2/regexp.h"

namespace re2 {

// In the past, we used 1<<30 here and zeroed the bit later, but that
// has undefined behaviour, so now we use an internal-only flag because
// otherwise we would have to introduce a new flag value just for this.
static const Regexp::ParseFlags TestZeroFlags = Regexp::WasDollar;

struct Test {
  const char* regexp;
  const char* parse;
  Regexp::ParseFlags flags;
};

static Regexp::ParseFlags kTestFlags = Regexp::MatchNL |
                                       Regexp::PerlX |
                                       Regexp::PerlClasses |
                                       Regexp::UnicodeGroups;

static Test tests[] = {
  // Base cases
  { "a", "lit{a}" },
  { "a.", "cat{lit{a}dot{}}" },
  { "a.b", "cat{lit{a}dot{}lit{b}}" },
  { "ab", "str{ab}" },
  { "a.b.c", "cat{lit{a}dot{}lit{b}dot{}lit{c}}" },
  { "abc", "str{abc}" },
  { "a|^", "alt{lit{a}bol{}}" },
  { "a|b", "cc{0x61-0x62}" },
  { "(a)", "cap{lit{a}}" },
  { "(a)|b", "alt{cap{lit{a}}lit{b}}" },
  { "a*", "star{lit{a}}" },
  { "a+", "plus{lit{a}}" },
  { "a?", "que{lit{a}}" },
  { "a{2}", "rep{2,2 lit{a}}" },
  { "a{2,3}", "rep{2,3 lit{a}}" },
  { "a{2,}", "rep{2,-1 lit{a}}" },
  { "a*?", "nstar{lit{a}}" },
  { "a+?", "nplus{lit{a}}" },
  { "a??", "nque{lit{a}}" },
  { "a{2}?", "nrep{2,2 lit{a}}" },
  { "a{2,3}?", "nrep{2,3 lit{a}}" },
  { "a{2,}?", "nrep{2,-1 lit{a}}" },
  { "", "emp{}" },
  { "|", "alt{emp{}emp{}}" },
  { "|x|", "alt{emp{}lit{x}emp{}}" },
  { ".", "dot{}" },
  { "^", "bol{}" },
  { "$", "eol{}" },
  { "\\|", "lit{|}" },
  { "\\(", "lit{(}" },
  { "\\)", "lit{)}" },
  { "\\*", "lit{*}" },
  { "\\+", "lit{+}" },
  { "\\?", "lit{?}" },
  { "{", "lit{{}" },
  { "}", "lit{}}" },
  { "\\.", "lit{.}" },
  { "\\^", "lit{^}" },
  { "\\$", "lit{$}" },
  { "\\\\", "lit{\\}" },
  { "[ace]", "cc{0x61 0x63 0x65}" },
  { "[abc]", "cc{0x61-0x63}" },
  { "[a-z]", "cc{0x61-0x7a}" },
  { "[a]", "lit{a}" },
  { "\\-", "lit{-}" },
  { "-", "lit{-}" },
  { "\\_", "lit{_}" },

  // Posix and Perl extensions
  { "[[:lower:]]", "cc{0x61-0x7a}" },
  { "[a-z]", "cc{0x61-0x7a}" },
  { "[^[:lower:]]", "cc{0-0x60 0x7b-0x10ffff}" },
  { "[[:^lower:]]", "cc{0-0x60 0x7b-0x10ffff}" },
  { "(?i)[[:lower:]]", "cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}" },
  { "(?i)[a-z]", "cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}" },
  { "(?i)[^[:lower:]]", "cc{0-0x40 0x5b-0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}" },
  { "(?i)[[:^lower:]]", "cc{0-0x40 0x5b-0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}" },
  { "\\d", "cc{0x30-0x39}" },
  { "\\D", "cc{0-0x2f 0x3a-0x10ffff}" },
  { "\\s", "cc{0x9-0xa 0xc-0xd 0x20}" },
  { "\\S", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}" },
  { "\\w", "cc{0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a}" },
  { "\\W", "cc{0-0x2f 0x3a-0x40 0x5b-0x5e 0x60 0x7b-0x10ffff}" },
  { "(?i)\\w", "cc{0x30-0x39 0x41-0x5a 0x5f 0x61-0x7a 0x17f 0x212a}" },
  { "(?i)\\W", "cc{0-0x2f 0x3a-0x40 0x5b-0x5e 0x60 0x7b-0x17e 0x180-0x2129 0x212b-0x10ffff}" },
  { "[^\\\\]", "cc{0-0x5b 0x5d-0x10ffff}" },
  { "\\C", "byte{}" },

  // Unicode, negatives, and a double negative.
  { "\\p{Braille}", "cc{0x2800-0x28ff}" },
  { "\\P{Braille}", "cc{0-0x27ff 0x2900-0x10ffff}" },
  { "\\p{^Braille}", "cc{0-0x27ff 0x2900-0x10ffff}" },
  { "\\P{^Braille}", "cc{0x2800-0x28ff}" },

  // More interesting regular expressions.
  { "a{,2}", "str{a{,2}}" },
  { "\\.\\^\\$\\\\", "str{.^$\\}" },
  { "[a-zABC]", "cc{0x41-0x43 0x61-0x7a}" },
  { "[^a]", "cc{0-0x60 0x62-0x10ffff}" },
  { "[\xce\xb1-\xce\xb5\xe2\x98\xba]", "cc{0x3b1-0x3b5 0x263a}" },  // utf-8
  { "a*{", "cat{star{lit{a}}lit{{}}" },

  // Test precedences
  { "(?:ab)*", "star{str{ab}}" },
  { "(ab)*", "star{cap{str{ab}}}" },
  { "ab|cd", "alt{str{ab}str{cd}}" },
  { "a(b|c)d", "cat{lit{a}cap{cc{0x62-0x63}}lit{d}}" },

  // Test squashing of **, ++, ?? et cetera.
  { "(?:(?:a)*)*", "star{lit{a}}" },
  { "(?:(?:a)+)+", "plus{lit{a}}" },
  { "(?:(?:a)?)?", "que{lit{a}}" },
  { "(?:(?:a)*)+", "star{lit{a}}" },
  { "(?:(?:a)*)?", "star{lit{a}}" },
  { "(?:(?:a)+)*", "star{lit{a}}" },
  { "(?:(?:a)+)?", "star{lit{a}}" },
  { "(?:(?:a)?)*", "star{lit{a}}" },
  { "(?:(?:a)?)+", "star{lit{a}}" },

  // Test flattening.
  { "(?:a)", "lit{a}" },
  { "(?:ab)(?:cd)", "str{abcd}" },
  { "(?:a|b)|(?:c|d)", "cc{0x61-0x64}" },
  { "a|c", "cc{0x61 0x63}" },
  { "a|[cd]", "cc{0x61 0x63-0x64}" },
  { "a|.", "dot{}" },
  { "[ab]|c", "cc{0x61-0x63}" },
  { "[ab]|[cd]", "cc{0x61-0x64}" },
  { "[ab]|.", "dot{}" },
  { ".|c", "dot{}" },
  { ".|[cd]", "dot{}" },
  { ".|.", "dot{}" },

  // Test Perl quoted literals
  { "\\Q+|*?{[\\E", "str{+|*?{[}" },
  { "\\Q+\\E+", "plus{lit{+}}" },
  { "\\Q\\\\E", "lit{\\}" },
  { "\\Q\\\\\\E", "str{\\\\}" },
  { "\\Qa\\E*", "star{lit{a}}" },
  { "\\Qab\\E*", "cat{lit{a}star{lit{b}}}" },
  { "\\Qabc\\E*", "cat{str{ab}star{lit{c}}}" },

  // Test Perl \A and \z
  { "(?m)^", "bol{}" },
  { "(?m)$", "eol{}" },
  { "(?-m)^", "bot{}" },
  { "(?-m)$", "eot{}" },
  { "(?m)\\A", "bot{}" },
  { "(?m)\\z", "eot{\\z}" },
  { "(?-m)\\A", "bot{}" },
  { "(?-m)\\z", "eot{\\z}" },

  // Test named captures
  { "(?P<name>a)", "cap{name:lit{a}}" },
  { "(?P<中文>a)", "cap{中文:lit{a}}" },
  { "(?<name>a)", "cap{name:lit{a}}" },
  { "(?<中文>a)", "cap{中文:lit{a}}" },

  // Case-folded literals
  { "[Aa]", "litfold{a}" },

  // Strings
  { "abcde", "str{abcde}" },
  { "[Aa][Bb]cd", "cat{strfold{ab}str{cd}}" },

  // Reported bug involving \n leaking in despite use of NeverNL.
  { "[^ ]", "cc{0-0x9 0xb-0x1f 0x21-0x10ffff}", TestZeroFlags },
  { "[^ ]", "cc{0-0x9 0xb-0x1f 0x21-0x10ffff}", Regexp::FoldCase },
  { "[^ ]", "cc{0-0x9 0xb-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ ]", "cc{0-0x9 0xb-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \f]", "cc{0-0x9 0xb 0xd-0x1f 0x21-0x10ffff}", TestZeroFlags },
  { "[^ \f]", "cc{0-0x9 0xb 0xd-0x1f 0x21-0x10ffff}", Regexp::FoldCase },
  { "[^ \f]", "cc{0-0x9 0xb 0xd-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \f]", "cc{0-0x9 0xb 0xd-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \r]", "cc{0-0x9 0xb-0xc 0xe-0x1f 0x21-0x10ffff}", TestZeroFlags },
  { "[^ \r]", "cc{0-0x9 0xb-0xc 0xe-0x1f 0x21-0x10ffff}", Regexp::FoldCase },
  { "[^ \r]", "cc{0-0x9 0xb-0xc 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \r]", "cc{0-0x9 0xb-0xc 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \v]", "cc{0-0x9 0xc-0x1f 0x21-0x10ffff}", TestZeroFlags },
  { "[^ \v]", "cc{0-0x9 0xc-0x1f 0x21-0x10ffff}", Regexp::FoldCase },
  { "[^ \v]", "cc{0-0x9 0xc-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \v]", "cc{0-0x9 0xc-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \t]", "cc{0-0x8 0xb-0x1f 0x21-0x10ffff}", TestZeroFlags },
  { "[^ \t]", "cc{0-0x8 0xb-0x1f 0x21-0x10ffff}", Regexp::FoldCase },
  { "[^ \t]", "cc{0-0x8 0xb-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \t]", "cc{0-0x8 0xb-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \r\f\v]", "cc{0-0x9 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \r\f\v]", "cc{0-0x9 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \r\f\t\v]", "cc{0-0x8 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \r\f\t\v]", "cc{0-0x8 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \r\n\f\t\v]", "cc{0-0x8 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \r\n\f\t\v]", "cc{0-0x8 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^ \r\n\f\t]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL },
  { "[^ \r\n\f\t]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}", Regexp::NeverNL | Regexp::FoldCase },
  { "[^\t-\n\f-\r ]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses },
  { "[^\t-\n\f-\r ]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::FoldCase },
  { "[^\t-\n\f-\r ]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::NeverNL },
  { "[^\t-\n\f-\r ]", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::NeverNL | Regexp::FoldCase },
  { "\\S", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses },
  { "\\S", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::FoldCase },
  { "\\S", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::NeverNL },
  { "\\S", "cc{0-0x8 0xb 0xe-0x1f 0x21-0x10ffff}",
    Regexp::PerlClasses | Regexp::NeverNL | Regexp::FoldCase },

  // Bug in Regexp::ToString() that emitted [^], which
  // would (obviously) fail to parse when fed back in.
  { "[\\s\\S]", "cc{0-0x10ffff}" },

  // As per https://github.com/google/re2/issues/477,
  // there were long-standing bugs involving Latin-1.
  // Here, we exercise it WITHOUT case folding...
  { "\xa5\x64\xd1", "str{\xa5""d\xd1}", Regexp::Latin1 },
  { "\xa5\xd1\x64", "str{\xa5\xd1""d}", Regexp::Latin1 },
  { "\xa5\x64[\xd1\xd2]", "cat{str{\xa5""d}cc{0xd1-0xd2}}", Regexp::Latin1 },
  { "\xa5[\xd1\xd2]\x64", "cat{lit{\xa5}cc{0xd1-0xd2}lit{d}}", Regexp::Latin1 },
  { "\xa5\x64|\xa5\xd1", "cat{lit{\xa5}cc{0x64 0xd1}}", Regexp::Latin1 },
  { "\xa5\xd1|\xa5\x64", "cat{lit{\xa5}cc{0x64 0xd1}}", Regexp::Latin1 },
  { "\xa5\x64|\xa5[\xd1\xd2]", "cat{lit{\xa5}cc{0x64 0xd1-0xd2}}", Regexp::Latin1 },
  { "\xa5[\xd1\xd2]|\xa5\x64", "cat{lit{\xa5}cc{0x64 0xd1-0xd2}}", Regexp::Latin1 },
  // Here, we exercise it WITH case folding...
  // 0x64 should fold to 0x44, but neither 0xD1 nor 0xD2
  // should fold to 0xF1 and 0xF2, respectively.
  { "\xa5\x64\xd1", "strfold{\xa5""d\xd1}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5\xd1\x64", "strfold{\xa5\xd1""d}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5\x64[\xd1\xd2]", "cat{strfold{\xa5""d}cc{0xd1-0xd2}}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5[\xd1\xd2]\x64", "cat{lit{\xa5}cc{0xd1-0xd2}litfold{d}}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5\x64|\xa5\xd1", "cat{lit{\xa5}cc{0x44 0x64 0xd1}}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5\xd1|\xa5\x64", "cat{lit{\xa5}cc{0x44 0x64 0xd1}}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5\x64|\xa5[\xd1\xd2]", "cat{lit{\xa5}cc{0x44 0x64 0xd1-0xd2}}", Regexp::Latin1 | Regexp::FoldCase },
  { "\xa5[\xd1\xd2]|\xa5\x64", "cat{lit{\xa5}cc{0x44 0x64 0xd1-0xd2}}", Regexp::Latin1 | Regexp::FoldCase },
};

bool RegexpEqualTestingOnly(Regexp* a, Regexp* b) {
  return Regexp::Equal(a, b);
}

void TestParse(const Test* tests, int ntests, Regexp::ParseFlags flags,
               const std::string& title) {
  Regexp** re = new Regexp*[ntests];
  for (int i = 0; i < ntests; i++) {
    RegexpStatus status;
    Regexp::ParseFlags f = flags;
    if (tests[i].flags != 0) {
      f = tests[i].flags & ~TestZeroFlags;
    }
    re[i] = Regexp::Parse(tests[i].regexp, f, &status);
    ASSERT_TRUE(re[i] != NULL)
      << " " << tests[i].regexp << " " << status.Text();
    std::string s = re[i]->Dump();
    EXPECT_EQ(std::string(tests[i].parse), s)
        << "Regexp: " << tests[i].regexp
        << "\nparse: " << std::string(tests[i].parse)
        << " s: " << s << " flag=" << f;
  }

  for (int i = 0; i < ntests; i++) {
    for (int j = 0; j < ntests; j++) {
      EXPECT_EQ(std::string(tests[i].parse) == std::string(tests[j].parse),
                RegexpEqualTestingOnly(re[i], re[j]))
        << "Regexp: " << tests[i].regexp << " " << tests[j].regexp;
    }
  }

  for (int i = 0; i < ntests; i++)
    re[i]->Decref();
  delete[] re;
}

// Test that regexps parse to expected structures.
TEST(TestParse, SimpleRegexps) {
  TestParse(tests, ABSL_ARRAYSIZE(tests), kTestFlags, "simple");
}

Test foldcase_tests[] = {
  { "AbCdE", "strfold{abcde}" },
  { "[Aa]", "litfold{a}" },
  { "a", "litfold{a}" },

  // 0x17F is an old English long s (looks like an f) and folds to s.
  // 0x212A is the Kelvin symbol and folds to k.
  { "A[F-g]", "cat{litfold{a}cc{0x41-0x7a 0x17f 0x212a}}" },  // [Aa][A-z...]
  { "[[:upper:]]", "cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}" },
  { "[[:lower:]]", "cc{0x41-0x5a 0x61-0x7a 0x17f 0x212a}" },
};

// Test that parsing with FoldCase works.
TEST(TestParse, FoldCase) {
  TestParse(foldcase_tests, ABSL_ARRAYSIZE(foldcase_tests), Regexp::FoldCase, "foldcase");
}

Test literal_tests[] = {
  { "(|)^$.[*+?]{5,10},\\", "str{(|)^$.[*+?]{5,10},\\}" },
};

// Test that parsing with Literal works.
TEST(TestParse, Literal) {
  TestParse(literal_tests, ABSL_ARRAYSIZE(literal_tests), Regexp::Literal, "literal");
}

Test matchnl_tests[] = {
  { ".", "dot{}" },
  { "\n", "lit{\n}" },
  { "[^a]", "cc{0-0x60 0x62-0x10ffff}" },
  { "[a\\n]", "cc{0xa 0x61}" },
};

// Test that parsing with MatchNL works.
// (Also tested above during simple cases.)
TEST(TestParse, MatchNL) {
  TestParse(matchnl_tests, ABSL_ARRAYSIZE(matchnl_tests), Regexp::MatchNL, "with MatchNL");
}

Test nomatchnl_tests[] = {
  { ".", "cc{0-0x9 0xb-0x10ffff}" },
  { "\n", "lit{\n}" },
  { "[^a]", "cc{0-0x9 0xb-0x60 0x62-0x10ffff}" },
  { "[a\\n]", "cc{0xa 0x61}" },
};

// Test that parsing without MatchNL works.
TEST(TestParse, NoMatchNL) {
  TestParse(nomatchnl_tests, ABSL_ARRAYSIZE(nomatchnl_tests), Regexp::NoParseFlags, "without MatchNL");
}

Test prefix_tests[] = {
  { "abc|abd", "cat{str{ab}cc{0x63-0x64}}" },
  { "a(?:b)c|abd", "cat{str{ab}cc{0x63-0x64}}" },
  { "abc|abd|aef|bcx|bcy",
    "alt{cat{lit{a}alt{cat{lit{b}cc{0x63-0x64}}str{ef}}}"
      "cat{str{bc}cc{0x78-0x79}}}" },
  { "abc|x|abd", "alt{str{abc}lit{x}str{abd}}" },
  { "(?i)abc|ABD", "cat{strfold{ab}cc{0x43-0x44 0x63-0x64}}" },
  { "[ab]c|[ab]d", "cat{cc{0x61-0x62}cc{0x63-0x64}}" },
  { ".c|.d", "cat{cc{0-0x9 0xb-0x10ffff}cc{0x63-0x64}}" },
  { "\\Cc|\\Cd", "cat{byte{}cc{0x63-0x64}}" },
  { "x{2}|x{2}[0-9]",
    "cat{rep{2,2 lit{x}}alt{emp{}cc{0x30-0x39}}}" },
  { "x{2}y|x{2}[0-9]y",
    "cat{rep{2,2 lit{x}}alt{lit{y}cat{cc{0x30-0x39}lit{y}}}}" },
  { "n|r|rs",
    "alt{lit{n}cat{lit{r}alt{emp{}lit{s}}}}" },
  { "n|rs|r",
    "alt{lit{n}cat{lit{r}alt{lit{s}emp{}}}}" },
  { "r|rs|n",
    "alt{cat{lit{r}alt{emp{}lit{s}}}lit{n}}" },
  { "rs|r|n",
    "alt{cat{lit{r}alt{lit{s}emp{}}}lit{n}}" },
  { "a\\C*?c|a\\C*?b",
    "cat{lit{a}alt{cat{nstar{byte{}}lit{c}}cat{nstar{byte{}}lit{b}}}}" },
  { "^/a/bc|^/a/de",
    "cat{bol{}cat{str{/a/}alt{str{bc}str{de}}}}" },
  // In the past, factoring was limited to kFactorAlternationMaxDepth (8).
  { "a|aa|aaa|aaaa|aaaaa|aaaaaa|aaaaaaa|aaaaaaaa|aaaaaaaaa|aaaaaaaaaa",
    "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}"
    "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}"
    "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}" "cat{lit{a}alt{emp{}"
    "lit{a}}}}}}}}}}}}}}}}}}}" },
  { "a|aardvark|aardvarks|abaci|aback|abacus|abacuses|abaft|abalone|abalones",
    "cat{lit{a}alt{emp{}cat{str{ardvark}alt{emp{}lit{s}}}"
    "cat{str{ba}alt{cat{lit{c}alt{cc{0x69 0x6b}cat{str{us}alt{emp{}str{es}}}}}"
    "str{ft}cat{str{lone}alt{emp{}lit{s}}}}}}}" },
  // As per https://github.com/google/re2/issues/467,
  // these should factor identically, but they didn't
  // because AddFoldedRange() terminated prematurely.
  { "0A|0[aA]", "cat{lit{0}cc{0x41 0x61}}" },
  { "0a|0[aA]", "cat{lit{0}cc{0x41 0x61}}" },
  { "0[aA]|0A", "cat{lit{0}cc{0x41 0x61}}" },
  { "0[aA]|0a", "cat{lit{0}cc{0x41 0x61}}" },
};

// Test that prefix factoring works.
TEST(TestParse, Prefix) {
  TestParse(prefix_tests, ABSL_ARRAYSIZE(prefix_tests), Regexp::PerlX, "prefix");
}

Test nested_tests[] = {
  { "((((((((((x{2}){2}){2}){2}){2}){2}){2}){2}){2}))",
    "cap{cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 lit{x}}}}}}}}}}}}}}}}}}}}" },
  { "((((((((((x{1}){2}){2}){2}){2}){2}){2}){2}){2}){2})",
    "cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{1,1 lit{x}}}}}}}}}}}}}}}}}}}}}" },
  { "((((((((((x{0}){2}){2}){2}){2}){2}){2}){2}){2}){2})",
    "cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 cap{rep{0,0 lit{x}}}}}}}}}}}}}}}}}}}}}" },
  { "((((((x{2}){2}){2}){5}){5}){5})",
    "cap{rep{5,5 cap{rep{5,5 cap{rep{5,5 cap{rep{2,2 cap{rep{2,2 cap{rep{2,2 lit{x}}}}}}}}}}}}}" },
};

// Test that nested repetition works.
TEST(TestParse, Nested) {
  TestParse(nested_tests, ABSL_ARRAYSIZE(nested_tests), Regexp::PerlX, "nested");
}

// Invalid regular expressions
const char* badtests[] = {
  "(",
  ")",
  "(a",
  "(a|b|",
  "(a|b",
  "[a-z",
  "([a-z)",
  "x{1001}",
  "\xff",      // Invalid UTF-8
  "[\xff]",
  "[\\\xff]",
  "\\\xff",
  "(?P<name>a",
  "(?P<name>",
  "(?P<name",
  "(?P<x y>a)",
  "(?P<>a)",
  "(?<name>a",
  "(?<name>",
  "(?<name",
  "(?<x y>a)",
  "(?<>a)",
  "[a-Z]",
  "(?i)[a-Z]",
  "a{100000}",
  "a{100000,}",
  "((((((((((x{2}){2}){2}){2}){2}){2}){2}){2}){2}){2})",
  "(((x{7}){11}){13})",
  "\\Q\\E*",
};

// Valid in Perl, bad in POSIX
const char* only_perl[] = {
 "[a-b-c]",
 "\\Qabc\\E",
 "\\Q*+?{[\\E",
 "\\Q\\\\E",
 "\\Q\\\\\\E",
 "\\Q\\\\\\\\E",
 "\\Q\\\\\\\\\\E",
 "(?:a)",
 "(?P<name>a)",
 "(?<name>a)",
};

// Valid in POSIX, bad in Perl.
const char* only_posix[] = {
  "a++",
  "a**",
  "a?*",
  "a+*",
  "a{1}*",
};

// Test that parser rejects bad regexps.
TEST(TestParse, InvalidRegexps) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(badtests); i++) {
    ASSERT_TRUE(Regexp::Parse(badtests[i], Regexp::PerlX, NULL) == NULL)
      << " " << badtests[i];
    ASSERT_TRUE(Regexp::Parse(badtests[i], Regexp::NoParseFlags, NULL) == NULL)
      << " " << badtests[i];
  }
  for (size_t i = 0; i < ABSL_ARRAYSIZE(only_posix); i++) {
    ASSERT_TRUE(Regexp::Parse(only_posix[i], Regexp::PerlX, NULL) == NULL)
      << " " << only_posix[i];
    Regexp* re = Regexp::Parse(only_posix[i], Regexp::NoParseFlags, NULL);
    ASSERT_TRUE(re != NULL) << " " << only_posix[i];
    re->Decref();
  }
  for (size_t i = 0; i < ABSL_ARRAYSIZE(only_perl); i++) {
    ASSERT_TRUE(Regexp::Parse(only_perl[i], Regexp::NoParseFlags, NULL) == NULL)
      << " " << only_perl[i];
    Regexp* re = Regexp::Parse(only_perl[i], Regexp::PerlX, NULL);
    ASSERT_TRUE(re != NULL) << " " << only_perl[i];
    re->Decref();
  }
}

// Test that ToString produces original regexp or equivalent one.
TEST(TestToString, EquivalentParse) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(tests); i++) {
    RegexpStatus status;
    Regexp::ParseFlags f = kTestFlags;
    if (tests[i].flags != 0) {
      f = tests[i].flags & ~TestZeroFlags;
    }
    Regexp* re = Regexp::Parse(tests[i].regexp, f, &status);
    ASSERT_TRUE(re != NULL) << " " << tests[i].regexp << " " << status.Text();
    std::string s = re->Dump();
    EXPECT_EQ(std::string(tests[i].parse), s)
        << "Regexp: " << tests[i].regexp
        << "\nparse: " << std::string(tests[i].parse)
        << " s: " << s << " flag=" << f;
    std::string t = re->ToString();
    if (t != tests[i].regexp) {
      // If ToString didn't return the original regexp,
      // it must have found one with fewer parens.
      // Unfortunately we can't check the length here, because
      // ToString produces "\\{" for a literal brace,
      // but "{" is a shorter equivalent.
      // ASSERT_LT(t.size(), strlen(tests[i].regexp))
      //     << " t=" << t << " regexp=" << tests[i].regexp;

      // Test that if we parse the new regexp we get the same structure.
      Regexp* nre = Regexp::Parse(t, f, &status);
      ASSERT_TRUE(nre != NULL) << " reparse " << t << " " << status.Text();
      std::string ss = nre->Dump();
      std::string tt = nre->ToString();
      if (s != ss || t != tt)
        ABSL_LOG(INFO) << "ToString(" << tests[i].regexp << ") = " << t;
      EXPECT_EQ(s, ss);
      EXPECT_EQ(t, tt);
      nre->Decref();
    }
    re->Decref();
  }
}

// Test that capture error args are correct.
TEST(NamedCaptures, ErrorArgs) {
  RegexpStatus status;
  Regexp* re;

  re = Regexp::Parse("test(?P<name", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadNamedCapture);
  EXPECT_EQ(status.error_arg(), "(?P<name");

  re = Regexp::Parse("test(?P<space bar>z)", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadNamedCapture);
  EXPECT_EQ(status.error_arg(), "(?P<space bar>");

  re = Regexp::Parse("test(?<name", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadNamedCapture);
  EXPECT_EQ(status.error_arg(), "(?<name");

  re = Regexp::Parse("test(?<space bar>z)", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadNamedCapture);
  EXPECT_EQ(status.error_arg(), "(?<space bar>");
}

// Test that look-around error args are correct.
TEST(LookAround, ErrorArgs) {
  RegexpStatus status;
  Regexp* re;

  re = Regexp::Parse("(?=foo).*", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadPerlOp);
  EXPECT_EQ(status.error_arg(), "(?=");

  re = Regexp::Parse("(?!foo).*", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadPerlOp);
  EXPECT_EQ(status.error_arg(), "(?!");

  re = Regexp::Parse("(?<=foo).*", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadPerlOp);
  EXPECT_EQ(status.error_arg(), "(?<=");

  re = Regexp::Parse("(?<!foo).*", Regexp::LikePerl, &status);
  EXPECT_TRUE(re == NULL);
  EXPECT_EQ(status.code(), kRegexpBadPerlOp);
  EXPECT_EQ(status.error_arg(), "(?<!");
}

}  // namespace re2
