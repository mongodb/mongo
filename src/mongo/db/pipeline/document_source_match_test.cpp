/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using std::string;

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceMatchTest = AggregationContextFixture;

TEST_F(DocumentSourceMatchTest, RedactSafePortion) {
    auto expCtx = getExpCtx();
    auto assertExpectedRedactSafePortion = [&expCtx](string input, string safePortion) {
        try {
            auto match = DocumentSourceMatch::create(fromjson(input), expCtx);
            ASSERT_BSONOBJ_EQ(match->redactSafePortion(), fromjson(safePortion));
        } catch (...) {
            unittest::log() << "Problem with redactSafePortion() of: " << input;
            throw;
        }
    };

    // Empty
    assertExpectedRedactSafePortion("{}", "{}");

    // Basic allowed things
    assertExpectedRedactSafePortion("{a:1}", "{a:1}");

    assertExpectedRedactSafePortion("{a:'asdf'}", "{a:'asdf'}");

    assertExpectedRedactSafePortion("{a:/asdf/i}", "{a:/asdf/i}");

    assertExpectedRedactSafePortion("{a: {$regex: 'adsf'}}", "{a: {$regex: 'adsf'}}");

    assertExpectedRedactSafePortion("{a: {$regex: 'adsf', $options: 'i'}}",
                                    "{a: {$regex: 'adsf', $options: 'i'}}");

    assertExpectedRedactSafePortion("{a: {$mod: [1, 0]}}", "{a: {$mod: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$type: 1}}", "{a: {$type: 1}}");

    // Basic disallowed things
    assertExpectedRedactSafePortion("{a: null}", "{}");

    assertExpectedRedactSafePortion("{a: {}}", "{}");

    assertExpectedRedactSafePortion("{a: []}", "{}");

    assertExpectedRedactSafePortion("{'a.0': 1}", "{}");

    assertExpectedRedactSafePortion("{'a.0.b': 1}", "{}");

    assertExpectedRedactSafePortion("{a: {$ne: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$nin: [1, 2, 3]}}", "{}");

    assertExpectedRedactSafePortion("{a: {$exists: true}}",
                                    "{}");  // could be allowed but currently isn't

    assertExpectedRedactSafePortion("{a: {$exists: false}}", "{}");  // can never be allowed

    assertExpectedRedactSafePortion("{a: {$size: 1}}", "{}");

    assertExpectedRedactSafePortion("{$nor: [{a:1}]}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMinItems: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMaxItems: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaUniqueItems: true}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMinLength: 1}}", "{}");

    assertExpectedRedactSafePortion("{a: {$_internalSchemaMaxLength: 1}}", "{}");

    // Combinations
    assertExpectedRedactSafePortion("{a:1, b: 'asdf'}", "{a:1, b: 'asdf'}");

    assertExpectedRedactSafePortion("{a:1, b: null}", "{a:1}");

    assertExpectedRedactSafePortion("{a:null, b: null}", "{}");

    // $elemMatch

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b: 1}}}", "{a: {$elemMatch: {b: 1}}}");

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b:null}}}", "{}");

    assertExpectedRedactSafePortion("{a: {$elemMatch: {b:null, c:1}}}",
                                    "{a: {$elemMatch: {c: 1}}}");

    // explicit $and
    assertExpectedRedactSafePortion("{$and:[{a: 1}]}", "{$and:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: 1}, {b: null}]}", "{$and:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: 1}, {b: null, c:1}]}", "{$and:[{a: 1}, {c:1}]}");

    assertExpectedRedactSafePortion("{$and:[{a: null}, {b: null}]}", "{}");

    // explicit $or
    assertExpectedRedactSafePortion("{$or:[{a: 1}]}", "{$or:[{a: 1}]}");

    assertExpectedRedactSafePortion("{$or:[{a: 1}, {b: null}]}", "{}");

    assertExpectedRedactSafePortion("{$or:[{a: 1}, {b: null, c:1}]}", "{$or:[{a: 1}, {c:1}]}");

    assertExpectedRedactSafePortion("{$or:[{a: null}, {b: null}]}", "{}");

    assertExpectedRedactSafePortion("{}", "{}");

    // $all and $in
    assertExpectedRedactSafePortion("{a: {$all: [1, 0]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$all: [{$elemMatch: {b:1}}]}}",
                                    "{}");  // could be allowed but currently isn't

    assertExpectedRedactSafePortion("{a: {$all: [1, 0, null]}}", "{a: {$all: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$in: [1, 0]}}", "{a: {$in: [1, 0]}}");

    assertExpectedRedactSafePortion("{a: {$in: [1, 0, null]}}", "{}");

    {
        const char* comparisonOps[] = {"$gt", "$lt", "$gte", "$lte", NULL};
        for (int i = 0; comparisonOps[i]; i++) {
            const char* op = comparisonOps[i];
            assertExpectedRedactSafePortion(string("{a: {") + op + ": 1}}",
                                            string("{a: {") + op + ": 1}}");

            // $elemMatch takes direct expressions ...
            assertExpectedRedactSafePortion(string("{a: {$elemMatch: {") + op + ": 1}}}",
                                            string("{a: {$elemMatch: {") + op + ": 1}}}");

            // ... or top-level style full matches
            assertExpectedRedactSafePortion(string("{a: {$elemMatch: {b: {") + op + ": 1}}}}",
                                            string("{a: {$elemMatch: {b: {") + op + ": 1}}}}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": null}}", "{}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": {}}}", "{}");

            assertExpectedRedactSafePortion(string("{a: {") + op + ": []}}", "{}");

            assertExpectedRedactSafePortion(string("{'a.0': {") + op + ": null}}", "{}");

            assertExpectedRedactSafePortion(string("{'a.0.b': {") + op + ": null}}", "{}");
        }
    }
}

TEST_F(DocumentSourceMatchTest, ShouldAddDependenciesOfAllBranchesOfOrClause) {
    auto match =
        DocumentSourceMatch::create(fromjson("{$or: [{a: 1}, {'x.y': {$gt: 4}}]}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.count("x.y"));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, TextSearchShouldRequireWholeDocumentAndTextScore) {
    auto match = DocumentSourceMatch::create(fromjson("{$text: {$search: 'hello'} }"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::EXHAUSTIVE_ALL, match->getDependencies(&dependencies));
    ASSERT_EQUALS(true, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ShouldOnlyAddOuterFieldAsDependencyOfImplicitEqualityPredicate) {
    // Parses to {a: {$eq: {notAField: {$gte: 4}}}}.
    auto match = DocumentSourceMatch::create(fromjson("{a: {notAField: {$gte: 4}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ShouldAddDependenciesOfClausesWithinElemMatchAsDottedPaths) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: {$elemMatch: {c: {$gte: 4}}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a.c"));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ShouldAddOuterFieldToDependenciesIfElemMatchContainsNoFieldNames) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: {$elemMatch: {$gt: 1, $lt: 5}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ShouldAddNotClausesFieldAsDependency) {
    auto match = DocumentSourceMatch::create(fromjson("{b: {$not: {$gte: 4}}}}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ShouldAddDependenciesOfEachNorClause) {
    auto match = DocumentSourceMatch::create(
        fromjson("{$nor: [{'a.b': {$gte: 4}}, {'b.c': {$in: [1, 2]}}]}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a.b"));
    ASSERT_EQUALS(1U, dependencies.fields.count("b.c"));
    ASSERT_EQUALS(2U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, CommentShouldNotAddAnyDependencies) {
    auto match = DocumentSourceMatch::create(fromjson("{$comment: 'misleading?'}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(0U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, ClauseAndedWithCommentShouldAddDependencies) {
    auto match =
        DocumentSourceMatch::create(fromjson("{a: 4, $comment: 'irrelevant'}"), getExpCtx());
    DepsTracker dependencies;
    ASSERT_EQUALS(DocumentSource::SEE_NEXT, match->getDependencies(&dependencies));
    ASSERT_EQUALS(1U, dependencies.fields.count("a"));
    ASSERT_EQUALS(1U, dependencies.fields.size());
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(false, dependencies.getNeedTextScore());
}

TEST_F(DocumentSourceMatchTest, MultipleMatchStagesShouldCombineIntoOne) {
    auto match1 = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    auto match2 = DocumentSourceMatch::create(BSON("b" << 1), getExpCtx());
    auto match3 = DocumentSourceMatch::create(BSON("c" << 1), getExpCtx());

    Pipeline::SourceContainer container;

    // Check initial state
    ASSERT_BSONOBJ_EQ(match1->getQuery(), BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(match2->getQuery(), BSON("b" << 1));
    ASSERT_BSONOBJ_EQ(match3->getQuery(), BSON("c" << 1));

    container.push_back(match1);
    container.push_back(match2);
    match1->optimizeAt(container.begin(), &container);

    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_BSONOBJ_EQ(match1->getQuery(), fromjson("{'$and': [{a:1}, {b:1}]}"));

    container.push_back(match3);
    match1->optimizeAt(container.begin(), &container);
    ASSERT_EQUALS(container.size(), 1U);
    ASSERT_BSONOBJ_EQ(match1->getQuery(),
                      fromjson("{'$and': [{'$and': [{a:1}, {b:1}]},"
                               "{c:1}]}"));
}

TEST_F(DocumentSourceMatchTest, ShouldPropagatePauses) {
    auto match = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    auto mock = DocumentSourceMock::create({DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 1}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 2}},
                                            Document{{"a", 2}},
                                            DocumentSource::GetNextResult::makePauseExecution(),
                                            Document{{"a", 1}}});
    match->setSource(mock.get());

    ASSERT_TRUE(match->getNext().isPaused());
    ASSERT_TRUE(match->getNext().isAdvanced());
    ASSERT_TRUE(match->getNext().isPaused());

    // {a: 2} doesn't match, should go directly to the next pause.
    ASSERT_TRUE(match->getNext().isPaused());
    ASSERT_TRUE(match->getNext().isAdvanced());
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldCorrectlyJoinWithSubsequentMatch) {
    const auto match = DocumentSourceMatch::create(BSON("a" << 1), getExpCtx());
    const auto secondMatch = DocumentSourceMatch::create(BSON("b" << 1), getExpCtx());

    match->joinMatchWith(secondMatch);

    const auto mock = DocumentSourceMock::create({Document{{"a", 1}, {"b", 1}},
                                                  Document{{"a", 2}, {"b", 1}},
                                                  Document{{"a", 1}, {"b", 2}},
                                                  Document{{"a", 2}, {"b", 2}}});

    match->setSource(mock.get());

    // The first result should match.
    auto next = match->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", 1}, {"b", 1}}));

    // The rest should not match.
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
}

DEATH_TEST_F(DocumentSourceMatchTest,
             ShouldFailToDescendExpressionOnPathThatIsNotACommonPrefix,
             "Invariant failure expression::isPathPrefixOf") {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a.b" << 1 << "b.c" << 1);
    const auto matchExpression = unittest::assertGet(
        MatchExpressionParser::parse(matchSpec, ExtensionsCallbackNoop(), expCtx->getCollator()));
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

DEATH_TEST_F(DocumentSourceMatchTest,
             ShouldFailToDescendExpressionOnPathThatContainsElemMatchWithObject,
             "Invariant failure node->matchType()") {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a" << BSON("$elemMatch" << BSON("a.b" << 1)));
    const auto matchExpression = unittest::assertGet(
        MatchExpressionParser::parse(matchSpec, ExtensionsCallbackNoop(), expCtx->getCollator()));
    BSONObjBuilder out;
    matchExpression->serialize(&out);
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

// Due to the order of traversal of the MatchExpression tree, this test may actually trigger the
// invariant failure that the path being descended is not a prefix of the path of the
// MatchExpression node corresponding to the '$gt' expression, which will report an empty path.
DEATH_TEST_F(DocumentSourceMatchTest,
             ShouldFailToDescendExpressionOnPathThatContainsElemMatchWithValue,
             "Invariant failure") {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a" << BSON("$elemMatch" << BSON("$gt" << 0)));
    const auto matchExpression = unittest::assertGet(
        MatchExpressionParser::parse(matchSpec, ExtensionsCallbackNoop(), expCtx->getCollator()));
    DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
}

TEST_F(DocumentSourceMatchTest, ShouldMatchCorrectlyAfterDescendingMatch) {
    const auto expCtx = getExpCtx();
    const auto matchSpec = BSON("a.b" << 1 << "a.c" << 1 << "a.d" << 1);
    const auto matchExpression = unittest::assertGet(
        MatchExpressionParser::parse(matchSpec, ExtensionsCallbackNoop(), expCtx->getCollator()));

    const auto descendedMatch =
        DocumentSourceMatch::descendMatchOnPath(matchExpression.get(), "a", expCtx);
    const auto mock = DocumentSourceMock::create(
        {Document{{"b", 1}, {"c", 1}, {"d", 1}},
         Document{{"b", 1}, {"a", Document{{"c", 1}}}, {"d", 1}},
         Document{{"a", Document{{"b", 1}}}, {"a", Document{{"c", 1}}}, {"d", 1}},
         Document{
             {"a", Document{{"b", 1}}}, {"a", Document{{"c", 1}}}, {"a", Document{{"d", 1}}}}});
    descendedMatch->setSource(mock.get());

    auto next = descendedMatch->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"b", 1}, {"c", 1}, {"d", 1}}));

    ASSERT_TRUE(descendedMatch->getNext().isEOF());
    ASSERT_TRUE(descendedMatch->getNext().isEOF());
    ASSERT_TRUE(descendedMatch->getNext().isEOF());
}

TEST_F(DocumentSourceMatchTest, ShouldCorrectlyEvaluateElemMatchPredicate) {
    const auto match =
        DocumentSourceMatch::create(BSON("a" << BSON("$elemMatch" << BSON("b" << 1))), getExpCtx());

    const std::vector<Document> matchingVector = {Document{{"b", 0}}, Document{{"b", 1}}};
    const std::vector<Document> nonMatchingVector = {Document{{"b", 0}}, Document{{"b", 2}}};
    const auto mock = DocumentSourceMock::create(
        {Document{{"a", matchingVector}}, Document{{"a", nonMatchingVector}}, Document{{"a", 1}}});

    match->setSource(mock.get());

    // The first result should match.
    auto next = match->getNext();
    ASSERT_TRUE(next.isAdvanced());
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), (Document{{"a", matchingVector}}));

    // The rest should not match.
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
    ASSERT_TRUE(match->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
