// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/unwind_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_unwind.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceUnwindToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto unwindDS = boost::dynamic_pointer_cast<DocumentSourceUnwind>(documentSource);

    tassert(10423200, "expected 'DocumentSourceUnwind' type", unwindDS);

    return make_intrusive<exec::agg::UnwindStage>(
        unwindDS->kStageName,
        unwindDS->getExpCtx(),
        createUnwindProcessorFromDocumentSource(unwindDS));
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(unwind, DocumentSourceUnwind::id, documentSourceUnwindToStageFn);

UnwindStage::UnwindStage(std::string_view stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         std::unique_ptr<UnwindProcessor> unwindProcessor)
    : Stage(stageName, pExpCtx), _unwindProcessor(std::move(unwindProcessor)) {};

GetNextResult UnwindStage::doGetNext() {
    auto nextOut = _unwindProcessor->getNext();
    while (!nextOut) {
        // No more elements in array currently being unwound. This will loop if the input
        // document is missing the unwind field or has an empty array.
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        // Try to extract an output document from the new input document.
        _unwindProcessor->process(nextInput.releaseDocument());
        nextOut = _unwindProcessor->getNext();
    }

    return DocumentSource::GetNextResult(std::move(*nextOut));
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
