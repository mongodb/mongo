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


#include "mongo/db/exec/agg/union_with_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

exec::agg::StagePtr documentSourceUnionWithToStageFn(
    const boost::intrusive_ptr<const DocumentSource>& documentSource) {
    auto unionWith = boost::dynamic_pointer_cast<const DocumentSourceUnionWith>(documentSource);
    tassert(1042403, "Expect 'DocumentSourceUnionWith' type", unionWith);

    return make_intrusive<exec::agg::UnionWithStage>(DocumentSourceUnionWith::kStageName,
                                                     unionWith->getExpCtx(),
                                                     unionWith->getSharedState(),
                                                     unionWith->_userNss);
}

namespace exec::agg {

namespace {

REGISTER_AGG_STAGE_MAPPING(unionWith,
                           mongo::DocumentSourceUnionWith::id,
                           documentSourceUnionWithToStageFn);

// The use of these logging macros is done in separate NOINLINE functions to reduce the stack space
// used on the hot getNext() path. This is done to avoid stack overflows.
MONGO_COMPILER_NOINLINE void logStartingSubPipeline(const std::vector<BSONObj>& serializedPipe) {
    LOGV2_DEBUG(1042401,
                1,
                "$unionWith attaching cursor to pipeline {pipeline}",
                "pipeline"_attr = serializedPipe);
}

MONGO_COMPILER_NOINLINE void logShardedViewFound(
    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e,
    const mongo::Pipeline& new_pipeline) {
    LOGV2_DEBUG(1042402,
                3,
                "$unionWith found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$unionWith sub-pipeline: {new_pipe}",
                logAttrs(e->getNamespace()),
                "pipeline"_attr = Value(e->getPipeline()),
                "new_pipe"_attr = new_pipeline.serializeToBson());
}

template <size_t N>
MONGO_COMPILER_NOINLINE void logPipeline(int32_t id,
                                         const char (&msg)[N],
                                         const mongo::Pipeline& pipe) {
    LOGV2_DEBUG(id, 5, msg, "pipeline"_attr = pipe.serializeToBson());
}

}  // namespace

UnionWithStage::UnionWithStage(const StringData stageName,
                               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               const std::shared_ptr<UnionWithSharedState>& sharedState,
                               const NamespaceString& userNss)
    : Stage(stageName, pExpCtx), _sharedState(sharedState), _userNss(userNss) {}

GetNextResult UnionWithStage::doGetNext() {
    if (!_sharedState->_pipeline) {
        // We must have already been disposed, so we're finished.
        return GetNextResult::makeEOF();
    }

    if (_sharedState->_executionState ==
        UnionWithSharedState::ExecutionProgress::kIteratingSource) {
        if (auto nextInput = pSource->getNext(); !nextInput.isEOF()) {
            return nextInput;
        }

        // All documents from the base collection have been returned, switch to iterating the sub-
        // pipeline by falling through below.
        _sharedState->_executionState =
            UnionWithSharedState::ExecutionProgress::kStartingSubPipeline;
    }

    if (_sharedState->_executionState ==
        UnionWithSharedState::ExecutionProgress::kStartingSubPipeline) {
        auto serializedPipeline = _sharedState->_pipeline->serializeToBson();

        // Prepare the sub pipeline. This is expected to fail if the command is not supported on a
        // sharded view.
        try {
            prepareSubPipeline(serializedPipeline);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // Preparation of sub pipeline failed. The pipeline will be modified to use the view
            // definition instead, and we attempt to prepare it again.
            _sharedState->_pipeline = DocumentSourceUnionWith::parsePipelineWithMaybeViewDefinition(
                pExpCtx,
                ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                std::move(serializedPipeline),
                _userNss);
            logShardedViewFound(e, *_sharedState->_pipeline);

            // Serialize the new pipeline.
            serializedPipeline = _sharedState->_pipeline->serializeToBson();

            // If this throws again, the exception will bubble up and will be caught by an outer
            // layer.
            prepareSubPipeline(serializedPipeline);
        }
    }

    if (auto res = _sharedState->_execPipeline->getNext()) {
        return std::move(*res);
    }

    // Record the plan summary stats after $unionWith operation is done.
    _sharedState->_execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);

    // stats update (previously done in usedDisk())
    _stats.planSummaryStats.usedDisk =
        _stats.planSummaryStats.usedDisk || _sharedState->_execPipeline->usedDisk();

    _sharedState->_executionState = UnionWithSharedState::ExecutionProgress::kFinished;
    return GetNextResult::makeEOF();
}

void UnionWithStage::prepareSubPipeline(const std::vector<BSONObj>& serializedPipeline) {
    // Since the subpipeline will be executed again for explain, we store the starting state of
    // the variables to reset them later.
    if (pExpCtx->getExplain()) {
        auto expCtx = _sharedState->_pipeline->getContext();
        _sharedState->_variables = expCtx->variables;
        _sharedState->_variablesParseState =
            expCtx->variablesParseState.copyWith(_sharedState->_variables.useIdGenerator());
    }

    logStartingSubPipeline(serializedPipeline);

    // Query settings are looked up after parsing and therefore are not populated in the
    // context of the unionWith '_pipeline' as part of DocumentSourceUnionWith constructor.
    // Attach query settings to the '_pipeline->getContext()' by copying them from the
    // parent query ExpressionContext.
    const boost::intrusive_ptr<ExpressionContext>& pipelineCtx =
        _sharedState->_pipeline->getContext();
    pipelineCtx->setQuerySettingsIfNotPresent(pExpCtx->getQuerySettings());

    logPipeline(104243, "$unionWith before pipeline prep: ", *_sharedState->_pipeline);

    _sharedState->_pipeline =
        pExpCtx->getMongoProcessInterface()->finalizeAndMaybePreparePipelineForExecution(
            pipelineCtx,
            std::move(_sharedState->_pipeline),
            true /* attachCursorAfterOptimizing */,
            pipeline_optimization::optimizeAndValidatePipeline);
    logPipeline(104244, "$unionWith POST pipeline prep: ", *_sharedState->_pipeline);

    _sharedState->_executionState = UnionWithSharedState::ExecutionProgress::kIteratingSubPipeline;

    _sharedState->_execPipeline = exec::agg::buildPipeline(_sharedState->_pipeline->freeze());

    // The $unionWith stage takes responsibility for disposing of its Pipeline. When the outer
    // Pipeline that contains the $unionWith is disposed of, it will propagate dispose() to its
    // subpipeline.
    _sharedState->_execPipeline->dismissDisposal();
}

bool UnionWithStage::usedDisk() const {
    return _sharedState->_execPipeline && _sharedState->_execPipeline->usedDisk();
}

void UnionWithStage::doDispose() {
    // Update execution statistics.
    if (_sharedState->_execPipeline) {
        _stats.planSummaryStats.usedDisk =
            _stats.planSummaryStats.usedDisk || _sharedState->_execPipeline->usedDisk();
        _sharedState->_execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);
    }

    // When not in explain command, propagate disposal to the subpipeline, otherwise the subpipeline
    // will be disposed in '~UnionWithSharedState()'.
    if (!pExpCtx->getExplain()) {
        if (_sharedState->_execPipeline) {
            _sharedState->_execPipeline->reattachToOperationContext(pExpCtx->getOperationContext());
            _sharedState->_execPipeline->dispose();
        }
        _sharedState->_pipeline.reset();
        _sharedState->_execPipeline.reset();
    }
}

void UnionWithStage::detachFromOperationContext() {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // detach it from the operation context when it goes out of scope.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->detachFromOperationContext();
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->detachFromOperationContext();
    }
}

void UnionWithStage::reattachToOperationContext(OperationContext* opCtx) {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // propagate the new operation context to the pipeline stages.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->reattachToOperationContext(opCtx);
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->reattachToOperationContext(opCtx);
    }
}

bool UnionWithStage::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        (!_sharedState->_execPipeline ||
         _sharedState->_execPipeline->validateOperationContext(opCtx));
}
}  // namespace exec::agg
}  // namespace mongo
