// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/redact_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_redact.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

boost::intrusive_ptr<exec::agg::Stage> documentSourceRedactToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* redactDS = dynamic_cast<DocumentSourceRedact*>(documentSource.get());

    tassert(10816600, "expected 'DocumentSourceRedact' type", redactDS);

    return make_intrusive<exec::agg::RedactStage>(
        redactDS->kStageName, redactDS->getExpCtx(), redactDS->getRedactProcessor());
}

REGISTER_AGG_STAGE_MAPPING(redact, DocumentSourceRedact::id, documentSourceRedactToStageFn);

RedactStage::RedactStage(std::string_view stageName,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const std::shared_ptr<RedactProcessor>& redactProcessor)
    : Stage(stageName, expCtx),
      _redactProcessor{redactProcessor},
      _memoryTracker(
          OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*expCtx)) {
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() &&
        feature_flags::gFeatureFlagExpressionMemoryTracking.isEnabled()) {
        _expressionEvalCtx.tracker = &_memoryTracker;
    }
    _expressionEvalCtx.stageName = _commonStats.stageTypeStr;
}

GetNextResult RedactStage::doGetNext() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        if (boost::optional<Document> result =
                _redactProcessor->process(nextInput.releaseDocument(), _expressionEvalCtx)) {
            return std::move(*result);
        }
    }
    return nextInput;
}

}  // namespace mongo::exec::agg
