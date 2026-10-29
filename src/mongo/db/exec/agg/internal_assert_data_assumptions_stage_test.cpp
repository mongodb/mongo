// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_assert_data_assumptions_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

using InternalInternalAssertDataAssumptionsStageTest = AggregationContextFixture;
using InternalInternalAssertDataAssumptionsStageDeathTest = AggregationContextFixture;

TEST_F(InternalInternalAssertDataAssumptionsStageTest, EmptyPathSetAllowsAllDocuments) {
    auto expCtx = getExpCtx();
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::set<FieldPath>{});

    auto mockStage = exec::agg::MockStage::createForTest({"{a: 1}", "{a: [1, 2]}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // With an empty path set, all documents should pass through
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isEOF());
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, ScalarFieldsPassValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a"), FieldPath("b"), FieldPath("c.d")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    auto mockStage = exec::agg::MockStage::createForTest(
        {"{a: 1, b: 'string', c: {d: 42}}", "{a: 2, b: 'another', c: {d: 100}}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // All documents have scalar values, should pass validation
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isEOF());
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, MissingFieldsPassValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("nonExistent"), FieldPath("another")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    auto mockStage = exec::agg::MockStage::createForTest({"{a: 1}", "{b: 2}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // Fields don't exist in documents, should pass validation
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isEOF());
}

DEATH_TEST_F(InternalInternalAssertDataAssumptionsStageDeathTest,
             ArrayFieldFailsValidation,
             "canPathBeArray") {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    auto mockStage = exec::agg::MockStage::createForTest({"{a: [1, 2]}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // This should trigger an assertion failure with error code 12508302
    query->getNext();
}

DEATH_TEST_F(InternalInternalAssertDataAssumptionsStageDeathTest,
             NestedArrayFieldFailsValidation,
             "canPathBeArray") {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a.b.c")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    auto mockStage = exec::agg::MockStage::createForTest({"{a: {b: {c: [1]}}}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // Nested array should trigger validation failure
    query->getNext();
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, MultipleFieldsValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("x"), FieldPath("y"), FieldPath("z.nested")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    auto mockStage = exec::agg::MockStage::createForTest(
        {"{x: 1, y: 'text', z: {nested: 42}}", "{x: 2, y: 'more', z: {nested: 100}}"}, expCtx);
    auto query = exec::agg::buildStageAndStitch(validateSource, mockStage);

    // Multiple scalar fields should all pass
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isAdvanced());
    ASSERT_TRUE(query->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
