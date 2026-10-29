// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/queue_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_queue.h"

#include <string_view>

namespace mongo {
boost::intrusive_ptr<exec::agg::Stage> documentSourceQueueToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceQueue) {
    auto* ptr = dynamic_cast<DocumentSourceQueue*>(documentSourceQueue.get());
    tassert(10817000, "expected 'DocumentSourceQueue' type", ptr);
    return make_intrusive<exec::agg::QueueStage>(
        ptr->kStageName, ptr->getExpCtx(), ptr->_queue.get());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(queueStage, DocumentSourceQueue::id, documentSourceQueueToStageFn);

QueueStage::QueueStage(std::string_view stageName,
                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       std::deque<GetNextResult> queue)
    : Stage(stageName, pExpCtx), _queue(std::move(queue)) {}

GetNextResult QueueStage::doGetNext() {
    if (_queue.empty()) {
        return GetNextResult::makeEOF();
    }

    auto next = std::move(_queue.front());
    _queue.pop_front();
    return next;
}
}  // namespace exec::agg
}  // namespace mongo
