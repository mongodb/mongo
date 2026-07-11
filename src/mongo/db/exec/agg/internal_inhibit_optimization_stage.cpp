// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_inhibit_optimization_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_inhibit_optimization.h"

#include <string_view>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalInhibitOptimizationToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* inhibitOptimisationDS =
        dynamic_cast<DocumentSourceInternalInhibitOptimization*>(documentSource.get());

    tassert(10816700,
            "expected 'DocumentSourceInternalInhibitOptimization' type",
            inhibitOptimisationDS);

    return make_intrusive<exec::agg::InternalInhibitOptimizationStage>(
        inhibitOptimisationDS->kStageName, inhibitOptimisationDS->getExpCtx());
}


REGISTER_AGG_STAGE_MAPPING(_internalInhibitOptimization,
                           DocumentSourceInternalInhibitOptimization::id,
                           documentSourceInternalInhibitOptimizationToStageFn);

InternalInhibitOptimizationStage::InternalInhibitOptimizationStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : Stage(stageName, expCtx) {}

GetNextResult InternalInhibitOptimizationStage::doGetNext() {
    return pSource->getNext();
}

}  // namespace mongo::exec::agg
