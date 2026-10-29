// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/exec/agg/skip_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSkipToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsSkip = dynamic_cast<DocumentSourceSkip*>(documentSource.get());

    tassert(10816501, "expected 'DocumentSourceSkip' type", dsSkip);

    return make_intrusive<exec::agg::SkipStage>(
        dsSkip->kStageName, dsSkip->getExpCtx(), dsSkip->getSkip());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(skipStage, DocumentSourceSkip::id, documentSourceSkipToStageFn)

SkipStage::SkipStage(std::string_view stageName,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     long long nToSkip)
    : Stage(stageName, expCtx), _nToSkip(nToSkip) {}

GetNextResult SkipStage::doGetNext() {
    while (_nSkippedSoFar < _nToSkip) {
        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }
        ++_nSkippedSoFar;
    }

    return pSource->getNext();
}

}  // namespace exec::agg
}  // namespace mongo
