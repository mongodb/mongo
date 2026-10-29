// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/mock_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceMockToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsMock = dynamic_cast<DocumentSourceMock*>(documentSource.get());

    tassert(10812600, "expected 'DocumentSourceMock' type", dsMock);

    return make_intrusive<exec::agg::MockStage>(
        dsMock->kStageName, dsMock->getExpCtx(), dsMock->_results);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(mockStage, mongo::DocumentSourceMock::id, documentSourceMockToStageFn)

MockStage::MockStage(std::string_view stageType,
                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     std::deque<GetNextResult> results)
    : Stage(stageType, expCtx) {
    for (auto& res : results) {
        _queue.push_back(QueueItem{std::move(res), /*count*/ 1});
    }
}

boost::intrusive_ptr<MockStage> MockStage::createForTest(
    Document doc, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::deque<GetNextResult> results;
    if (doc.metadata().isChangeStreamControlEvent()) {
        results.push_back(GetNextResult::makeAdvancedControlDocument(std::move(doc)));
    } else {
        results.push_back(std::move(doc));
    }
    return make_intrusive<MockStage>(DocumentSourceMock::kStageName, expCtx, std::move(results));
}

GetNextResult MockStage::doGetNext() {
    invariant(!isDisposed);
    invariant(!isDetachedFromOpCtx);

    if (_queue.empty()) {
        return GetNextResult::makeEOF();
    }

    boost::optional<DocumentSource::GetNextResult> next;
    auto& nextQueueItem = _queue.front();
    --nextQueueItem.count;
    if (nextQueueItem.count == 0) {
        next = std::move(nextQueueItem.next);
        _queue.pop_front();
    } else {
        next = nextQueueItem.next;
    }
    return std::move(*next);
}

size_t MockStage::size() const {
    return _queue.size();
}

}  // namespace exec::agg
}  // namespace mongo
