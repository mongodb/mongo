/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{a: [1, 2]}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // With an empty path set, all documents should pass through
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isEOF());
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, ScalarFieldsPassValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a"), FieldPath("b"), FieldPath("c.d")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest(
        {"{a: 1, b: 'string', c: {d: 42}}", "{a: 2, b: 'another', c: {d: 100}}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // All documents have scalar values, should pass validation
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isEOF());
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, MissingFieldsPassValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("nonExistent"), FieldPath("another")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest({"{a: 1}", "{b: 2}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // Fields don't exist in documents, should pass validation
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isEOF());
}

DEATH_TEST_F(InternalInternalAssertDataAssumptionsStageDeathTest,
             ArrayFieldFailsValidation,
             "canPathBeArray") {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest({"{a: [1, 2]}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // This should trigger an assertion failure with error code 9587703
    validateStage->getNext();
}

DEATH_TEST_F(InternalInternalAssertDataAssumptionsStageDeathTest,
             NestedArrayFieldFailsValidation,
             "canPathBeArray") {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a.b.c")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest({"{a: {b: {c: [1]}}}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // Nested array should trigger validation failure
    validateStage->getNext();
}

TEST_F(InternalInternalAssertDataAssumptionsStageTest, MultipleFieldsValidation) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("x"), FieldPath("y"), FieldPath("z.nested")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));
    auto validateStage = exec::agg::buildStage(validateSource);

    auto mock = DocumentSourceMock::createForTest(
        {"{x: 1, y: 'text', z: {nested: 42}}", "{x: 2, y: 'more', z: {nested: 100}}"}, expCtx);
    auto mockStage = exec::agg::buildStage(mock);
    exec::agg::MockStage::setSource_forTest(validateStage.get(), mockStage.get());

    // Multiple scalar fields should all pass
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isAdvanced());
    ASSERT_TRUE(validateStage->getNext().isEOF());
}

}  // namespace
}  // namespace mongo
