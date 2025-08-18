/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/agg/queue_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_queue.h"

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

QueueStage::QueueStage(StringData stageName,
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
