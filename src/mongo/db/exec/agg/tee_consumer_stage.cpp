// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/tee_consumer_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceTeeConsumerToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceTeeConsumer*>(source.get());

    tassert(10537003, "expected 'DocumentSourceTeeConsumer' type", documentSource);

    return make_intrusive<exec::agg::TeeConsumerStage>(
        documentSource->getExpCtx(), documentSource->_facetId, documentSource->_stageName);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(teeConsumerStage,
                           DocumentSourceTeeConsumer::id,
                           documentSourceTeeConsumerToStageFn);

TeeConsumerStage::TeeConsumerStage(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   size_t facetId,
                                   std::string_view stageName)
    : Stage(stageName, expCtx), _facetId(facetId) {}

void TeeConsumerStage::setTeeBuffer(const boost::intrusive_ptr<TeeBuffer>& bufferSource) {
    _bufferSource = std::move(bufferSource);
}

GetNextResult TeeConsumerStage::doGetNext() {
    tassert(10537004, "TeeBuffer not set", _bufferSource);
    return _bufferSource->getNext(_facetId);
}

void TeeConsumerStage::doDispose() {
    if (_bufferSource) {
        _bufferSource->dispose(_facetId);
    }
}
}  // namespace exec::agg
}  // namespace mongo
