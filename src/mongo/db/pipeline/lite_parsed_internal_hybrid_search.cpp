// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/lite_parsed_internal_hybrid_search.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"

namespace mongo {

ALLOCATE_STAGE_PARAMS_ID(internalHybridSearch, InternalHybridSearchStageParams::id);

namespace {

// Build the real (passthrough) $_internalHybridSearch DocumentSource so the marker survives
// serialization to shards; its LiteParsed carries canRunOnTimeseries=false, enforced at each
// collection acquisition via validateWithCollectionMetadata.
DocumentSourceContainer internalHybridSearchToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<InternalHybridSearchStageParams*>(stageParams.get());
    tassert(12109102, "expected 'InternalHybridSearchStageParams' type", typedParams);
    return {
        DocumentSourceInternalHybridSearch::createFromBson(typedParams->getOriginalBson(), expCtx)};
}

}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(internalHybridSearch,
                                                 InternalHybridSearchStageParams::id,
                                                 internalHybridSearchToDocumentSourceFn);

REGISTER_INTERNAL_LITE_PARSED_DOCUMENT_SOURCE(_internalHybridSearch,
                                              LiteParsedInternalHybridSearch::parse);

}  // namespace mongo
