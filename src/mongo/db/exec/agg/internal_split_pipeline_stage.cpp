// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_split_pipeline_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_split_pipeline.h"

#include <string_view>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalSplitPipelineToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* internalSplitPipelineDS =
        dynamic_cast<DocumentSourceInternalSplitPipeline*>(documentSource.get());

    tassert(
        10816701, "expected 'DocumentSourceInternalSplitPipeline' type", internalSplitPipelineDS);

    return make_intrusive<exec::agg::InternalSplitPipelineStage>(
        internalSplitPipelineDS->kStageName, internalSplitPipelineDS->getExpCtx());
}


REGISTER_AGG_STAGE_MAPPING(_internalSplitPipeline,
                           DocumentSourceInternalSplitPipeline::id,
                           documentSourceInternalSplitPipelineToStageFn);

InternalSplitPipelineStage::InternalSplitPipelineStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Stage(stageName, expCtx) {}

GetNextResult InternalSplitPipelineStage::doGetNext() {
    return pSource->getNext();
}

}  // namespace mongo::exec::agg
