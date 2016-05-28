// fts_spec_test.cpp

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::set;
using std::string;
using unittest::assertGet;

namespace fts {

/**
 * Assert that fixSpec() accepts the provided text index spec.
 */
void assertFixSuccess(const std::string& s) {
    BSONObj user = fromjson(s);

    try {
        // fixSpec() should not throw on a valid spec.
        BSONObj fixed = assertGet(FTSSpec::fixSpec(user));

        // fixSpec() on an already-fixed spec shouldn't change it.
        BSONObj fixed2 = assertGet(FTSSpec::fixSpec(fixed));
        ASSERT_EQUALS(fixed, fixed2);
    } catch (UserException&) {
        ASSERT(false);
    }
}

/**
 * Assert that fixSpec() rejects the provided text index spec.
 */
void assertFixFailure(const std::string& s) {
    BSONObj user = fromjson(s);
    ASSERT_NOT_OK(FTSSpec::fixSpec(user));
}

TEST(FTSSpec, FixNormalKey1) {
    assertFixSuccess("{key: {a: 'text'}}");
    assertFixSuccess("{key: {a: 'text', b: 'text'}}");
    assertFixSuccess("{key: {a: 'text', b: 'text', c: 'text'}}");

    assertFixFailure("{key: {_fts: 'text'}}");  // not allowed to index reserved field
    assertFixFailure("{key: {_ftsx: 'text'}}");
}

TEST(FTSSpec, FixCompoundKey1) {
    assertFixSuccess("{key: {a: 'text', b: 1.0}}");
    assertFixSuccess("{key: {a: 'text', b: NumberInt(1)}}");
    assertFixSuccess("{key: {a: 'text', b: NumberLong(1)}}");
    assertFixSuccess("{key: {a: 'text', b: -1.0}}");
    assertFixSuccess("{key: {a: 'text', b: NumberInt(-1)}}");
    assertFixSuccess("{key: {a: 'text', b: NumberLong(-1)}}");
    assertFixSuccess("{key: {a: 1.0, b: 'text'}}");
    assertFixSuccess("{key: {a: NumberInt(1), b: 'text'}}");
    assertFixSuccess("{key: {a: NumberLong(1), b: 'text'}}");
    assertFixSuccess("{key: {a: -1, b: 'text'}}");
    assertFixSuccess("{key: {a: 1, b: 1, c: 'text'}}");
    assertFixSuccess("{key: {a: 1, b: -1, c: 'text'}}");
    assertFixSuccess("{key: {a: -1, b: 1, c: 'text'}}");
    assertFixSuccess("{key: {a: 1, b: 'text', c: 1}}");
    assertFixSuccess("{key: {a: 'text', b: 1, c: 1}}");
    assertFixSuccess("{key: {a: 'text', b: 1, c: -1}}");
    assertFixSuccess("{key: {a: 'text', b: 'text', c: 1}}");
    assertFixSuccess("{key: {a: 1, b: 'text', c: 'text'}}");

    assertFixFailure("{key: {a: 'text', b: 0}}");
    assertFixFailure("{key: {a: 'text', b: '2d'}}");  // not allowed to mix special indexes
    assertFixFailure("{key: {a: 'text', b: '1'}}");
    assertFixFailure("{key: {a: 'text', _fts: 1}}");
    assertFixFailure("{key: {a: 'text', _fts: 'text'}}");
    assertFixFailure("{key: {a: 'text', _ftsx: 1}}");
    assertFixFailure("{key: {a: 'text', _ftsx: 'text'}}");
    assertFixFailure("{key: {_fts: 1, a: 'text'}}");
    assertFixFailure("{key: {_fts: 'text', a: 'text'}}");
    assertFixFailure("{key: {_ftsx: 1, a: 'text'}}");
    assertFixFailure("{key: {_ftsx: 'text', a: 'text'}}");
    assertFixFailure("{key: {a: 'text', b: 1, c: 'text'}}");  // 'text' must all be adjacent
    assertFixFailure("{key: {a: 'text', b: 1, c: 'text', d: 1}}");
    assertFixFailure("{key: {a: 1, b: 'text', c: 1, d: 'text', e: 1}}");
}

TEST(FTSSpec, FixDefaultLanguage1) {
    assertFixSuccess("{key: {a: 'text'}, default_language: 'english'}");
    assertFixSuccess("{key: {a: 'text'}, default_language: 'engLISH'}");
    assertFixSuccess("{key: {a: 'text'}, default_language: 'en'}");
    assertFixSuccess("{key: {a: 'text'}, default_language: 'eN'}");
    assertFixSuccess("{key: {a: 'text'}, default_language: 'spanish'}");
    assertFixSuccess("{key: {a: 'text'}, default_language: 'none'}");

    assertFixFailure("{key: {a: 'text'}, default_language: 'engrish'}");
    assertFixFailure("{key: {a: 'text'}, default_language: ' english'}");
    assertFixFailure("{key: {a: 'text'}, default_language: ''}");
}

TEST(FTSSpec, FixWeights1) {
    assertFixSuccess("{key: {a: 'text'}, weights: {}}");
    assertFixSuccess("{key: {a: 'text'}, weights: {a: 1.0}}");
    assertFixSuccess("{key: {a: 'text'}, weights: {a: NumberInt(1)}}");
    assertFixSuccess("{key: {a: 'text'}, weights: {a: NumberLong(1)}}");
    assertFixSuccess("{key: {a: 'text'}, weights: {a: 99999}}");
    assertFixSuccess("{key: {'$**': 'text'}, weights: {'a.b': 2}}");
    assertFixSuccess("{key: {'$**': 'text'}, weights: {a: 2, b: 2}}");
    assertFixSuccess("{key: {'$**': 'text'}, weights: {'$**': 2}}");

    assertFixFailure("{key: {a: 'text'}, weights: 0}");
    assertFixFailure("{key: {a: 'text'}, weights: []}");
    assertFixFailure("{key: {a: 'text'}, weights: 'x'}");
    assertFixFailure("{key: {a: 'text'}, weights: {a: 0}}");
    assertFixFailure("{key: {a: 'text'}, weights: {a: -1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {a: 100000}}");  // above max weight
    assertFixFailure("{key: {a: 'text'}, weights: {a: '1'}}");
    assertFixFailure("{key: {a: 'text'}, weights: {'': 1}}");  // "invalid" path
    assertFixFailure("{key: {a: 'text'}, weights: {'a.': 1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {'.a': 1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {'a..a': 1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {$a: 1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {'a.$a': 1}}");
    assertFixFailure("{key: {a: 'text'}, weights: {'a.$**': 1}}");

    assertFixFailure("{key: {_fts: 'text', _ftsx: 1}, weights: {}}");
    assertFixFailure("{key: {_fts: 'text', _ftsx: 1}}");
}

TEST(FTSSpec, FixLanguageOverride1) {
    assertFixSuccess("{key: {a: 'text'}, language_override: 'foo'}");
    assertFixSuccess("{key: {a: 'text'}, language_override: 'foo$bar'}");

    assertFixFailure("{key: {a: 'text'}, language_override: 'foo.bar'}");  // can't have '.'
    assertFixFailure("{key: {a: 'text'}, language_override: ''}");
    assertFixFailure("{key: {a: 'text'}, language_override: '$foo'}");
}

TEST(FTSSpec, FixTextIndexVersion1) {
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: 1.0}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberInt(1)}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberLong(1)}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: 2.0}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberInt(2)}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberLong(2)}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: 3.0}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberInt(3)}}");
    assertFixSuccess("{key: {a: 'text'}, textIndexVersion: NumberLong(3)}}");

    assertFixFailure("{key: {a: 'text'}, textIndexVersion: 4}");
    assertFixFailure("{key: {a: 'text'}, textIndexVersion: '2'}");
    assertFixFailure("{key: {a: 'text'}, textIndexVersion: {}}");
}

TEST(FTSSpec, ScoreSingleField1) {
    BSONObj user = BSON("key" << BSON("title"
                                      << "text"
                                      << "text"
                                      << "text")
                              << "weights"
                              << BSON("title" << 10));

    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    TermFrequencyMap m;
    spec.scoreDocument(BSON("title"
                            << "cat sat run"),
                       &m);
    ASSERT_EQUALS(3U, m.size());
    ASSERT_EQUALS(m["cat"], m["sat"]);
    ASSERT_EQUALS(m["cat"], m["run"]);
    ASSERT(m["cat"] > 0);
}

TEST(FTSSpec, ScoreMultipleField1) {
    BSONObj user = BSON("key" << BSON("title"
                                      << "text"
                                      << "text"
                                      << "text")
                              << "weights"
                              << BSON("title" << 10));

    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    TermFrequencyMap m;
    spec.scoreDocument(BSON("title"
                            << "cat sat run"
                            << "text"
                            << "cat book"),
                       &m);

    ASSERT_EQUALS(4U, m.size());
    ASSERT_EQUALS(m["sat"], m["run"]);
    ASSERT(m["sat"] > 0);

    ASSERT(m["cat"] > m["sat"]);
    ASSERT(m["cat"] > m["book"]);
    ASSERT(m["book"] > 0);
    ASSERT(m["book"] < m["sat"]);
}

TEST(FTSSpec, ScoreMultipleField2) {
    // Test where one indexed field is a parent component of another indexed field.
    BSONObj user = BSON("key" << BSON("a"
                                      << "text"
                                      << "a.b"
                                      << "text"));

    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    TermFrequencyMap m;
    spec.scoreDocument(BSON("a" << BSON("b"
                                        << "term")),
                       &m);
    ASSERT_EQUALS(1U, m.size());
}

TEST(FTSSpec, ScoreRepeatWord) {
    BSONObj user = BSON("key" << BSON("title"
                                      << "text"
                                      << "text"
                                      << "text")
                              << "weights"
                              << BSON("title" << 10));

    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    TermFrequencyMap m;
    spec.scoreDocument(BSON("title"
                            << "cat sat sat run run run"),
                       &m);
    ASSERT_EQUALS(3U, m.size());
    ASSERT(m["cat"] > 0);
    ASSERT(m["sat"] > m["cat"]);
    ASSERT(m["run"] > m["sat"]);
}

TEST(FTSSpec, Extra1) {
    BSONObj user = BSON("key" << BSON("data"
                                      << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));
    ASSERT_EQUALS(0U, spec.numExtraBefore());
    ASSERT_EQUALS(0U, spec.numExtraAfter());
}

TEST(FTSSpec, Extra2) {
    BSONObj user = BSON("key" << BSON("data"
                                      << "text"
                                      << "x"
                                      << 1));
    BSONObj fixed = assertGet(FTSSpec::fixSpec(user));
    FTSSpec spec(fixed);
    ASSERT_EQUALS(0U, spec.numExtraBefore());
    ASSERT_EQUALS(1U, spec.numExtraAfter());
    ASSERT_EQUALS(StringData("x"), spec.extraAfter(0));

    BSONObj fixed2 = assertGet(FTSSpec::fixSpec(fixed));
    ASSERT_EQUALS(fixed, fixed2);
}

TEST(FTSSpec, Extra3) {
    BSONObj user = BSON("key" << BSON("x" << 1 << "data"
                                          << "text"));
    BSONObj fixed = assertGet(FTSSpec::fixSpec(user));

    ASSERT_EQUALS(BSON("x" << 1 << "_fts"
                           << "text"
                           << "_ftsx"
                           << 1),
                  fixed["key"].Obj());
    ASSERT_EQUALS(BSON("data" << 1), fixed["weights"].Obj());

    BSONObj fixed2 = assertGet(FTSSpec::fixSpec(fixed));
    ASSERT_EQUALS(fixed, fixed2);

    FTSSpec spec(fixed);
    ASSERT_EQUALS(1U, spec.numExtraBefore());
    ASSERT_EQUALS(StringData("x"), spec.extraBefore(0));
    ASSERT_EQUALS(0U, spec.numExtraAfter());

    BSONObj prefix;

    ASSERT(spec.getIndexPrefix(BSON("x" << 2), &prefix).isOK());
    ASSERT_EQUALS(BSON("x" << 2), prefix);

    ASSERT(spec.getIndexPrefix(BSON("x" << 3 << "y" << 4), &prefix).isOK());
    ASSERT_EQUALS(BSON("x" << 3), prefix);

    ASSERT(!spec.getIndexPrefix(BSON("x" << BSON("$gt" << 5)), &prefix).isOK());
    ASSERT(!spec.getIndexPrefix(BSON("y" << 4), &prefix).isOK());
    ASSERT(!spec.getIndexPrefix(BSONObj(), &prefix).isOK());
}

// Test for correct behavior when encountering nested arrays (both directly nested and
// indirectly nested).

TEST(FTSSpec, NestedArraysPos1) {
    BSONObj user = BSON("key" << BSON("a.b"
                                      << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    // The following document matches {"a.b": {$type: 2}}, so "term" should be indexed.
    BSONObj obj = fromjson("{a: [{b: ['term']}]}");  // indirectly nested arrays
    TermFrequencyMap m;
    spec.scoreDocument(obj, &m);
    ASSERT_EQUALS(1U, m.size());
}

TEST(FTSSpec, NestedArraysPos2) {
    BSONObj user = BSON("key" << BSON("$**"
                                      << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    // The wildcard spec implies a full recursive traversal, so "term" should be indexed.
    BSONObj obj = fromjson("{a: {b: [['term']]}}");  // directly nested arrays
    TermFrequencyMap m;
    spec.scoreDocument(obj, &m);
    ASSERT_EQUALS(1U, m.size());
}

TEST(FTSSpec, NestedArraysNeg1) {
    BSONObj user = BSON("key" << BSON("a.b"
                                      << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(user)));

    // The following document does not match {"a.b": {$type: 2}}, so "term" should not be
    // indexed.
    BSONObj obj = fromjson("{a: {b: [['term']]}}");  // directly nested arrays
    TermFrequencyMap m;
    spec.scoreDocument(obj, &m);
    ASSERT_EQUALS(0U, m.size());
}

// Multi-language test_1: test independent stemming per sub-document
TEST(FTSSpec, NestedLanguages_PerArrayItemStemming) {
    BSONObj indexSpec = BSON("key" << BSON("a.b.c"
                                           << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ a :"
        "  { b :"
        "    [ { c : \"walked\", language : \"english\" },"
        "      { c : \"camminato\", language : \"italian\" },"
        "      { c : \"ging\", language : \"german\" } ]"
        "   }"
        " }");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("walk");
    hits.insert("cammin");
    hits.insert("ging");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

// Multi-language test_2: test nested stemming per sub-document
TEST(FTSSpec, NestedLanguages_PerSubdocStemming) {
    BSONObj indexSpec = BSON("key" << BSON("a.b.c"
                                           << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  a :"
        "  { language : \"danish\","
        "    b :"
        "    [ { c : \"foredrag\" },"
        "      { c : \"foredragsholder\" },"
        "      { c : \"lector\" } ]"
        "  }"
        "}");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("foredrag");
    hits.insert("foredragshold");
    hits.insert("lector");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

// Multi-language test_3: test nested arrays
TEST(FTSSpec, NestedLanguages_NestedArrays) {
    BSONObj indexSpec = BSON("key" << BSON("a.b.c"
                                           << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  a : ["
        "  { language : \"danish\","
        "    b :"
        "    [ { c : [\"foredrag\"] },"
        "      { c : [\"foredragsholder\"] },"
        "      { c : [\"lector\"] } ]"
        "  } ]"
        "}");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("foredrag");
    hits.insert("foredragshold");
    hits.insert("lector");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

// Multi-language test_4: test pruning
TEST(FTSSpec, NestedLanguages_PathPruning) {
    BSONObj indexSpec = BSON("key" << BSON("a.b.c"
                                           << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  a : "
        "  { language : \"danish\","
        "    bc : \"foo\","
        "    b : { d: \"bar\" },"
        "    b :"
        "    [ { c : \"foredrag\" },"
        "      { c : \"foredragsholder\" },"
        "      { c : \"lector\" } ]"
        "  }"
        "}");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("foredrag");
    hits.insert("foredragshold");
    hits.insert("lector");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

// Multi-language test_5: test wildcard spec
TEST(FTSSpec, NestedLanguages_Wildcard) {
    BSONObj indexSpec = BSON("key" << BSON("$**"
                                           << "text"));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  b : \"walking\","
        "  c : { e: \"walked\" },"
        "  d : "
        "  { language : \"danish\","
        "    e :"
        "    [ { f : \"foredrag\" },"
        "      { f : \"foredragsholder\" },"
        "      { f : \"lector\" } ]"
        "  }"
        "}");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("foredrag");
    hits.insert("foredragshold");
    hits.insert("lector");
    hits.insert("walk");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

// Multi-language test_6: test wildcard spec with override
TEST(FTSSpec, NestedLanguages_WildcardOverride) {
    BSONObj indexSpec = BSON("key" << BSON("$**"
                                           << "text")
                                   << "weights"
                                   << BSON("d.e.f" << 20));
    FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
    TermFrequencyMap tfm;

    BSONObj obj = fromjson(
        "{ language : \"english\","
        "  b : \"walking\","
        "  c : { e: \"walked\" },"
        "  d : "
        "  { language : \"danish\","
        "    e :"
        "    [ { f : \"foredrag\" },"
        "      { f : \"foredragsholder\" },"
        "      { f : \"lector\" } ]"
        "  }"
        "}");

    spec.scoreDocument(obj, &tfm);

    set<string> hits;
    hits.insert("foredrag");
    hits.insert("foredragshold");
    hits.insert("lector");
    hits.insert("walk");

    for (TermFrequencyMap::const_iterator i = tfm.begin(); i != tfm.end(); ++i) {
        string term = i->first;
        ASSERT_EQUALS(1U, hits.count(term));
    }
}

/** Test differences across textIndexVersion values in handling of nested arrays. */
TEST(FTSSpec, TextIndexLegacyNestedArrays) {
    BSONObj obj = fromjson("{a: [{b: ['hello']}]}");

    // textIndexVersion=1 FTSSpec objects do not index nested arrays.
    {
        BSONObj indexSpec = fromjson("{key: {'a.b': 'text'}, textIndexVersion: 1}");
        FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
        TermFrequencyMap tfm;
        spec.scoreDocument(obj, &tfm);
        ASSERT_EQUALS(tfm.size(), 0U);
    }

    // textIndexVersion=2 FTSSpec objects do index nested arrays.
    {
        BSONObj indexSpec = fromjson("{key: {'a.b': 'text'}, textIndexVersion: 2}");
        FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
        TermFrequencyMap tfm;
        spec.scoreDocument(obj, &tfm);
        ASSERT_EQUALS(tfm.size(), 1U);
    }
}

/** Test differences across textIndexVersion values in handling of language annotations. */
TEST(FTSSpec, TextIndexLegacyLanguageRecognition) {
    BSONObj obj = fromjson("{a: 'the', language: 'EN'}");

    // textIndexVersion=1 FTSSpec objects treat two-letter language annotations as "none"
    // for purposes of stopword processing.
    {
        BSONObj indexSpec = fromjson("{key: {'a': 'text'}, textIndexVersion: 1}");
        FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
        TermFrequencyMap tfm;
        spec.scoreDocument(obj, &tfm);
        ASSERT_EQUALS(tfm.size(), 1U);  // "the" not recognized as stopword
    }

    // textIndexVersion=2 FTSSpec objects recognize two-letter codes.
    {
        BSONObj indexSpec = fromjson("{key: {'a': 'text'}, textIndexVersion: 2}");
        FTSSpec spec(assertGet(FTSSpec::fixSpec(indexSpec)));
        TermFrequencyMap tfm;
        spec.scoreDocument(obj, &tfm);
        ASSERT_EQUALS(tfm.size(), 0U);  // "the" recognized as stopword
    }
}
}
}
