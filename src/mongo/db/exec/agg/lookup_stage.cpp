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

#include "mongo/db/exec/agg/lookup_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceLookUpToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* lookupDS = dynamic_cast<DocumentSourceLookUp*>(documentSource.get());

    tassert(10423100, "expected 'DocumentSourceLookUp' type", lookupDS);

    return make_intrusive<exec::agg::LookUpStage>(
        lookupDS->kStageName, lookupDS->getExpCtx(), lookupDS->getSharedState());
};

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(lookup, DocumentSourceLookUp::id, documentSourceLookUpToStageFn)

LookUpStage::LookUpStage(StringData stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         std::shared_ptr<LookUpSharedState> sharedState)
    : Stage(stageName, pExpCtx), _sharedState(sharedState) {}

GetNextResult LookUpStage::doGetNext() {
    if (_sharedState->unwindSrc) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    auto inputDoc = nextInput.releaseDocument();

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a
    // $unwind,
    // '_sharedState->unwindSrc' would be non-null, and we would not have made it here.
    invariant(!_sharedState->matchSrc);

    std::unique_ptr<mongo::Pipeline> pipeline;
    std::unique_ptr<Pipeline> execPipeline;
    try {
        pipeline = _sharedState->buildPipeline(_sharedState->fromExpCtx, inputDoc, pExpCtx);
        execPipeline = buildPipeline(pipeline->freeze());
        LOGV2_DEBUG(
            9497000, 5, "Built pipeline", "pipeline"_attr = pipeline->serializeForLogging());
    } catch (const ExceptionFor<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is
        // sharded, throw a custom exception.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>(); staleInfo &&
            staleInfo->getVersionWanted() &&
            staleInfo->getVersionWanted() != ShardVersion::UNSHARDED()) {
            uassert(3904800,
                    "Cannot run $lookup with a sharded foreign collection in a transaction",
                    _sharedState->isForeignShardedLookupAllowed);
        }
        throw;
    }

    std::vector<Value> results;
    long long objsize = 0;
    const auto maxBytes = internalLookupStageIntermediateDocumentMaxSizeBytes.load();

    LOGV2_DEBUG(9497001, 5, "Beginning to iterate sub-pipeline");
    while (auto result = execPipeline->getNext()) {
        long long safeSum = 0;
        bool hasOverflowed = overflow::add(objsize, result->getApproximateSize(), &safeSum);
        uassert(4568,
                str::stream() << "Total size of documents in " << _sharedState->fromNs.coll()
                              << " matching pipeline's $lookup stage exceeds " << maxBytes
                              << " bytes",

                !hasOverflowed && objsize <= maxBytes);
        objsize = safeSum;
        results.emplace_back(std::move(*result));
    }
    execPipeline->accumulatePlanSummaryStats(_sharedState->stats.planSummaryStats);

    _sharedState->stats.planSummaryStats.usedDisk |= execPipeline->usedDisk();

    MutableDocument output(std::move(inputDoc));
    output.setNestedField(_sharedState->as, Value(std::move(results)));
    return output.freeze();
}

GetNextResult LookUpStage::unwindResult() {
    const auto& indexPath(_sharedState->unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_sharedState->pipeline || !_nextValue) {
        // Accumulate stats from the pipeline for the previous input, if applicable. This is to
        // avoid missing the accumulation of stats on an early exit (below) if the input (i.e.,
        // left side of the lookup) is done.
        if (_sharedState->execPipeline) {
            _sharedState->execPipeline->accumulatePlanSummaryStats(
                _sharedState->stats.planSummaryStats);
            _sharedState->execPipeline->dispose(pExpCtx->getOperationContext());
        }

        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _input = nextInput.releaseDocument();

        _sharedState->pipeline =
            _sharedState->buildPipeline(_sharedState->fromExpCtx, *_input, pExpCtx);
        _sharedState->execPipeline = exec::agg::buildPipeline(_sharedState->pipeline->freeze());

        // The $lookup stage takes responsibility for disposing of its Pipeline, since it will
        // potentially be used by multiple OperationContexts, and the $lookup stage is part of
        // an outer Pipeline that will propagate dispose() calls before being destroyed.
        _sharedState->execPipeline->dismissDisposal();

        _cursorIndex = 0;
        _nextValue = _sharedState->execPipeline->getNext();

        if (_sharedState->unwindSrc->preserveNullAndEmptyArrays() && !_nextValue) {
            // There were no results for this cursor, but the $unwind was asked to preserve
            // empty arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly create objects in the prefix of
            // '_sharedState->as', to act as if we had created an empty array and then
            // removed it.
            output.setNestedField(_sharedState->as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }

    invariant(bool(_input) && bool(_nextValue));
    auto currentValue = std::move(*_nextValue);
    _nextValue = _sharedState->execPipeline->getNext();

    // Move input document into output if this is the last or only result, otherwise perform a
    // copy.
    MutableDocument output(_nextValue ? *_input : std::move(*_input));
    output.setNestedField(_sharedState->as, Value(currentValue));

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    ++_cursorIndex;
    return output.freeze();
}

void LookUpStage::doDispose() {
    if (_sharedState->execPipeline) {
        _sharedState->execPipeline->accumulatePlanSummaryStats(
            _sharedState->stats.planSummaryStats);
        _sharedState->execPipeline->dispose(pExpCtx->getOperationContext());
        _sharedState->execPipeline.reset();
    }
    if (_sharedState->pipeline) {
        _sharedState->pipeline.reset();
    }
}

void LookUpStage::detachFromOperationContext() {
    if (_sharedState->pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so
        // we use Pipeline::detachFromOperationContext() to take care of updating
        // '_sharedState->fromExpCtx->getOperationContext()'.
        tassert(10713704,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' "
                "is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
        tassert(10713705,
                "expecting _sharedState->fromExpCtx->getOperationContext() == nullptr",
                _sharedState->fromExpCtx->getOperationContext() == nullptr);
    }
    if (_sharedState->fromExpCtx) {
        _sharedState->fromExpCtx->setOperationContext(nullptr);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->detachFromOperationContext();
    }
}

void LookUpStage::reattachToOperationContext(OperationContext* opCtx) {
    if (_sharedState->pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so
        // we use Pipeline::reattachToOperationContext() to take care of updating
        // '_sharedState->fromExpCtx->getOperationContext()'.
        tassert(10713708,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' "
                "is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
        tassert(10713709,
                "expecting _sharedState->fromExpCtx->getOperationContext() == opCtx",
                _sharedState->fromExpCtx->getOperationContext() == opCtx);
    }
    if (_sharedState->fromExpCtx) {
        _sharedState->fromExpCtx->setOperationContext(opCtx);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->reattachToOperationContext(opCtx);
    }
}

bool LookUpStage::validateOperationContext(const OperationContext* opCtx) const {
    if (getContext()->getOperationContext() != opCtx ||
        (_sharedState->fromExpCtx && _sharedState->fromExpCtx->getOperationContext() != opCtx)) {
        return false;
    }
    if (_sharedState->execPipeline &&
        !_sharedState->execPipeline->validateOperationContext(opCtx)) {
        return false;
    }
    if (_sharedState->resolvedIntrospectionPipeline &&
        !_sharedState->resolvedIntrospectionPipeline->validateOperationContext(opCtx)) {
        return false;
    }

    return true;
}

bool LookUpStage::usedDisk() const {
    return _sharedState->execPipeline && _sharedState->execPipeline->usedDisk();
}

}  // namespace exec::agg
}  // namespace mongo
