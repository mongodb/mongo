// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_hybrid_search_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalHybridSearchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* hybridSearchDS = dynamic_cast<DocumentSourceInternalHybridSearch*>(documentSource.get());

    tassert(12109101, "expected 'DocumentSourceInternalHybridSearch' type", hybridSearchDS);

    return make_intrusive<exec::agg::InternalHybridSearchStage>(hybridSearchDS->kStageName,
                                                                hybridSearchDS->getExpCtx());
}

REGISTER_AGG_STAGE_MAPPING(_internalHybridSearch,
                           DocumentSourceInternalHybridSearch::id,
                           documentSourceInternalHybridSearchToStageFn);

InternalHybridSearchStage::InternalHybridSearchStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Stage(stageName, expCtx) {}

GetNextResult InternalHybridSearchStage::doGetNext() {
    return pSource->getNext();
}

}  // namespace mongo::exec::agg
