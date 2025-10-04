// Copyright 2009 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "re2/filtered_re2.h"

#include <stddef.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/macros.h"
#include "absl/log/absl_log.h"
#include "gtest/gtest.h"
#include "re2/re2.h"

namespace re2 {

struct FilterTestVars {
  FilterTestVars() {}
  explicit FilterTestVars(int min_atom_len) : f(min_atom_len) {}

  std::vector<std::string> atoms;
  std::vector<int> atom_indices;
  std::vector<int> matches;
  RE2::Options opts;
  FilteredRE2 f;
};

TEST(FilteredRE2Test, EmptyTest) {
  FilterTestVars v;

  v.f.Compile(&v.atoms);
  EXPECT_EQ(size_t{0}, v.atoms.size());

  // Compile has no effect at all when called before Add: it will not
  // record that it has been called and it will not clear the vector.
  // The second point does not matter here, but the first point means
  // that an error will be logged during the call to AllMatches.
  v.f.AllMatches("foo", v.atom_indices, &v.matches);
  EXPECT_EQ(size_t{0}, v.matches.size());
}

TEST(FilteredRE2Test, SmallOrTest) {
  FilterTestVars v(4);  // override the minimum atom length
  int id;
  v.f.Add("(foo|bar)", v.opts, &id);

  v.f.Compile(&v.atoms);
  EXPECT_EQ(size_t{0}, v.atoms.size());

  v.f.AllMatches("lemurs bar", v.atom_indices, &v.matches);
  EXPECT_EQ(size_t{1}, v.matches.size());
  EXPECT_EQ(id, v.matches[0]);
}

TEST(FilteredRE2Test, SmallLatinTest) {
  FilterTestVars v;
  int id;

  v.opts.set_encoding(RE2::Options::EncodingLatin1);
  v.f.Add("\xde\xadQ\xbe\xef", v.opts, &id);
  v.f.Compile(&v.atoms);
  EXPECT_EQ(size_t{1}, v.atoms.size());
  EXPECT_EQ(v.atoms[0], "\xde\xadq\xbe\xef");

  v.atom_indices.push_back(0);
  v.f.AllMatches("foo\xde\xadQ\xbe\xeflemur", v.atom_indices, &v.matches);
  EXPECT_EQ(size_t{1}, v.matches.size());
  EXPECT_EQ(id, v.matches[0]);
}

struct AtomTest {
  const char* testname;
  // If any test needs more than this many regexps or atoms, increase
  // the size of the corresponding array.
  const char* regexps[20];
  const char* atoms[20];
};

AtomTest atom_tests[] = {
  {
    // This test checks to make sure empty patterns are allowed.
    "CheckEmptyPattern",
    {""},
    {}
  }, {
    // This test checks that all atoms of length greater than min length
    // are found, and no atoms that are of smaller length are found.
    "AllAtomsGtMinLengthFound", {
      "(abc123|def456|ghi789).*mnop[x-z]+",
      "abc..yyy..zz",
      "mnmnpp[a-z]+PPP"
    }, {
      "abc123",
      "def456",
      "ghi789",
      "mnop",
      "abc",
      "yyy",
      "mnmnpp",
      "ppp"
    }
  }, {
    // Test to make sure that any atoms that have another atom as a
    // substring in an OR are removed; that is, only the shortest
    // substring is kept.
    "SubstrAtomRemovesSuperStrInOr", {
      "(abc123|abc|defxyz|ghi789|abc1234|xyz).*[x-z]+",
      "abcd..yyy..yyyzzz",
      "mnmnpp[a-z]+PPP"
    }, {
      "abc",
      "ghi789",
      "xyz",
      "abcd",
      "yyy",
      "yyyzzz",
      "mnmnpp",
      "ppp"
    }
  }, {
    // Test character class expansion.
    "CharClassExpansion", {
      "m[a-c][d-f]n.*[x-z]+",
      "[x-y]bcde[ab]"
    }, {
      "madn", "maen", "mafn",
      "mbdn", "mben", "mbfn",
      "mcdn", "mcen", "mcfn",
      "xbcdea", "xbcdeb",
      "ybcdea", "ybcdeb"
    }
  }, {
    // Test upper/lower of non-ASCII.
    "UnicodeLower", {
      "(?i)ΔδΠϖπΣςσ",
      "ΛΜΝΟΠ",
      "ψρστυ",
    }, {
      "δδπππσσσ",
      "λμνοπ",
      "ψρστυ",
    },
  },
};

void AddRegexpsAndCompile(const char* regexps[],
                          size_t n,
                          struct FilterTestVars* v) {
  for (size_t i = 0; i < n; i++) {
    int id;
    v->f.Add(regexps[i], v->opts, &id);
  }
  v->f.Compile(&v->atoms);
}

bool CheckExpectedAtoms(const char* atoms[],
                        size_t n,
                        const char* testname,
                        struct FilterTestVars* v) {
  std::vector<std::string> expected;
  for (size_t i = 0; i < n; i++)
    expected.push_back(atoms[i]);

  bool pass = expected.size() == v->atoms.size();

  std::sort(v->atoms.begin(), v->atoms.end());
  std::sort(expected.begin(), expected.end());
  for (size_t i = 0; pass && i < n; i++)
      pass = pass && expected[i] == v->atoms[i];

  if (!pass) {
    ABSL_LOG(ERROR) << "Failed " << testname;
    ABSL_LOG(ERROR) << "Expected #atoms = " << expected.size();
    for (size_t i = 0; i < expected.size(); i++)
      ABSL_LOG(ERROR) << expected[i];
    ABSL_LOG(ERROR) << "Found #atoms = " << v->atoms.size();
    for (size_t i = 0; i < v->atoms.size(); i++)
      ABSL_LOG(ERROR) << v->atoms[i];
  }

  return pass;
}

TEST(FilteredRE2Test, AtomTests) {
  int nfail = 0;
  for (size_t i = 0; i < ABSL_ARRAYSIZE(atom_tests); i++) {
    FilterTestVars v;
    AtomTest* t = &atom_tests[i];
    size_t nregexp, natom;
    for (nregexp = 0; nregexp < ABSL_ARRAYSIZE(t->regexps); nregexp++)
      if (t->regexps[nregexp] == NULL)
        break;
    for (natom = 0; natom < ABSL_ARRAYSIZE(t->atoms); natom++)
      if (t->atoms[natom] == NULL)
        break;
    AddRegexpsAndCompile(t->regexps, nregexp, &v);
    if (!CheckExpectedAtoms(t->atoms, natom, t->testname, &v))
      nfail++;
  }
  EXPECT_EQ(0, nfail);
}

void FindAtomIndices(const std::vector<std::string>& atoms,
                     const std::vector<std::string>& matched_atoms,
                     std::vector<int>* atom_indices) {
  atom_indices->clear();
  for (size_t i = 0; i < matched_atoms.size(); i++) {
    for (size_t j = 0; j < atoms.size(); j++) {
      if (matched_atoms[i] == atoms[j]) {
        atom_indices->push_back(static_cast<int>(j));
        break;
      }
    }
  }
}

TEST(FilteredRE2Test, MatchEmptyPattern) {
  FilterTestVars v;
  AtomTest* t = &atom_tests[0];
  // We are using the regexps used in one of the atom tests
  // for this test. Adding the EXPECT here to make sure
  // the index we use for the test is for the correct test.
  EXPECT_EQ("CheckEmptyPattern", std::string(t->testname));
  size_t nregexp;
  for (nregexp = 0; nregexp < ABSL_ARRAYSIZE(t->regexps); nregexp++)
    if (t->regexps[nregexp] == NULL)
      break;
  AddRegexpsAndCompile(t->regexps, nregexp, &v);
  std::string text = "0123";
  std::vector<int> atom_ids;
  std::vector<int> matching_regexps;
  EXPECT_EQ(0, v.f.FirstMatch(text, atom_ids));
}

TEST(FilteredRE2Test, MatchTests) {
  FilterTestVars v;
  AtomTest* t = &atom_tests[2];
  // We are using the regexps used in one of the atom tests
  // for this test.
  EXPECT_EQ("SubstrAtomRemovesSuperStrInOr", std::string(t->testname));
  size_t nregexp;
  for (nregexp = 0; nregexp < ABSL_ARRAYSIZE(t->regexps); nregexp++)
    if (t->regexps[nregexp] == NULL)
      break;
  AddRegexpsAndCompile(t->regexps, nregexp, &v);

  std::string text = "abc121212xyz";
  // atoms = abc
  std::vector<int> atom_ids;
  std::vector<std::string> atoms;
  atoms.push_back("abc");
  FindAtomIndices(v.atoms, atoms, &atom_ids);
  std::vector<int> matching_regexps;
  v.f.AllMatches(text, atom_ids, &matching_regexps);
  EXPECT_EQ(size_t{1}, matching_regexps.size());

  text = "abc12312yyyzzz";
  atoms.clear();
  atoms.push_back("abc");
  atoms.push_back("yyy");
  atoms.push_back("yyyzzz");
  FindAtomIndices(v.atoms, atoms, &atom_ids);
  v.f.AllMatches(text, atom_ids, &matching_regexps);
  EXPECT_EQ(size_t{1}, matching_regexps.size());

  text = "abcd12yyy32yyyzzz";
  atoms.clear();
  atoms.push_back("abc");
  atoms.push_back("abcd");
  atoms.push_back("yyy");
  atoms.push_back("yyyzzz");
  FindAtomIndices(v.atoms, atoms, &atom_ids);
  ABSL_LOG(INFO) << "S: " << atom_ids.size();
  for (size_t i = 0; i < atom_ids.size(); i++)
    ABSL_LOG(INFO) << "i: " << i << " : " << atom_ids[i];
  v.f.AllMatches(text, atom_ids, &matching_regexps);
  EXPECT_EQ(size_t{2}, matching_regexps.size());
}

TEST(FilteredRE2Test, EmptyStringInStringSetBug) {
  // Bug due to find() finding "" at the start of everything in a string
  // set and thus SimplifyStringSet() would end up erasing everything.
  // In order to test this, we have to keep PrefilterTree from discarding
  // the OR entirely, so we have to make the minimum atom length zero.

  FilterTestVars v(0);  // override the minimum atom length
  const char* regexps[] = {"-R.+(|ADD=;AA){12}}"};
  const char* atoms[] = {"", "-r", "add=;aa", "}"};
  AddRegexpsAndCompile(regexps, ABSL_ARRAYSIZE(regexps), &v);
  EXPECT_TRUE(CheckExpectedAtoms(atoms, ABSL_ARRAYSIZE(atoms),
                                 "EmptyStringInStringSetBug", &v));
}

TEST(FilteredRE2Test, MoveSemantics) {
  FilterTestVars v1;
  int id;
  v1.f.Add("foo\\d+", v1.opts, &id);
  EXPECT_EQ(0, id);
  v1.f.Compile(&v1.atoms);
  EXPECT_EQ(size_t{1}, v1.atoms.size());
  EXPECT_EQ("foo", v1.atoms[0]);
  v1.f.AllMatches("abc foo1 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{1}, v1.matches.size());
  EXPECT_EQ(0, v1.matches[0]);
  v1.f.AllMatches("abc bar2 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{0}, v1.matches.size());

  // The moved-to object should do what the moved-from object did.
  FilterTestVars v2;
  v2.f = std::move(v1.f);
  v2.f.AllMatches("abc foo1 xyz", {0}, &v2.matches);
  EXPECT_EQ(size_t{1}, v2.matches.size());
  EXPECT_EQ(0, v2.matches[0]);
  v2.f.AllMatches("abc bar2 xyz", {0}, &v2.matches);
  EXPECT_EQ(size_t{0}, v2.matches.size());

  // The moved-from object should have been reset and be reusable.
  v1.f.Add("bar\\d+", v1.opts, &id);
  EXPECT_EQ(0, id);
  v1.f.Compile(&v1.atoms);
  EXPECT_EQ(size_t{1}, v1.atoms.size());
  EXPECT_EQ("bar", v1.atoms[0]);
  v1.f.AllMatches("abc foo1 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{0}, v1.matches.size());
  v1.f.AllMatches("abc bar2 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{1}, v1.matches.size());
  EXPECT_EQ(0, v1.matches[0]);

  // Verify that "overwriting" works and also doesn't leak memory.
  // (The latter will need a leak detector such as LeakSanitizer.)
  v1.f = std::move(v2.f);
  v1.f.AllMatches("abc foo1 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{1}, v1.matches.size());
  EXPECT_EQ(0, v1.matches[0]);
  v1.f.AllMatches("abc bar2 xyz", {0}, &v1.matches);
  EXPECT_EQ(size_t{0}, v1.matches.size());
}

}  //  namespace re2
