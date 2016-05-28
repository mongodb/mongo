// fts_query_impl_test.cpp

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

#include "mongo/bson/json.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace fts {

TEST(FTSQueryImpl, Basic1) {
    FTSQueryImpl q;
    q.setQuery("this is fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(false, q.getCaseSensitive());
    ASSERT_EQUALS(1U, q.getPositiveTerms().size());
    ASSERT_EQUALS("fun", *q.getPositiveTerms().begin());
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, ParsePunctuation) {
    FTSQueryImpl q;
    q.setQuery("hello.world");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(false, q.getCaseSensitive());
    ASSERT_EQUALS(2U, q.getPositiveTerms().size());
    ASSERT_EQUALS("hello", *q.getPositiveTerms().begin());
    ASSERT_EQUALS("world", *(--q.getPositiveTerms().end()));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, HyphenBeforeWordShouldNegateTerm) {
    FTSQueryImpl q;
    q.setQuery("-really fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(1U, q.getPositiveTerms().size());
    ASSERT_EQUALS("fun", *q.getPositiveTerms().begin());
    ASSERT_EQUALS(1U, q.getNegatedTerms().size());
    ASSERT_EQUALS("realli", *q.getNegatedTerms().begin());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, HyphenFollowedByWhitespaceShouldNotNegate) {
    FTSQueryImpl q;
    q.setQuery("- really fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    auto positiveTerms = q.getPositiveTerms();
    ASSERT_EQUALS(2U, positiveTerms.size());
    ASSERT_EQUALS(1U, positiveTerms.count("fun"));
    ASSERT_EQUALS(1U, positiveTerms.count("realli"));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, TwoHyphensShouldNegate) {
    FTSQueryImpl q;
    q.setQuery("--really fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(1U, q.getPositiveTerms().size());
    ASSERT_EQUALS("fun", *q.getPositiveTerms().begin());
    ASSERT_EQUALS(1U, q.getNegatedTerms().size());
    ASSERT_EQUALS("realli", *q.getNegatedTerms().begin());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, HyphenWithNoSurroundingWhitespaceShouldBeTreatedAsDelimiter) {
    FTSQueryImpl q;
    q.setQuery("really-fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    auto positiveTerms = q.getPositiveTerms();
    ASSERT_EQUALS(2U, positiveTerms.size());
    ASSERT_EQUALS(1U, positiveTerms.count("fun"));
    ASSERT_EQUALS(1U, positiveTerms.count("realli"));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, HyphenShouldNegateAllSucceedingTermsSeparatedByHyphens) {
    FTSQueryImpl q;
    q.setQuery("-really-fun-stuff");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    auto negatedTerms = q.getNegatedTerms();
    ASSERT_EQUALS(3U, negatedTerms.size());
    ASSERT_EQUALS(1U, negatedTerms.count("realli"));
    ASSERT_EQUALS(1U, negatedTerms.count("fun"));
    ASSERT_EQUALS(1U, negatedTerms.count("stuff"));
    ASSERT_EQUALS(0U, q.getPositiveTerms().size());
}

TEST(FTSQueryImpl, Phrase1) {
    FTSQueryImpl q;
    q.setQuery("doing a \"phrase test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(q.toBSON(),
                  fromjson("{terms: ['fun', 'phrase', 'test'], negatedTerms: [], phrases: ['phrase "
                           "test'], negatedPhrases: []}"));
    ASSERT_TRUE(q.getTermsForBounds() == q.getPositiveTerms());
}

TEST(FTSQueryImpl, Phrase2) {
    FTSQueryImpl q;
    q.setQuery("doing a \"phrase-test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(1U, q.getPositivePhr().size());
    ASSERT_EQUALS("phrase-test", q.getPositivePhr()[0]);
}

TEST(FTSQueryImpl, HyphenDirectlyBeforePhraseShouldNegateEntirePhrase) {
    FTSQueryImpl q;
    q.setQuery("doing a -\"phrase test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(
        q.toBSON(),
        fromjson(
            "{terms: ['fun'], negatedTerms: [], phrases: [], negatedPhrases: ['phrase test']}"));
}

TEST(FTSQueryImpl, HyphenSurroundedByWhitespaceBeforePhraseShouldNotNegateEntirePhrase) {
    FTSQueryImpl q;
    q.setQuery("doing a - \"phrase test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(q.toBSON(),
                  fromjson("{terms: ['fun', 'phrase', 'test'], negatedTerms: [], phrases: ['phrase "
                           "test'], negatedPhrases: []}"));
}

TEST(FTSQueryImpl, HyphenBetweenTermAndPhraseShouldBeTreatedAsDelimiter) {
    FTSQueryImpl q;
    q.setQuery("doing a-\"phrase test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(q.toBSON(),
                  fromjson("{terms: ['fun', 'phrase', 'test'], negatedTerms: [], phrases: ['phrase "
                           "test'], negatedPhrases: []}"));
}

TEST(FTSQueryImpl, HyphenShouldNegateAllSucceedingPhrasesSeparatedByHyphens) {
    FTSQueryImpl q;
    q.setQuery("-\"really fun\"-\"stuff here\" \"another phrase\"");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(q.toBSON(),
                  fromjson("{terms: ['anoth', 'phrase'], negatedTerms: [], phrases: ['another "
                           "phrase'], negatedPhrases: ['really fun', 'stuff here']}"));
}

TEST(FTSQueryImpl, CaseSensitiveOption) {
    FTSQueryImpl q;
    q.setQuery("this is fun");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(true, q.getCaseSensitive());
}

TEST(FTSQueryImpl, CaseSensitivePositiveTerms) {
    FTSQueryImpl q;
    q.setQuery("This is Positively fun");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(2U, q.getTermsForBounds().size());
    ASSERT_EQUALS(1,
                  std::count(q.getTermsForBounds().begin(), q.getTermsForBounds().end(), "posit"));
    ASSERT_EQUALS(1, std::count(q.getTermsForBounds().begin(), q.getTermsForBounds().end(), "fun"));
    ASSERT_EQUALS(2U, q.getPositiveTerms().size());
    ASSERT_EQUALS(1, std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "Posit"));
    ASSERT_EQUALS(1, std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "fun"));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
}

TEST(FTSQueryImpl, CaseSensitiveNegativeTerms) {
    FTSQueryImpl q;
    q.setQuery("-This -is -Negatively -miserable");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(0U, q.getPositiveTerms().size());
    ASSERT_EQUALS(0U, q.getTermsForBounds().size());
    ASSERT_EQUALS(2U, q.getNegatedTerms().size());
    ASSERT_EQUALS(1, std::count(q.getNegatedTerms().begin(), q.getNegatedTerms().end(), "Negat"));
    ASSERT_EQUALS(1, std::count(q.getNegatedTerms().begin(), q.getNegatedTerms().end(), "miser"));
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
}

TEST(FTSQueryImpl, CaseSensitivePositivePhrases) {
    FTSQueryImpl q;
    q.setQuery("doing a \"Phrase Test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(1U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
    ASSERT_EQUALS("Phrase Test", q.getPositivePhr()[0]);
}

TEST(FTSQueryImpl, CaseSensitiveNegativePhrases) {
    FTSQueryImpl q;
    q.setQuery("doing a -\"Phrase Test\" for fun");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(1U, q.getNegatedPhr().size());
    ASSERT_EQUALS("Phrase Test", q.getNegatedPhr()[0]);
}

TEST(FTSQueryImpl, Mix1) {
    FTSQueryImpl q;
    q.setQuery("\"industry\" -Melbourne -Physics");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_3).isOK());
    ASSERT_EQUALS(q.toBSON(),
                  fromjson("{terms: ['industri'], negatedTerms: ['melbourn', 'physic'], phrases: "
                           "['industry'], negatedPhrases: []}"));
}

TEST(FTSQueryImpl, NegPhrase2) {
    FTSQueryImpl q1, q2, q3;

    q1.setQuery("foo \"bar\"");
    q1.setLanguage("english");
    q1.setCaseSensitive(false);
    q1.setDiacriticSensitive(false);
    ASSERT(q1.parse(TEXT_INDEX_VERSION_3).isOK());

    q2.setQuery("foo \"-bar\"");
    q2.setLanguage("english");
    q2.setCaseSensitive(false);
    q2.setDiacriticSensitive(false);
    ASSERT(q2.parse(TEXT_INDEX_VERSION_3).isOK());

    q3.setQuery("foo \" -bar\"");
    q3.setLanguage("english");
    q3.setCaseSensitive(false);
    q3.setDiacriticSensitive(false);
    ASSERT(q3.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(2U, q1.getPositiveTerms().size());
    ASSERT_EQUALS(2U, q2.getPositiveTerms().size());
    ASSERT_EQUALS(2U, q3.getPositiveTerms().size());

    ASSERT_EQUALS(0U, q1.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q2.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q3.getNegatedTerms().size());

    ASSERT_EQUALS(1U, q1.getPositivePhr().size());
    ASSERT_EQUALS(1U, q2.getPositivePhr().size());
    ASSERT_EQUALS(1U, q3.getPositivePhr().size());

    ASSERT_EQUALS(0U, q1.getNegatedPhr().size());
    ASSERT_EQUALS(0U, q2.getNegatedPhr().size());
    ASSERT_EQUALS(0U, q3.getNegatedPhr().size());
}

TEST(FTSQueryImpl, NegPhrase3) {
    FTSQueryImpl q1, q2, q3;

    q1.setQuery("foo -\"bar\"");
    q1.setLanguage("english");
    q1.setCaseSensitive(false);
    q1.setDiacriticSensitive(false);
    ASSERT(q1.parse(TEXT_INDEX_VERSION_3).isOK());

    q2.setQuery("foo -\"-bar\"");
    q2.setLanguage("english");
    q2.setCaseSensitive(false);
    q2.setDiacriticSensitive(false);
    ASSERT(q2.parse(TEXT_INDEX_VERSION_3).isOK());

    q3.setQuery("foo -\" -bar\"");
    q3.setLanguage("english");
    q3.setCaseSensitive(false);
    q3.setDiacriticSensitive(false);
    ASSERT(q3.parse(TEXT_INDEX_VERSION_3).isOK());

    ASSERT_EQUALS(1U, q1.getPositiveTerms().size());
    ASSERT_EQUALS(1U, q2.getPositiveTerms().size());
    ASSERT_EQUALS(1U, q3.getPositiveTerms().size());

    ASSERT_EQUALS(0U, q1.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q2.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q3.getNegatedTerms().size());

    ASSERT_EQUALS(0U, q1.getPositivePhr().size());
    ASSERT_EQUALS(0U, q2.getPositivePhr().size());
    ASSERT_EQUALS(0U, q3.getPositivePhr().size());

    ASSERT_EQUALS(1U, q1.getNegatedPhr().size());
    ASSERT_EQUALS(1U, q2.getNegatedPhr().size());
    ASSERT_EQUALS(1U, q3.getNegatedPhr().size());
}

// Test textIndexVersion:1 query with language "english".  This invokes the standard English
// stemmer and stopword list.
TEST(FTSQueryImpl, TextIndexVersion1LanguageEnglish) {
    FTSQueryImpl q;
    q.setQuery("the running");
    q.setLanguage("english");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_1).isOK());
    ASSERT_EQUALS(1U, q.getPositiveTerms().size());
    ASSERT_EQUALS("run", *q.getPositiveTerms().begin());
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
}

// Test textIndexVersion:1 query with language "eng".  "eng" uses the English stemmer, and
// no stopword list.
TEST(FTSQueryImpl, TextIndexVersion1LanguageEng) {
    FTSQueryImpl q;
    q.setQuery("the running");
    q.setLanguage("eng");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_1).isOK());
    ASSERT_EQUALS(2U, q.getPositiveTerms().size());
    ASSERT_EQUALS(1, std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "the"));
    ASSERT_EQUALS(1, std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "run"));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
}

// Test textIndexVersion:1 query with language "invalid".  No stemming will be performed,
// and no stopword list will be used.
TEST(FTSQueryImpl, TextIndexVersion1LanguageInvalid) {
    FTSQueryImpl q;
    q.setQuery("the running");
    q.setLanguage("invalid");
    q.setCaseSensitive(false);
    q.setDiacriticSensitive(false);
    ASSERT(q.parse(TEXT_INDEX_VERSION_1).isOK());
    ASSERT_EQUALS(2U, q.getPositiveTerms().size());
    ASSERT_EQUALS(1, std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "the"));
    ASSERT_EQUALS(1,
                  std::count(q.getPositiveTerms().begin(), q.getPositiveTerms().end(), "running"));
    ASSERT_EQUALS(0U, q.getNegatedTerms().size());
    ASSERT_EQUALS(0U, q.getPositivePhr().size());
    ASSERT_EQUALS(0U, q.getNegatedPhr().size());
}

TEST(FTSQueryImpl, CloneUnparsedQuery) {
    FTSQueryImpl q;
    q.setQuery("foo");
    q.setLanguage("bar");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(true);

    auto clone = q.clone();
    ASSERT_EQUALS(clone->getQuery(), q.getQuery());
    ASSERT_EQUALS(clone->getLanguage(), q.getLanguage());
    ASSERT_EQUALS(clone->getCaseSensitive(), q.getCaseSensitive());
    ASSERT_EQUALS(clone->getDiacriticSensitive(), q.getDiacriticSensitive());
}

TEST(FTSQueryImpl, CloneParsedQuery) {
    FTSQueryImpl q;
    q.setQuery("Foo -bar \"baz\" -\"quux\"");
    q.setLanguage("english");
    q.setCaseSensitive(true);
    q.setDiacriticSensitive(true);
    ASSERT_OK(q.parse(TEXT_INDEX_VERSION_3));
    ASSERT(std::set<std::string>({"Foo", "baz"}) == q.getPositiveTerms());
    ASSERT(std::set<std::string>({"bar"}) == q.getNegatedTerms());
    ASSERT(std::vector<std::string>({"baz"}) == q.getPositivePhr());
    ASSERT(std::vector<std::string>({"quux"}) == q.getNegatedPhr());
    ASSERT(std::set<std::string>({"foo", "baz"}) == q.getTermsForBounds());

    auto clone = q.clone();
    ASSERT_EQUALS(clone->getQuery(), q.getQuery());
    ASSERT_EQUALS(clone->getLanguage(), q.getLanguage());
    ASSERT_EQUALS(clone->getCaseSensitive(), q.getCaseSensitive());
    ASSERT_EQUALS(clone->getDiacriticSensitive(), q.getDiacriticSensitive());
    FTSQueryImpl* castedClone = static_cast<FTSQueryImpl*>(clone.get());
    ASSERT(castedClone->getPositiveTerms() == q.getPositiveTerms());
    ASSERT(castedClone->getNegatedTerms() == q.getNegatedTerms());
    ASSERT(castedClone->getPositivePhr() == q.getPositivePhr());
    ASSERT(castedClone->getNegatedPhr() == q.getNegatedPhr());
    ASSERT(castedClone->getTermsForBounds() == q.getTermsForBounds());
}
}
}
