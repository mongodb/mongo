// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace {
using DocumentSourceInternalSplitPipelineTest = AggregationContextFixture;

TEST_F(DocumentSourceInternalSplitPipelineTest, NotAllowedInLookupIfMustRunOnMongos) {
    auto expCtx = getExpCtx();
    auto split = DocumentSourceInternalSplitPipeline::create(
        expCtx, StageConstraints::HostTypeRequirement::kRouter);
    ASSERT_FALSE(split->constraints().isAllowedInLookupPipeline());
    ASSERT(split->constraints().hostRequirement == StageConstraints::HostTypeRequirement::kRouter);
}

}  // namespace
}  // namespace mongo
