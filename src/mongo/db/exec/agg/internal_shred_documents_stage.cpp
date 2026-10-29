// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/internal_shred_documents_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_internal_shred_documents.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> internalShredDocumentsStageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceInternalShredDocuments) {
    auto* shredDocumentsDS = dynamic_cast<DocumentSourceInternalShredDocuments*>(
        documentSourceInternalShredDocuments.get());
    tassert(10980100, "expected 'DocumentSourceInternalShredDocuments' type", shredDocumentsDS);
    return make_intrusive<exec::agg::InternalShredDocumentsStage>(shredDocumentsDS->kStageName,
                                                                  shredDocumentsDS->getExpCtx());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(internalShredDocumentsStage,
                           DocumentSourceInternalShredDocuments::id,
                           internalShredDocumentsStageToStageFn);

InternalShredDocumentsStage::InternalShredDocumentsStage(
    std::string_view stageName, const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
    : Stage(stageName, pExpCtx) {}

GetNextResult InternalShredDocumentsStage::doGetNext() {
    auto next = pSource->getNext();
    if (next.isAdvanced()) {
        return GetNextResult(next.getDocument().shred());
    }
    return next;
}

}  // namespace exec::agg
}  // namespace mongo
