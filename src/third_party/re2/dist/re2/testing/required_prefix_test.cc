// Copyright 2009 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include <stddef.h>

#include <string>

#include "absl/base/macros.h"
#include "gtest/gtest.h"
#include "re2/prog.h"
#include "re2/regexp.h"

namespace re2 {

struct PrefixTest {
  const char* regexp;
  bool return_value;
  const char* prefix;
  bool foldcase;
  const char* suffix;
};

static PrefixTest tests[] = {
  // Empty cases.
  { "", false },
  { "(?m)^", false },
  { "(?-m)^", false },

  // If the regexp has no ^, there's no required prefix.
  { "abc", false },

  // If the regexp immediately goes into
  // something not a literal match, there's no required prefix.
  { "^a*",  false },
  { "^(abc)", false },

  // Otherwise, it should work.
  { "^abc$", true, "abc", false, "(?-m:$)" },
  { "^abc", true, "abc", false, "" },
  { "^(?i)abc", true, "abc", true, "" },
  { "^abcd*", true, "abc", false, "d*" },
  { "^[Aa][Bb]cd*", true, "ab", true, "cd*" },
  { "^ab[Cc]d*", true, "ab", false, "[Cc]d*" },
  { "^☺abc", true, "☺abc", false, "" },
};

TEST(RequiredPrefix, SimpleTests) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(tests); i++) {
    const PrefixTest& t = tests[i];
    for (size_t j = 0; j < 2; j++) {
      Regexp::ParseFlags flags = Regexp::LikePerl;
      if (j == 0)
        flags = flags | Regexp::Latin1;
      Regexp* re = Regexp::Parse(t.regexp, flags, NULL);
      ASSERT_TRUE(re != NULL) << " " << t.regexp;

      std::string p;
      bool f;
      Regexp* s;
      ASSERT_EQ(t.return_value, re->RequiredPrefix(&p, &f, &s))
        << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8")
        << " " << re->Dump();
      if (t.return_value) {
        ASSERT_EQ(p, std::string(t.prefix))
          << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8");
        ASSERT_EQ(f, t.foldcase)
          << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8");
        ASSERT_EQ(s->ToString(), std::string(t.suffix))
          << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8");
        s->Decref();
      }
      re->Decref();
    }
  }
}

static PrefixTest for_accel_tests[] = {
  // Empty cases.
  { "", false },
  { "(?m)^", false },
  { "(?-m)^", false },

  // If the regexp has a ^, there's no required prefix.
  { "^abc", false },

  // If the regexp immediately goes into
  // something not a literal match, there's no required prefix.
  { "a*",  false },

  // Unlike RequiredPrefix(), RequiredPrefixForAccel() can "see through"
  // capturing groups, but doesn't try to glue prefix fragments together.
  { "(a?)def", false },
  { "(ab?)def", true, "a", false },
  { "(abc?)def", true, "ab", false },
  { "(()a)def", false },
  { "((a)b)def", true, "a", false },
  { "((ab)c)def", true, "ab", false },

  // Otherwise, it should work.
  { "abc$", true, "abc", false },
  { "abc", true, "abc", false },
  { "(?i)abc", true, "abc", true },
  { "abcd*", true, "abc", false },
  { "[Aa][Bb]cd*", true, "ab", true },
  { "ab[Cc]d*", true, "ab", false },
  { "☺abc", true, "☺abc", false },
};

TEST(RequiredPrefixForAccel, SimpleTests) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(for_accel_tests); i++) {
    const PrefixTest& t = for_accel_tests[i];
    for (size_t j = 0; j < 2; j++) {
      Regexp::ParseFlags flags = Regexp::LikePerl;
      if (j == 0)
        flags = flags | Regexp::Latin1;
      Regexp* re = Regexp::Parse(t.regexp, flags, NULL);
      ASSERT_TRUE(re != NULL) << " " << t.regexp;

      std::string p;
      bool f;
      ASSERT_EQ(t.return_value, re->RequiredPrefixForAccel(&p, &f))
        << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8")
        << " " << re->Dump();
      if (t.return_value) {
        ASSERT_EQ(p, std::string(t.prefix))
          << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8");
        ASSERT_EQ(f, t.foldcase)
          << " " << t.regexp << " " << (j == 0 ? "latin1" : "utf8");
      }
      re->Decref();
    }
  }
}

TEST(RequiredPrefixForAccel, CaseFoldingForKAndS) {
  Regexp* re;
  std::string p;
  bool f;

  // With Latin-1 encoding, `(?i)` prefixes can include 'k' and 's'.
  re = Regexp::Parse("(?i)KLM", Regexp::LikePerl|Regexp::Latin1, NULL);
  ASSERT_TRUE(re != NULL);
  ASSERT_TRUE(re->RequiredPrefixForAccel(&p, &f));
  ASSERT_EQ(p, "klm");
  ASSERT_EQ(f, true);
  re->Decref();

  re = Regexp::Parse("(?i)STU", Regexp::LikePerl|Regexp::Latin1, NULL);
  ASSERT_TRUE(re != NULL);
  ASSERT_TRUE(re->RequiredPrefixForAccel(&p, &f));
  ASSERT_EQ(p, "stu");
  ASSERT_EQ(f, true);
  re->Decref();

  // With UTF-8 encoding, `(?i)` prefixes can't include 'k' and 's'.
  // This is because they match U+212A and U+017F, respectively, and
  // so the parser ends up emitting character classes, not literals.
  re = Regexp::Parse("(?i)KLM", Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);
  ASSERT_FALSE(re->RequiredPrefixForAccel(&p, &f));
  re->Decref();

  re = Regexp::Parse("(?i)STU", Regexp::LikePerl, NULL);
  ASSERT_TRUE(re != NULL);
  ASSERT_FALSE(re->RequiredPrefixForAccel(&p, &f));
  re->Decref();
}

static const char* prefix_accel_tests[] = {
    "aababc\\d+",
    "(?i)AABABC\\d+",
};

TEST(PrefixAccel, SimpleTests) {
  for (size_t i = 0; i < ABSL_ARRAYSIZE(prefix_accel_tests); i++) {
    const char* pattern = prefix_accel_tests[i];
    Regexp* re = Regexp::Parse(pattern, Regexp::LikePerl, NULL);
    ASSERT_TRUE(re != NULL);
    Prog* prog = re->CompileToProg(0);
    ASSERT_TRUE(prog != NULL);
    ASSERT_TRUE(prog->can_prefix_accel());
    for (int j = 0; j < 100; j++) {
      std::string text(j, 'a');
      const char* p = reinterpret_cast<const char*>(
          prog->PrefixAccel(text.data(), text.size()));
      EXPECT_TRUE(p == NULL);
      text.append("aababc");
      for (int k = 0; k < 100; k++) {
        text.append(k, 'a');
        p = reinterpret_cast<const char*>(
            prog->PrefixAccel(text.data(), text.size()));
        EXPECT_EQ(j, p - text.data());
      }
    }
    delete prog;
    re->Decref();
  }
}

}  // namespace re2
