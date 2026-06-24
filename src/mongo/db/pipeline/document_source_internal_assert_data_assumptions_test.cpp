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

#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/server_parameter_guard.h"

namespace mongo {
namespace {

using DocumentSourceInternalAssertDataAssumptionsTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalAssertDataAssumptionsTest, SerializeForCloning) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a"), FieldPath("b.c")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    query_shape::SerializationOptions opts;
    opts.serializeForCloning = true;

    auto serialized = validateSource->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(
        serialized,
        BSON("$_internalAssertDataAssumptions" << BSON("paths" << BSON_ARRAY("a" << "b.c"))));
}

TEST_F(DocumentSourceInternalAssertDataAssumptionsTest, SerializeWithRedaction) {
    auto expCtx = getExpCtx();
    std::set<FieldPath> nonArrayPaths = {FieldPath("a"), FieldPath("b.c")};
    auto validateSource =
        DocumentSourceInternalAssertDataAssumptions::create(expCtx, std::move(nonArrayPaths));

    query_shape::SerializationOptions opts;
    opts.transformIdentifiers = true;
    opts.transformIdentifiersCallback = [](std::string_view s) -> std::string {
        return "HASH(" + std::string{s} + ")";
    };

    auto serialized = validateSource->serialize(opts).getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serialized,
                      BSON("$_internalAssertDataAssumptions"
                           << BSON("paths" << BSON_ARRAY("HASH(a)" << "HASH(b).HASH(c)"))));
}

TEST_F(DocumentSourceInternalAssertDataAssumptionsTest, CreateFromBson) {
    auto expCtx = getExpCtx();
    BSONObj spec =
        BSON("$_internalAssertDataAssumptions" << BSON("paths" << BSON_ARRAY("a" << "b.c")));

    auto stage =
        DocumentSourceInternalAssertDataAssumptions::createFromBson(spec.firstElement(), expCtx);

    ASSERT_TRUE(stage);
}

TEST_F(DocumentSourceInternalAssertDataAssumptionsTest, AutoInsertedWhenKnobEnabled) {
    unittest::ServerParameterGuard knob("internalEnableDependencyGraphValidation", true);

    auto expCtx = getExpCtx();

    // Pipeline: {$set: {a: 1}} followed by {$match: {a: 1}}.
    // After $set, field 'a' is a scalar constant so the dependency graph should report
    // canPathBeArray("a") == false for $match. This causes a $_internalAssertDataAssumptions
    // stage to be inserted before $match.
    auto pipeline =
        pipeline_factory::makePipeline({fromjson("{$set: {a: 1}}"), fromjson("{$match: {a: 1}}")},
                                       expCtx,
                                       pipeline_factory::kOptionsMinimal);

    pipeline_optimization::optimizePipeline(*pipeline);

    // Check that at least one $_internalAssertDataAssumptions stage was inserted.
    bool foundValidationStage = false;
    for (const auto& source : pipeline->getSources()) {
        if (dynamic_cast<DocumentSourceInternalAssertDataAssumptions*>(source.get())) {
            foundValidationStage = true;
            break;
        }
    }
    ASSERT_TRUE(foundValidationStage);
}

TEST_F(DocumentSourceInternalAssertDataAssumptionsTest, NotInsertedWhenKnobDisabled) {
    unittest::ServerParameterGuard knob("internalEnableDependencyGraphValidation", false);

    auto expCtx = getExpCtx();

    auto pipeline =
        pipeline_factory::makePipeline({fromjson("{$set: {a: 1}}"), fromjson("{$match: {a: 1}}")},
                                       expCtx,
                                       pipeline_factory::kOptionsMinimal);

    pipeline_optimization::optimizePipeline(*pipeline);

    // With the knob disabled, no validation stages should be present.
    for (const auto& source : pipeline->getSources()) {
        ASSERT_FALSE(dynamic_cast<DocumentSourceInternalAssertDataAssumptions*>(source.get()));
    }
}

}  // namespace
}  // namespace mongo
