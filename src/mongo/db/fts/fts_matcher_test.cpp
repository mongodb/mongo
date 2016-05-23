// fts_matcher_test.cpp

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

#include "mongo/db/fts/fts_matcher.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

using unittest::assertGet;

TEST(FTSMatcher, NegWild1) {
    FTSQueryImpl q;
    q.setQuery("foo -bar");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("$**"
                                                                       << "text"))))));

    ASSERT(m.hasNegativeTerm(BSON("x" << BSON("y"
                                              << "bar"))));
    ASSERT(m.hasNegativeTerm(BSON("x" << BSON("y"
                                              << "bar"))));
}

// Regression test for SERVER-11994.
TEST(FTSMatcher, NegWild2) {
    FTSQueryImpl q;
    q.setQuery("pizza -restaurant");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("$**"
                                                                       << "text"))))));

    ASSERT(m.hasNegativeTerm(BSON("x" << BSON("y"
                                              << "pizza restaurant"))));
    ASSERT(m.hasNegativeTerm(BSON("x" << BSON("y"
                                              << "PIZZA RESTAURANT"))));
}

TEST(FTSMatcher, Phrase1) {
    FTSQueryImpl q;
    q.setQuery("foo \"table top\"");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("$**"
                                                                       << "text"))))));

    ASSERT(m.positivePhrasesMatch(BSON("x"
                                       << "table top")));
    ASSERT(m.positivePhrasesMatch(BSON("x"
                                       << " asd table top asd")));
    ASSERT(!m.positivePhrasesMatch(BSON("x"
                                        << "tablz top")));
    ASSERT(!m.positivePhrasesMatch(BSON("x"
                                        << " asd tablz top asd")));

    ASSERT(m.positivePhrasesMatch(BSON("x"
                                       << "table top")));
    ASSERT(!m.positivePhrasesMatch(BSON("x"
                                        << "table a top")));
}

TEST(FTSMatcher, Phrase2) {
    FTSQueryImpl q;
    q.setQuery("foo \"table top\"");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));
    ASSERT(m.positivePhrasesMatch(BSON("x" << BSON_ARRAY("table top"))));
}

// Test that the matcher parses the document with the document language, not the search
// language.
TEST(FTSMatcher, ParsesUsingDocLanguage) {
    FTSQueryImpl q;
    q.setQuery("-glad");
    q.setLanguage("none");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    // Even though the search language is "none", the document {x: "gladly"} should be
    // parsed using the English stemmer, and as such should match the negated term "glad".
    ASSERT(m.hasNegativeTerm(BSON("x"
                                  << "gladly")));
}

// Test the matcher does not filter out stop words from positive terms
TEST(FTSMatcher, MatcherDoesNotFilterStopWordsNeg) {
    FTSQueryImpl q;
    q.setQuery("-the");
    q.setLanguage("none");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    ASSERT(m.hasNegativeTerm(BSON("x"
                                  << "the")));
}

// Test the matcher does not filter out stop words from negative terms
TEST(FTSMatcher, MatcherDoesNotFilterStopWordsPos) {
    FTSQueryImpl q;
    q.setQuery("the");
    q.setLanguage("none");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    ASSERT(m.hasPositiveTerm(BSON("x"
                                  << "the")));
}

// Returns whether a document indexed with text data 'doc' contains any positive terms from
// case-sensitive text query 'search'.
static bool docHasPositiveTermWithCase(const std::string& doc, const std::string& search) {
    FTSQueryImpl q;
    q.setQuery(search);
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    return m.hasPositiveTerm(BSON("x" << doc));
}

TEST(FTSMatcher, HasPositiveTermCaseSensitive) {
    ASSERT_TRUE(docHasPositiveTermWithCase("hello world", "hello"));
    ASSERT_TRUE(docHasPositiveTermWithCase("Hello World", "Hello"));
    ASSERT_TRUE(docHasPositiveTermWithCase("Hello World", "World Hello"));
    ASSERT_TRUE(docHasPositiveTermWithCase("Hello World", "World GoodBye"));
    ASSERT_TRUE(docHasPositiveTermWithCase("John Runs", "Runs"));
    ASSERT_TRUE(docHasPositiveTermWithCase("John Runs", "Running"));
    ASSERT_TRUE(docHasPositiveTermWithCase("John Runs", "Run"));

    ASSERT_FALSE(docHasPositiveTermWithCase("John Runs", "run"));
    ASSERT_FALSE(docHasPositiveTermWithCase("Hello World", "HELLO"));
    ASSERT_FALSE(docHasPositiveTermWithCase("hello world", "Hello"));
    ASSERT_FALSE(docHasPositiveTermWithCase("Hello World", "hello"));
}

// Returns whether a document indexed with text data 'doc' contains any negative terms from
// case-sensitive text query 'search'.
static bool docHasNegativeTermWithCase(const std::string& doc, const std::string& search) {
    FTSQueryImpl q;
    q.setQuery(search);
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    return m.hasNegativeTerm(BSON("x" << doc));
}

TEST(FTSMatcher, HasNegativeTermCaseSensitive) {
    ASSERT_TRUE(docHasNegativeTermWithCase("hello world", "hello -world"));
    ASSERT_TRUE(docHasNegativeTermWithCase("Hello World", "Hello -World"));
    ASSERT_TRUE(docHasNegativeTermWithCase("Hello World", "-World -Hello"));
    ASSERT_TRUE(docHasNegativeTermWithCase("Hello World", "-Goodbye -World"));
    ASSERT_TRUE(docHasNegativeTermWithCase("John Runs", "-Runs"));
    ASSERT_TRUE(docHasNegativeTermWithCase("John Runs", "-Running"));
    ASSERT_TRUE(docHasNegativeTermWithCase("John Runs", "-Run"));

    ASSERT_FALSE(docHasNegativeTermWithCase("John Runs", "-run"));
    ASSERT_FALSE(docHasNegativeTermWithCase("Hello World", "Hello -WORLD"));
    ASSERT_FALSE(docHasNegativeTermWithCase("hello world", "hello -World"));
    ASSERT_FALSE(docHasNegativeTermWithCase("Hello World", "Hello -world"));
}

// Returns whether a document indexed with text data 'doc' contains all positive phrases
// from case-sensitive text query 'search'.
static bool docPositivePhrasesMatchWithCase(const std::string& doc, const std::string& search) {
    FTSQueryImpl q;
    q.setQuery(search);
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    return m.positivePhrasesMatch(BSON("x" << doc));
}

TEST(FTSMatcher, PositivePhrasesMatchWithCase) {
    ASSERT_TRUE(docPositivePhrasesMatchWithCase("John Runs", "\"John Runs\""));
    ASSERT_TRUE(docPositivePhrasesMatchWithCase("John Runs", "\"John Run\""));
    ASSERT_TRUE(docPositivePhrasesMatchWithCase("John Runs", "\"John\" \"Run\""));
    ASSERT_TRUE(docPositivePhrasesMatchWithCase("John Runs", "\"n R\""));

    ASSERT_FALSE(docPositivePhrasesMatchWithCase("John Runs", "\"john runs\""));
    ASSERT_FALSE(docPositivePhrasesMatchWithCase("john runs", "\"John Runs\""));
    ASSERT_FALSE(docPositivePhrasesMatchWithCase("John Runs", "\"John\" \"Running\""));
}

// Returns whether a document indexed with text data 'doc' contains zero negative phrases
// from case-sensitive text query 'search'.
static bool docNegativePhrasesMatchWithCase(const std::string& doc, const std::string& search) {
    FTSQueryImpl q;
    q.setQuery(search);
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    FTSMatcher m(q,
                 FTSSpec(assertGet(FTSSpec::fixSpec(BSON("key" << BSON("x"
                                                                       << "text"))))));

    return m.negativePhrasesMatch(BSON("x" << doc));
}

TEST(FTSMatcher, NegativePhrasesMatchWithCase) {
    ASSERT_TRUE(docNegativePhrasesMatchWithCase("John Runs", "-\"john runs\""));
    ASSERT_TRUE(docNegativePhrasesMatchWithCase("john runs", "-\"John Runs\""));
    ASSERT_TRUE(docNegativePhrasesMatchWithCase("john runs", "-\"John\" -\"Runs\""));

    ASSERT_FALSE(docNegativePhrasesMatchWithCase("John Runs", "-\"John Runs\""));
    ASSERT_FALSE(docNegativePhrasesMatchWithCase("John Runs", "-\"John Run\""));
    ASSERT_FALSE(docNegativePhrasesMatchWithCase("John Runs", "-\"John\" -\"Run\""));
    ASSERT_FALSE(docNegativePhrasesMatchWithCase("John Runs", "-\"n R\""));
    ASSERT_FALSE(docNegativePhrasesMatchWithCase("John Runs", "-\"John\" -\"Running\""));
}
}
}
