// Copyright 2010 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/set.h"

#include <stddef.h>

#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "re2/re2.h"

namespace re2 {

TEST(Set, Unanchored) {
  RE2::Set s(RE2::DefaultOptions, RE2::UNANCHORED);

  ASSERT_EQ(s.Size(), 0);
  ASSERT_EQ(s.Add("foo", NULL), 0);
  ASSERT_EQ(s.Size(), 1);
  ASSERT_EQ(s.Add("(", NULL), -1);
  ASSERT_EQ(s.Size(), 1);
  ASSERT_EQ(s.Add("bar", NULL), 1);
  ASSERT_EQ(s.Size(), 2);
  ASSERT_EQ(s.Compile(), true);
  ASSERT_EQ(s.Size(), 2);

  ASSERT_EQ(s.Match("foobar", NULL), true);
  ASSERT_EQ(s.Match("fooba", NULL), true);
  ASSERT_EQ(s.Match("oobar", NULL), true);

  std::vector<int> v;
  ASSERT_EQ(s.Match("foobar", &v), true);
  ASSERT_EQ(v.size(), size_t{2});
  ASSERT_EQ(v[0], 0);
  ASSERT_EQ(v[1], 1);

  ASSERT_EQ(s.Match("fooba", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("oobar", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 1);
}

TEST(Set, UnanchoredFactored) {
  RE2::Set s(RE2::DefaultOptions, RE2::UNANCHORED);

  ASSERT_EQ(s.Add("foo", NULL), 0);
  ASSERT_EQ(s.Add("(", NULL), -1);
  ASSERT_EQ(s.Add("foobar", NULL), 1);
  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("foobar", NULL), true);
  ASSERT_EQ(s.Match("obarfoobaroo", NULL), true);
  ASSERT_EQ(s.Match("fooba", NULL), true);
  ASSERT_EQ(s.Match("oobar", NULL), false);

  std::vector<int> v;
  ASSERT_EQ(s.Match("foobar", &v), true);
  ASSERT_EQ(v.size(), size_t{2});
  ASSERT_EQ(v[0], 0);
  ASSERT_EQ(v[1], 1);

  ASSERT_EQ(s.Match("obarfoobaroo", &v), true);
  ASSERT_EQ(v.size(), size_t{2});
  ASSERT_EQ(v[0], 0);
  ASSERT_EQ(v[1], 1);

  ASSERT_EQ(s.Match("fooba", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("oobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});
}

TEST(Set, UnanchoredDollar) {
  RE2::Set s(RE2::DefaultOptions, RE2::UNANCHORED);

  ASSERT_EQ(s.Add("foo$", NULL), 0);
  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("foo", NULL), true);
  ASSERT_EQ(s.Match("foobar", NULL), false);

  std::vector<int> v;
  ASSERT_EQ(s.Match("foo", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("foobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});
}

TEST(Set, UnanchoredWordBoundary) {
  RE2::Set s(RE2::DefaultOptions, RE2::UNANCHORED);

  ASSERT_EQ(s.Add("foo\\b", NULL), 0);
  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("foo", NULL), true);
  ASSERT_EQ(s.Match("foobar", NULL), false);
  ASSERT_EQ(s.Match("foo bar", NULL), true);

  std::vector<int> v;
  ASSERT_EQ(s.Match("foo", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("foobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("foo bar", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);
}

TEST(Set, Anchored) {
  RE2::Set s(RE2::DefaultOptions, RE2::ANCHOR_BOTH);

  ASSERT_EQ(s.Add("foo", NULL), 0);
  ASSERT_EQ(s.Add("(", NULL), -1);
  ASSERT_EQ(s.Add("bar", NULL), 1);
  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("foobar", NULL), false);
  ASSERT_EQ(s.Match("fooba", NULL), false);
  ASSERT_EQ(s.Match("oobar", NULL), false);
  ASSERT_EQ(s.Match("foo", NULL), true);
  ASSERT_EQ(s.Match("bar", NULL), true);

  std::vector<int> v;
  ASSERT_EQ(s.Match("foobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("fooba", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("oobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("foo", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("bar", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 1);
}

TEST(Set, EmptyUnanchored) {
  RE2::Set s(RE2::DefaultOptions, RE2::UNANCHORED);

  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("", NULL), false);
  ASSERT_EQ(s.Match("foobar", NULL), false);

  std::vector<int> v;
  ASSERT_EQ(s.Match("", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("foobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});
}

TEST(Set, EmptyAnchored) {
  RE2::Set s(RE2::DefaultOptions, RE2::ANCHOR_BOTH);

  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("", NULL), false);
  ASSERT_EQ(s.Match("foobar", NULL), false);

  std::vector<int> v;
  ASSERT_EQ(s.Match("", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("foobar", &v), false);
  ASSERT_EQ(v.size(), size_t{0});
}

TEST(Set, Prefix) {
  RE2::Set s(RE2::DefaultOptions, RE2::ANCHOR_BOTH);

  ASSERT_EQ(s.Add("/prefix/\\d*", NULL), 0);
  ASSERT_EQ(s.Compile(), true);

  ASSERT_EQ(s.Match("/prefix", NULL), false);
  ASSERT_EQ(s.Match("/prefix/", NULL), true);
  ASSERT_EQ(s.Match("/prefix/42", NULL), true);

  std::vector<int> v;
  ASSERT_EQ(s.Match("/prefix", &v), false);
  ASSERT_EQ(v.size(), size_t{0});

  ASSERT_EQ(s.Match("/prefix/", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);

  ASSERT_EQ(s.Match("/prefix/42", &v), true);
  ASSERT_EQ(v.size(), size_t{1});
  ASSERT_EQ(v[0], 0);
}

TEST(Set, MoveSemantics) {
  RE2::Set s1(RE2::DefaultOptions, RE2::UNANCHORED);
  ASSERT_EQ(s1.Add("foo\\d+", NULL), 0);
  ASSERT_EQ(s1.Compile(), true);
  ASSERT_EQ(s1.Match("abc foo1 xyz", NULL), true);
  ASSERT_EQ(s1.Match("abc bar2 xyz", NULL), false);

  // The moved-to object should do what the moved-from object did.
  RE2::Set s2 = std::move(s1);
  ASSERT_EQ(s2.Match("abc foo1 xyz", NULL), true);
  ASSERT_EQ(s2.Match("abc bar2 xyz", NULL), false);

  // The moved-from object should have been reset and be reusable.
  ASSERT_EQ(s1.Add("bar\\d+", NULL), 0);
  ASSERT_EQ(s1.Compile(), true);
  ASSERT_EQ(s1.Match("abc foo1 xyz", NULL), false);
  ASSERT_EQ(s1.Match("abc bar2 xyz", NULL), true);

  // Verify that "overwriting" works and also doesn't leak memory.
  // (The latter will need a leak detector such as LeakSanitizer.)
  s1 = std::move(s2);
  ASSERT_EQ(s1.Match("abc foo1 xyz", NULL), true);
  ASSERT_EQ(s1.Match("abc bar2 xyz", NULL), false);
}

}  // namespace re2
