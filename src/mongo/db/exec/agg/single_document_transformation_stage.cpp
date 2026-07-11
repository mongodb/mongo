// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/single_document_transformation_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSingleDocumentTransformationToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto singleDocumentTransformationDS =
        boost::dynamic_pointer_cast<DocumentSourceSingleDocumentTransformation>(documentSource);

    tassert(10422800,
            "expected 'DocumentSourceSingleDocumentTransformation' type",
            singleDocumentTransformationDS);

    // Cache the stage options document in case this document source is serialized after
    // disposing this stage.
    singleDocumentTransformationDS->_cachedStageOptions =
        singleDocumentTransformationDS->getTransformer().serializeTransformation(
            query_shape::SerializationOptions{
                .verbosity = singleDocumentTransformationDS->getExpCtx()->getExplain()});

    return make_intrusive<exec::agg::SingleDocumentTransformationStage>(
        singleDocumentTransformationDS->getSourceName(),
        singleDocumentTransformationDS->getExpCtx(),
        singleDocumentTransformationDS->_transformationProcessor);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(singleDocumentTransformation,
                           DocumentSourceSingleDocumentTransformation::id,
                           documentSourceSingleDocumentTransformationToStageFn)

SingleDocumentTransformationStage::SingleDocumentTransformationStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const std::shared_ptr<SingleDocumentTransformationProcessor>& transformationProcessor)
    : Stage(stageName, pExpCtx),
      _ownedStageName(stageName),
      _transformationProcessor(transformationProcessor),
      _memoryTracker(
          OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(*pExpCtx)) {
    _commonStats.stageTypeStr = _ownedStageName;
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() &&
        feature_flags::gFeatureFlagExpressionMemoryTracking.isEnabled()) {
        _expressionEvalCtx.tracker = &_memoryTracker;
    }
    _expressionEvalCtx.stageName = _commonStats.stageTypeStr;
}

GetNextResult SingleDocumentTransformationStage::doGetNext() {
    if (!_transformationProcessor) {
        return GetNextResult::makeEOF();
    }

    // Get the next input document.
    auto input = pSource->getNext();

    if (!input.isAdvanced()) {
        return input;
    }

    return _transformationProcessor->process(input.releaseDocument(), _expressionEvalCtx);
}

void SingleDocumentTransformationStage::doDispose() {
    if (_transformationProcessor) {
        _transformationProcessor.reset();
    }
}

Document SingleDocumentTransformationStage::getExplainOutput(
    const query_shape::SerializationOptions& opts) const {
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
