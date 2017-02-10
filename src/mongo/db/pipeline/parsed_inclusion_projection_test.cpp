/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#include "mongo/db/pipeline/parsed_inclusion_projection.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {
using std::vector;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

TEST(InclusionProjection, ShouldThrowWhenParsingInvalidExpression) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(inclusion.parse(expCtx,
                                  BSON("a" << BSON("$gt" << BSON("bad"
                                                                 << "arguments")))),
                  UserException);
}

TEST(InclusionProjection, ShouldRejectProjectionWithNoOutputFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_THROWS(inclusion.parse(expCtx, BSON("_id" << false)), UserException);
}

TEST(InclusionProjection, ShouldAddIncludedFieldsToDependencies) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("_id" << false << "a" << true << "x.y" << true));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("x.y"), 1UL);
}

TEST(InclusionProjection, ShouldAddIdToDependenciesIfNotSpecified) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
}

TEST(InclusionProjection, ShouldAddDependenciesOfComputedFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx,
                    BSON("a"
                         << "$a"
                         << "x"
                         << "$z"));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("z"), 1UL);
}

TEST(InclusionProjection, ShouldAddPathToDependenciesForNestedComputedFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx,
                    BSON("x.y"
                         << "$z"));

    DepsTracker deps;
    inclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    // Implicit "_id".
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    // Needed by the ExpressionFieldPath.
    ASSERT_EQ(deps.fields.count("z"), 1UL);
    // Needed to ensure we preserve the structure of the input document.
    ASSERT_EQ(deps.fields.count("x"), 1UL);
}

TEST(InclusionProjection, ShouldSerializeToEquivalentProjection) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, fromjson("{a: {$add: ['$a', 2]}, b: {d: 3}, 'x.y': {$literal: 4}}"));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization = Document(fromjson(
        "{_id: true, a: {$add: [\"$a\", {$const: 2}]}, b: {d: true}, x: {y: {$const: 4}}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serialize(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjection, ShouldSerializeExplicitExclusionOfId) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("_id" << false << "a" << true));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization = Document{{"_id", false}, {"a", true}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serialize(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecAllPlans));
}


TEST(InclusionProjection, ShouldOptimizeTopLevelExpressions) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion.optimize();

    auto expectedSerialization = Document{{"_id", true}, {"a", Document{{"$const", 3}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serialize(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjection, ShouldOptimizeNestedExpressions) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.b" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion.optimize();

    auto expectedSerialization =
        Document{{"_id", true}, {"a", Document{{"b", Document{{"$const", 3}}}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion.serialize(boost::none));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion.serialize(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(InclusionProjection, ShouldReportThatAllExceptIncludedFieldsAreModified) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(
        expCtx,
        BSON("a" << wrapInLiteral("computedVal") << "b.c" << wrapInLiteral("computedVal") << "d"
                 << true
                 << "e.f"
                 << true));

    auto modifiedPaths = inclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 3UL);
}

TEST(InclusionProjection, ShouldReportThatAllExceptIncludedFieldsAreModifiedWithIdExclusion) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx,
                    BSON("_id" << false << "a" << wrapInLiteral("computedVal") << "b.c"
                               << wrapInLiteral("computedVal")
                               << "d"
                               << true
                               << "e.f"
                               << true));

    auto modifiedPaths = inclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    // _id is explicitly excluded.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 0UL);

    ASSERT_EQ(modifiedPaths.paths.size(), 2UL);
}

//
// Top-level only.
//

TEST(InclusionProjectionExecutionTest, ShouldIncludeTopLevelField) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true));

    // More than one field in document.
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = inclusion.applyProjection(Document{{"a", 1}});
    expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = inclusion.applyProjection(Document{{"c", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = inclusion.applyProjection(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedTopLevelField) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Computed field should replace existing field.
    result = inclusion.applyProjection(Document{{"newField", "preExisting"_sd}});
    expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyBothInclusionsAndComputedFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true << "newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldIncludeFieldsInOrderOfInputDoc) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("first" << true << "second" << true << "third" << true));
    auto inputDoc = Document{{"second", 1}, {"first", 0}, {"third", 2}};
    auto result = inclusion.applyProjection(inputDoc);
    ASSERT_DOCUMENT_EQ(result, inputDoc);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyComputedFieldsInOrderSpecified) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx,
                    BSON("firstComputed" << wrapInLiteral("FIRST") << "secondComputed"
                                         << wrapInLiteral("SECOND")));
    auto result = inclusion.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"firstComputed", "FIRST"_sd}, {"secondComputed", "SECOND"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldImplicitlyIncludeId) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true));
    auto result = inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should leave the "_id" in the same place as in the original document.
    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    expectedResult = Document{{"a", 1}, {"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldImplicitlyIncludeIdWithComputedFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldIncludeIdIfExplicitlyIncluded) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true << "_id" << true << "b" << true));
    auto result =
        inclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldExcludeIdIfExplicitlyExcluded) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true << "_id" << false));
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldReplaceIdWithComputedId) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("_id" << wrapInLiteral("newId")));
    auto result = inclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"_id", "newId"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Projections with nested fields.
//

TEST(InclusionProjectionExecutionTest, ShouldIncludeSimpleDottedFieldFromSubDoc) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.b" << true));

    // More than one field in sub document.
    auto result = inclusion.applyProjection(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = inclusion.applyProjection(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion.applyProjection(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion.applyProjection(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldNotCreateSubDocIfDottedIncludedFieldDoesNotExist) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("sub.target" << true));

    // Should not add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace the first part of the path if that part exists.
    result = inclusion.applyProjection(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyDottedInclusionToEachElementInArray) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.b" << true));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};

    // Drops non-documents and non-arrays. Applies projection to documents, recurses on nested
    // arrays.
    vector<Value> expectedNestedValues = {Value(),
                                          Value(Document{}),
                                          Value(Document{{"b", 1}}),
                                          Value(Document{{"b", 1}}),
                                          Value(vector<Value>{}),
                                          Value(vector<Value>{Value(), Value(Document{})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedDottedFieldToSubDocument) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("sub.target" << wrapInLiteral("computedVal")));

    // Other fields exist in sub document, one of which is the specified field.
    auto result = inclusion.applyProjection(Document{{"sub", Document{{"target", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion.applyProjection(Document{{"sub", Document{{"c", 1}}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion.applyProjection(Document{{"sub", Document{}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldCreateSubDocIfDottedComputedFieldDoesntExist) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("sub.target" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion.applyProjection(Document{{"sub", "notADocument"_sd}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldCreateNestedSubDocumentsAllTheWayToComputedField) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.b.c.d" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult =
        Document{{"a", Document{{"b", Document{{"c", Document{{"d", "computedVal"_sd}}}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion.applyProjection(Document{{"a", Document{{"b", "other"_sd}}}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddComputedDottedFieldToEachElementInArray) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.b" << wrapInLiteral("COMPUTED")));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"b", 1}}),
                                  Value(Document{{"b", 1}, {"c", 2}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};
    vector<Value> expectedNestedValues = {
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(Document{{"b", "COMPUTED"_sd}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(Document{{"b", "COMPUTED"_sd}}),
                            Value(Document{{"b", "COMPUTED"_sd}})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyInclusionsAndAdditionsToEachElementInArray) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a.inc" << true << "a.comp" << wrapInLiteral("COMPUTED")));

    vector<Value> nestedValues = {Value(1),
                                  Value(Document{}),
                                  Value(Document{{"inc", 1}}),
                                  Value(Document{{"inc", 1}, {"c", 2}}),
                                  Value(Document{{"c", 2}, {"inc", 1}}),
                                  Value(Document{{"inc", 1}, {"c", 2}, {"comp", "original"_sd}}),
                                  Value(vector<Value>{}),
                                  Value(vector<Value>{Value(1), Value(Document{{"inc", 1}})})};
    vector<Value> expectedNestedValues = {
        Value(Document{{"comp", "COMPUTED"_sd}}),
        Value(Document{{"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(Document{{"comp", "COMPUTED"_sd}}),
                            Value(Document{{"inc", 1}, {"comp", "COMPUTED"_sd}})})};
    auto result = inclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAddOrIncludeSubFieldsOfId) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("_id.X" << true << "_id.Z" << wrapInLiteral("NEW")));
    auto result = inclusion.applyProjection(Document{{"_id", Document{{"X", 1}, {"Y", 2}}}});
    auto expectedResult = Document{{"_id", Document{{"X", 1}, {"Z", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    // Include all of "a.b", "a.c", "a.d", and "a.e".
    // Add new computed fields "a.W", "a.X", "a.Y", and "a.Z".
    inclusion.parse(
        expCtx,
        BSON("a.b" << true << "a.c" << true << "a.W" << wrapInLiteral("W") << "a.X"
                   << wrapInLiteral("X")
                   << "a"
                   << BSON("d" << true << "e" << true << "Y" << wrapInLiteral("Y") << "Z"
                               << wrapInLiteral("Z"))));
    auto result = inclusion.applyProjection(Document{
        {"a",
         Document{{"b", "b"_sd}, {"c", "c"_sd}, {"d", "d"_sd}, {"e", "e"_sd}, {"f", "f"_sd}}}});
    auto expectedResult = Document{{"a",
                                    Document{{"b", "b"_sd},
                                             {"c", "c"_sd},
                                             {"d", "d"_sd},
                                             {"e", "e"_sd},
                                             {"W", "W"_sd},
                                             {"X", "X"_sd},
                                             {"Y", "Y"_sd},
                                             {"Z", "Z"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyNestedComputedFieldsInOrderSpecified) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx,
                    BSON("a" << wrapInLiteral("FIRST") << "b.c" << wrapInLiteral("SECOND")));
    auto result = inclusion.applyProjection(Document{});
    auto expectedResult = Document{{"a", "FIRST"_sd}, {"b", Document{{"c", "SECOND"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ShouldApplyComputedFieldsAfterAllInclusions) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("b.c" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion.applyProjection(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", Document{{"c", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // In this case, the field 'b' shows up first and has a nested inclusion or computed field. Even
    // though it is a computed field, it will appear first in the output document. This is
    // inconsistent, but the expected behavior, and a consequence of applying the projection
    // recursively to each sub-document.
    result = inclusion.applyProjection(Document{{"b", 4}, {"a", 1}});
    expectedResult = Document{{"b", Document{{"c", "NEW"_sd}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(InclusionProjectionExecutionTest, ComputedFieldReplacingExistingShouldAppearAfterInclusions) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("b" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion.applyProjection(Document{{"b", 1}, {"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", "NEW"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion.applyProjection(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Misc.
//

TEST(InclusionProjectionExecutionTest, ShouldAlwaysKeepMetadataFromOriginalDoc) {
    ParsedInclusionProjection inclusion;
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    inclusion.parse(expCtx, BSON("a" << true));

    MutableDocument inputDocBuilder(Document{{"a", 1}});
    inputDocBuilder.setRandMetaField(1.0);
    inputDocBuilder.setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = inclusion.applyProjection(inputDoc);

    MutableDocument expectedDoc(inputDoc);
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
