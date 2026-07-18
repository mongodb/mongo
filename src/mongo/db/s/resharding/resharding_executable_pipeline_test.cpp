// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_executable_pipeline.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo::resharding {
namespace {

class ReshardingExecutablePipelineTest : public AggregationContextFixture {
protected:
    std::unique_ptr<Pipeline> makeMockPipeline() {
        DocumentSourceContainer stages;
        stages.push_back(DocumentSourceMock::createForTest({"{_id: 0}", "{_id: 1}"}, getExpCtx()));
        return Pipeline::create(std::move(stages), getExpCtx());
    }
};

using ReshardingExecutablePipelineDeathTest = ReshardingExecutablePipelineTest;

TEST_F(ReshardingExecutablePipelineTest, DefaultIsUninitializedAndDisposeIsSafe) {
    ReshardingExecutablePipeline pipeline;
    ASSERT_FALSE(pipeline.isInitialized());

    // Disposing an uninitialized holder must be a safe no-op.
    pipeline.dispose(getOpCtx());
    ASSERT_FALSE(pipeline.isInitialized());
}

TEST_F(ReshardingExecutablePipelineTest, ReinitializeBuildsPipelineAndGetReturnsExecutable) {
    ReshardingExecutablePipeline pipeline;
    pipeline.reinitialize(makeMockPipeline());
    ASSERT_TRUE(pipeline.isInitialized());

    auto& execPipeline = pipeline.get();
    ASSERT_TRUE(execPipeline.getNext());
    ASSERT_TRUE(execPipeline.getNext());
    ASSERT_FALSE(execPipeline.getNext());

    pipeline.dispose(getOpCtx());
    ASSERT_FALSE(pipeline.isInitialized());
}

TEST_F(ReshardingExecutablePipelineTest, ReinitializeIsStronglyExceptionSafe) {
    ReshardingExecutablePipeline pipeline(
        [](const Pipeline&) -> std::unique_ptr<exec::agg::Pipeline> {
            uasserted(ErrorCodes::Interrupted,
                      "simulated failure while building executable pipeline");
        });

    ASSERT_THROWS_CODE(
        pipeline.reinitialize(makeMockPipeline()), DBException, ErrorCodes::Interrupted);
    ASSERT_FALSE(pipeline.isInitialized());

    // Ensure that calling dispose is safe even after a failed reinitialize.
    pipeline.dispose(getOpCtx());
    ASSERT_FALSE(pipeline.isInitialized());
}

TEST_F(ReshardingExecutablePipelineTest, ExecutesPipelineEndToEnd) {
    DocumentSourceContainer stages;
    stages.push_back(
        DocumentSourceMock::createForTest({"{_id: 0}", "{_id: 1}", "{_id: 2}"}, getExpCtx()));
    stages.push_back(DocumentSourceMatch::create(BSON("_id" << BSON("$gte" << 1)), getExpCtx()));

    ReshardingExecutablePipeline pipeline;
    pipeline.reinitialize(Pipeline::create(std::move(stages), getExpCtx()));

    auto& execPipeline = pipeline.get();

    auto first = execPipeline.getNext();
    ASSERT_TRUE(first);
    ASSERT_EQ(first->getField("_id").coerceToInt(), 1);

    auto second = execPipeline.getNext();
    ASSERT_TRUE(second);
    ASSERT_EQ(second->getField("_id").coerceToInt(), 2);

    ASSERT_FALSE(execPipeline.getNext());

    pipeline.dispose(getOpCtx());
    ASSERT_FALSE(pipeline.isInitialized());
}

TEST_F(ReshardingExecutablePipelineTest, DetachAndReattachRoundTrip) {
    ReshardingExecutablePipeline pipeline;
    pipeline.reinitialize(makeMockPipeline());

    pipeline.detachFromOpCtx();
    pipeline.reattachToOpCtx(getOpCtx());
    ASSERT_TRUE(pipeline.isInitialized());
    ASSERT_TRUE(pipeline.get().getNext());

    pipeline.dispose(getOpCtx());
    ASSERT_FALSE(pipeline.isInitialized());
}

DEATH_TEST_REGEX_F(ReshardingExecutablePipelineDeathTest,
                   ReinitializeWhileInitializedTasserts,
                   "Tripwire assertion.*13159800") {
    ReshardingExecutablePipeline pipeline;
    pipeline.reinitialize(makeMockPipeline());
    pipeline.reinitialize(makeMockPipeline());
}

DEATH_TEST_REGEX_F(ReshardingExecutablePipelineDeathTest,
                   ReattachToOpCtxWhileUninitializedTasserts,
                   "Tripwire assertion.*13159801") {
    ReshardingExecutablePipeline pipeline;
    pipeline.reattachToOpCtx(getOpCtx());
}

DEATH_TEST_REGEX_F(ReshardingExecutablePipelineDeathTest,
                   DetachFromOpCtxWhileUninitializedTasserts,
                   "Tripwire assertion.*13159802") {
    ReshardingExecutablePipeline pipeline;
    pipeline.detachFromOpCtx();
}

DEATH_TEST_REGEX_F(ReshardingExecutablePipelineDeathTest,
                   GetWhileUninitializedTasserts,
                   "Tripwire assertion.*13159803") {
    ReshardingExecutablePipeline pipeline;
    pipeline.get();
}

}  // namespace
}  // namespace mongo::resharding
