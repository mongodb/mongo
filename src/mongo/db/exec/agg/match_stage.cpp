// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/match_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceMatchToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto matchDS = boost::dynamic_pointer_cast<DocumentSourceMatch>(documentSource);

    tassert(10422700, "expected 'DocumentSourceMatch' type", matchDS);

    return make_intrusive<exec::agg::MatchStage>(
        matchDS->kStageName, matchDS->getExpCtx(), matchDS->_matchProcessor, matchDS->_isTextQuery);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(match, DocumentSourceMatch::id, documentSourceMatchToStageFn)

MatchStage::MatchStage(std::string_view stageName,
                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                       const std::shared_ptr<MatchProcessor>& matchProcessor,
                       bool isTextQuery)
    : Stage(stageName, pExpCtx),
      _matchProcessor(matchProcessor),
      _memoryTracker(
          OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*pExpCtx)) {
    // The user facing error should have been generated earlier.
    massert(17309, "Should never call getNext on a $match stage with $text clause", !isTextQuery);
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() &&
        feature_flags::gFeatureFlagExpressionMemoryTracking.isEnabled()) {
        _expressionEvalCtx.tracker = &_memoryTracker;
    }
    _expressionEvalCtx.stageName = _commonStats.stageTypeStr;
}

GetNextResult MatchStage::doGetNext() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        if (_matchProcessor->process(nextInput.getDocument(), _expressionEvalCtx)) {
            return nextInput;
        }

        // For performance reasons, a streaming stage must not keep references to documents
        // across calls to getNext(). Such stages must retrieve a result from their child and
        // then release it (or return it) before asking for another result. Failing to do so can
        // result in extra work, since the Document/Value library must copy data on write when
        // that data has a refcount above one.
        nextInput.releaseDocument();
    }

    return nextInput;
}

Document MatchStage::getExplainOutput(const query_shape::SerializationOptions& opts) const {
    MutableDocument out(Stage::getExplainOutput(opts));
    if (_expressionEvalCtx.tracker) {
        out["expressionEvaluationPeakMemoryBytes"] = opts.serializeLiteral(
            static_cast<long long>(_expressionEvalCtx.tracker->peakTrackedMemoryBytes()));
    }
    return out.freeze();
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
