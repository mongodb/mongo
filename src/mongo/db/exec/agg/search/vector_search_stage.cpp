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

#include "mongo/db/exec/agg/search/vector_search_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceVectorSearchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceVectorSearch*>(source.get());

    tassert(10807800, "expected 'DocumentSourceVectorSearch' type", documentSource);

    auto execStatsWrapper = std::make_shared<DSVectorSearchExecStatsWrapper>();
    documentSource->_execStatsWrapper = execStatsWrapper;
    return make_intrusive<exec::agg::VectorSearchStage>(documentSource->kStageName,
                                                        documentSource->getExpCtx(),
                                                        documentSource->_taskExecutor,
                                                        documentSource->_originalSpec.copy(),
                                                        std::move(execStatsWrapper));
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(vectorSearchStage,
                           DocumentSourceVectorSearch::id,
                           documentSourceVectorSearchToStageFn);

namespace {

class StatsProviderImpl : public DSVectorSearchExecStatsWrapper::StatsProvider {
public:
    StatsProviderImpl(executor::TaskExecutorCursor* cursor) : _cursor(cursor) {}
    boost::optional<BSONObj> getStats() override {
        return _cursor->getCursorExplain();
    }

private:
    // The lifetime of this object is bound to the lifetime of the VectorSearchStage, so the
    // pointer is always valid.
    const executor::TaskExecutorCursor* _cursor;
};
}  // namespace

VectorSearchStage::VectorSearchStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::shared_ptr<executor::TaskExecutor>& taskExecutor,
    BSONObj originalSpec,
    const std::shared_ptr<DSVectorSearchExecStatsWrapper>& execStatsWrapper)
    : Stage(stageName, expCtx),
      _taskExecutor(taskExecutor),
      _execStatsWrapper(execStatsWrapper),
      _originalSpec(originalSpec.getOwned()) {}

boost::optional<BSONObj> VectorSearchStage::getNext() try {
    return _cursor->getNext(pExpCtx->getOperationContext());
} catch (DBException& ex) {
    ex.addContext("Remote error from mongot");
    throw;
}

GetNextResult VectorSearchStage::getNextAfterSetup() {
    auto response = getNext();
    auto& opDebug = CurOp::get(pExpCtx->getOperationContext())->debug();

    if (opDebug.msWaitingForMongot) {
        *opDebug.msWaitingForMongot += durationCount<Milliseconds>(_cursor->resetWaitingTime());
    } else {
        opDebug.msWaitingForMongot = durationCount<Milliseconds>(_cursor->resetWaitingTime());
    }
    opDebug.mongotBatchNum = _cursor->getBatchNum();

    // The TaskExecutorCursor will store '0' as its CursorId if the cursor to mongot is exhausted.
    // If we already have a cursorId from a previous call, just use that.
    if (!_cursorId) {
        _cursorId = _cursor->getCursorId();
    }

    opDebug.mongotCursorId = _cursorId;

    if (!response) {
        return GetNextResult::makeEOF();
    }

    // Populate $sortKey metadata field so that downstream operators can correctly reason about the
    // sort order. This can be important for mongos, so it can properly merge sort the document
    // stream, or for $rankFusion to calculate the ranks of the results. This metadata can be safely
    // ignored if nobody ends up needing it. It will be stripped out before a response is sent back
    // to a client.

    // Metadata can't be changed on a Document. Create a MutableDocument to set the sortKey.
    MutableDocument output(Document::fromBsonWithMetaData(response.value()));

    tassert(7828500,
            "Expected vector search distance to be present",
            output.metadata().hasVectorSearchScore());
    output.metadata().setSortKey(Value{output.metadata().getVectorSearchScore()},
                                 true /* isSingleElementKey */);
    return output.freeze();
}

GetNextResult VectorSearchStage::doGetNext() {
    // Return EOF if pExpCtx->getUUID() is unset here; the collection we are searching over has not
    // been created yet.
    if (!pExpCtx->getUUID()) {
        return GetNextResult::makeEOF();
    }

    if (pExpCtx->getExplain() &&
        !feature_flags::gFeatureFlagSearchExplainExecutionStats.isEnabled()) {
        return GetNextResult::makeEOF();
    }

    // If this is the first call, establish the cursor.
    if (!_cursor) {
        _cursor =
            search_helpers::establishVectorSearchCursor(pExpCtx, _originalSpec, _taskExecutor);
        auto statsProvider = std::make_unique<StatsProviderImpl>(_cursor.get());
        _execStatsWrapper->setStatsProvider(std::move(statsProvider));
    }

    return getNextAfterSetup();
}

}  // namespace exec::agg
}  // namespace mongo
