// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
