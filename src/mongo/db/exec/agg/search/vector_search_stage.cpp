// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/search/vector_search_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/extension/host/extension_vector_search_server_status.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/vector_search_helper.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceVectorSearchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& source) {
    auto documentSource = dynamic_cast<DocumentSourceVectorSearch*>(source.get());

    tassert(10807800, "expected 'DocumentSourceVectorSearch' type", documentSource);

    // Increment legacyVectorSearchQueryCount when DocumentSourceVectorSearch is converted to
    // executable stage.
    vector_search_metrics::legacyVectorSearchQueryCount.increment(1);

    auto execStatsWrapper = std::make_shared<DSVectorSearchExecStatsWrapper>();
    documentSource->_execStatsWrapper = execStatsWrapper;
    return make_intrusive<exec::agg::VectorSearchStage>(documentSource->kStageName,
                                                        documentSource->getExpCtx(),
                                                        documentSource->_taskExecutor,
                                                        documentSource->_stageSpec.copy(),
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
    std::string_view stageName,
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
