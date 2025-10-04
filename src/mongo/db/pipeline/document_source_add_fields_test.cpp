/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/pipeline/document_source_add_fields.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <vector>

namespace mongo {
namespace {

using std::vector;

//
// DocumentSourceAddFields delegates much of its responsibilities to the
// AddFieldsProjectionExecutor. Most of the functional tests are testing
// AddFieldsProjectionExecutor. directly. These are meant as simpler integration tests.
//

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using AddFieldsTest = AggregationContextFixture;

TEST_F(AddFieldsTest, ShouldKeepUnspecifiedFieldsReplaceExistingFieldsAndAddNewFields) {
    auto addFields =
        DocumentSourceAddFields::create(BSON("e" << 2 << "b" << BSON("c" << 3)), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(
        Document{{"a", 1}, {"b", Document{{"c", 1}}}, {"d", 1}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document{{"a", 1}, {"b", Document{{"c", 3}}}, {"d", 1}, {"e", 2}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

TEST_F(AddFieldsTest, ShouldSerializeAndParse) {
    auto addFields =
        DocumentSourceAddFields::create(BSON("a" << BSON("$const" << "new")), getExpCtx());
    ASSERT(addFields->getSourceName() == DocumentSourceAddFields::kStageName);
    vector<Value> serializedArray;
    addFields->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, fromjson("{$addFields: {a: {$const: 'new'}}}"));
    addFields = DocumentSourceAddFields::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(addFields != nullptr);
    ASSERT(addFields->getSourceName() == DocumentSourceAddFields::kStageName);
}

TEST_F(AddFieldsTest, SetAliasShouldSerializeAndParse) {
    auto setStage = DocumentSourceAddFields::create(
        BSON("a" << BSON("$const" << "new")), getExpCtx(), DocumentSourceAddFields::kAliasNameSet);
    ASSERT(setStage->getSourceName() == DocumentSourceAddFields::kAliasNameSet);
    vector<Value> serializedArray;
    setStage->serializeToArray(serializedArray);
    auto serializedBson = serializedArray[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedBson, fromjson("{$set: {a: {$const: 'new'}}}"));
    setStage = DocumentSourceAddFields::createFromBson(serializedBson.firstElement(), getExpCtx());
    ASSERT(setStage != nullptr);
    ASSERT(setStage->getSourceName() == DocumentSourceAddFields::kAliasNameSet);
}

TEST_F(AddFieldsTest, ShouldOptimizeInnerExpressions) {
    auto addFields = DocumentSourceAddFields::create(
        BSON("a" << BSON("$and" << BSON_ARRAY(BSON("$const" << true)))), getExpCtx());
    addFields->optimize();
    // The $and should have been replaced with its only argument.
    vector<Value> serializedArray;
    addFields->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(serializedArray[0].getDocument().toBson(),
                      fromjson("{$addFields: {a: {$const: true}}}"));
}

TEST_F(AddFieldsTest, ShouldErrorOnNonObjectSpec) {
    BSONObj spec = BSON("$addFields" << "foo");
    BSONElement specElement = spec.firstElement();
    ASSERT_THROWS_CODE(DocumentSourceAddFields::createFromBson(specElement, getExpCtx()),
                       AssertionException,
                       40272);
}

TEST_F(AddFieldsTest, ShouldBeAbleToProcessMultipleDocuments) {
    auto addFields = DocumentSourceAddFields::create(BSON("a" << 10), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(
        {Document{{"a", 1}, {"b", 2}}, Document{{"c", 3}, {"d", 4}}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected = Document{{"a", 10}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    expected = Document{{"c", 3}, {"d", 4}, {"a", 10}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);

    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

TEST_F(AddFieldsTest, ShouldAddReferencedFieldsToDependencies) {
    auto addFields = DocumentSourceAddFields::create(
        fromjson("{a: true, x: '$b', y: {$and: ['$c','$d']}, z: {$meta: 'textScore'}}"),
        getExpCtx());
    DepsTracker dependencies(DepsTracker::kOnlyTextScore);
    ASSERT_EQUALS(DepsTracker::State::SEE_NEXT, addFields->getDependencies(&dependencies));
    ASSERT_EQUALS(3U, dependencies.fields.size());

    // No implicit _id dependency.
    ASSERT_EQUALS(0U, dependencies.fields.count("_id"));

    // Replaced field is not dependent.
    ASSERT_EQUALS(0U, dependencies.fields.count("a"));

    // Field path expression dependency.
    ASSERT_EQUALS(1U, dependencies.fields.count("b"));

    // Nested expression dependencies.
    ASSERT_EQUALS(1U, dependencies.fields.count("c"));
    ASSERT_EQUALS(1U, dependencies.fields.count("d"));
    ASSERT_EQUALS(false, dependencies.needWholeDocument);
    ASSERT_EQUALS(true, dependencies.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST_F(AddFieldsTest, ShouldPropagatePauses) {
    auto addFields = DocumentSourceAddFields::create(BSON("a" << 10), getExpCtx());
    auto mock =
        exec::agg::MockStage::createForTest({Document(),
                                             DocumentSource::GetNextResult::makePauseExecution(),
                                             Document(),
                                             DocumentSource::GetNextResult::makePauseExecution()},
                                            getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    ASSERT_TRUE(addFieldsStage->getNext().isAdvanced());
    ASSERT_TRUE(addFieldsStage->getNext().isPaused());
    ASSERT_TRUE(addFieldsStage->getNext().isAdvanced());
    ASSERT_TRUE(addFieldsStage->getNext().isPaused());

    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

TEST_F(AddFieldsTest, AddFieldsWithRemoveSystemVariableDoesNotAddField) {
    auto addFields = DocumentSourceAddFields::create(BSON("fieldToAdd" << "$$REMOVE"), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(Document{{"existingField", 1}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected{{"existingField", 1}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

TEST_F(AddFieldsTest, AddFieldsWithRootSystemVariableAddsRootAsSubDoc) {
    auto addFields = DocumentSourceAddFields::create(BSON("b" << "$$ROOT"), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(Document{{"a", 1}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected{{"a", 1}, {"b", Document{{"a", 1}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

TEST_F(AddFieldsTest, AddFieldsWithCurrentSystemVariableAddsRootAsSubDoc) {
    auto addFields = DocumentSourceAddFields::create(BSON("b" << "$$CURRENT"), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(Document{{"a", 1}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
    Document expected{{"a", 1}, {"b", Document{{"a", 1}}}};
    ASSERT_DOCUMENT_EQ(next.releaseDocument(), expected);
    ASSERT_TRUE(addFieldsStage->getNext().isEOF());
}

/**
 * Creates BSON for a DocumentSourceAddFields that represents computing a new field nested 'depth'
 * levels deep.
 */
BSONObj makeAddFieldsForNestedDocument(size_t depth) {
    ASSERT_GTE(depth, 1U);
    StringBuilder builder;
    builder << "a";
    for (size_t i = 0; i < depth - 1; ++i) {
        builder << ".a";
    }
    return BSON(builder.str() << 1);
}

TEST_F(AddFieldsTest, CanAddNestedDocumentExactlyAtDepthLimit) {
    auto addFields = DocumentSourceAddFields::create(
        makeAddFieldsForNestedDocument(BSONDepth::getMaxAllowableDepth()), getExpCtx());
    auto mock = exec::agg::MockStage::createForTest(Document{{"_id", 1}}, getExpCtx());
    auto addFieldsStage = exec::agg::buildStage(addFields);
    addFieldsStage->setSource(mock.get());

    auto next = addFieldsStage->getNext();
    ASSERT_TRUE(next.isAdvanced());
}

TEST_F(AddFieldsTest, CannotAddNestedDocumentExceedingDepthLimit) {
    ASSERT_THROWS_CODE(
        DocumentSourceAddFields::create(
            makeAddFieldsForNestedDocument(BSONDepth::getMaxAllowableDepth() + 1), getExpCtx()),
        AssertionException,
        ErrorCodes::Overflow);
}

TEST_F(AddFieldsTest, TestModifiedPaths) {
    auto addFields = DocumentSourceAddFields::create(
        BSON("a" << BSON("$concat" << BSON_ARRAY("$b" << "$c")) << "x"
                 << "$y"),
        getExpCtx());

    auto modifiedPaths = addFields->getModifiedPaths();

    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(1U, modifiedPaths.paths.size());
    ASSERT_EQUALS(1U, modifiedPaths.renames.size());
}

TEST_F(AddFieldsTest, AddFieldsRenameModifiesDestination) {
    auto addFields =
        DocumentSourceAddFields::create(fromjson("{'somePath' : '$otherField'}"), getExpCtx());

    // Forwards: "somePath" is _not_ preserved by this addFields - any existing value has been
    // overwritten.
    auto renames = semantic_analysis::renamedPaths(
        {"somePath"}, *addFields, semantic_analysis::Direction::kForward);
    ASSERT_FALSE(renames.has_value());

    // Forwards: "otherField" _is_ preserved by this addFields, and is renamed to "somePath".
    renames = semantic_analysis::renamedPaths(
        {"otherField"}, *addFields, semantic_analysis::Direction::kForward);
    ASSERT_TRUE(renames.has_value());
    ASSERT_EQUALS(renames->at("otherField"), "somePath");

    // Backwards: "somePath" is the result of a rename, so traversing backwards should map to the
    // previous name.
    renames = semantic_analysis::renamedPaths(
        {"somePath"}, *addFields, semantic_analysis::Direction::kBackward);
    ASSERT_TRUE(renames.has_value());
    ASSERT_EQUALS(renames->at("somePath"), "otherField");

    // Backwards: "otherField" still exists, and has not been modified.
    renames = semantic_analysis::renamedPaths(
        {"otherField"}, *addFields, semantic_analysis::Direction::kBackward);
    ASSERT_TRUE(renames.has_value());
    ASSERT_EQUALS(renames->at("otherField"), "otherField");
}

}  // namespace
}  // namespace mongo
