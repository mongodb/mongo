// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_transform_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamTransformToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamTransformDS =
        dynamic_cast<DocumentSourceChangeStreamTransform*>(documentSource.get());

    tassert(
        10561306, "expected 'DocumentSourceChangeStreamTransform' type", changeStreamTransformDS);

    return make_intrusive<exec::agg::ChangeStreamTransformStage>(
        changeStreamTransformDS->kStageName,
        changeStreamTransformDS->getExpCtx(),
        changeStreamTransformDS->_transformer);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamTransform,
                           DocumentSourceChangeStreamTransform::id,
                           documentSourceChangeStreamTransformToStageFn)

ChangeStreamTransformStage::ChangeStreamTransformStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::shared_ptr<ChangeStreamEventTransformer> transformer)
    : Stage(stageName, pExpCtx),
      _showErrorsAsEvents(change_stream::changeStreamShowsErrorsAsEvents(pExpCtx)),
      _transformer(std::move(transformer)) {
    uassert(50988,
            "Illegal attempt to execute an internal change stream stage on router. A $changeStream "
            "stage must be the first stage in a pipeline",
            !pExpCtx->getInRouter());
}

GetNextResult ChangeStreamTransformStage::doGetNext() {
    // We are about to produce a NEW event. Any identity recorded for the previous event (which has
    // already been delivered to, or dropped by, the stages above) must not be allowed to survive
    // into the production of this one: if producing it fails, the stale identity from the prior
    // event would be used by the top-level conversion stage to mislabel the failure. Clear it
    // before pulling the next input so it is only ever set for the event currently being produced.
    if (_showErrorsAsEvents) {
        pExpCtx->clearActiveChangeStreamEvent();
        pExpCtx->clearActiveChangeStreamErrorSource();
    }

    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    auto result = _transformer->applyTransformation(input.releaseDocument());

    // From here on this event is in flight above the transform: record it so that a failure in
    // the stages above can be reported against it.
    if (_showErrorsAsEvents) {
        change_stream::recordActiveEventIdentity(pExpCtx, result);
    }
    return std::move(result);
}

}  // namespace exec::agg
}  // namespace mongo
