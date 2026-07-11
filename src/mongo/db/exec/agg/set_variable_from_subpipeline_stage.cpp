// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/set_variable_from_subpipeline_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceSetVariableFromSubPipelineToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto ds = boost::dynamic_pointer_cast<DocumentSourceSetVariableFromSubPipeline>(documentSource);

    tassert(10577600, "expected 'DocumentSourceSetVariableFromSubPipeline' type", ds);

    return make_intrusive<exec::agg::SetVariableFromSubPipelineStage>(
        ds->kStageName, ds->getExpCtx(), ds->_sharedState, ds->_subPipeline, ds->_variableID);
}

}  // namespace mongo

namespace mongo::exec::agg {

REGISTER_AGG_STAGE_MAPPING(setVariableFromSubPipeline,
                           DocumentSourceSetVariableFromSubPipeline::id,
                           documentSourceSetVariableFromSubPipelineToStageFn);

SetVariableFromSubPipelineStage::SetVariableFromSubPipelineStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::shared_ptr<SetVariableFromSubPipelineSharedState> sharedState,
    std::shared_ptr<mongo::Pipeline> subPipeline,
    Variables::Id variableID)
    : Stage(stageName, pExpCtx),
      _sharedState(std::move(sharedState)),
      _subPipeline(std::move(subPipeline)),
      _variableID(variableID) {}

bool SetVariableFromSubPipelineStage::validateOperationContext(
    const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        (!_sharedState->_subExecPipeline ||
         _sharedState->_subExecPipeline->validateOperationContext(opCtx));
}

void SetVariableFromSubPipelineStage::reattachToOperationContext(OperationContext* opCtx) {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // propagate the new operation context to the pipeline stages.
    if (_sharedState->_subExecPipeline) {
        _sharedState->_subExecPipeline->reattachToOperationContext(opCtx);
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_subPipeline) {
        _subPipeline->reattachToOperationContext(opCtx);
    }
}

void SetVariableFromSubPipelineStage::detachFromOperationContext() {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // detach it from the operation context when it goes out of scope.
    if (_sharedState->_subExecPipeline) {
        _sharedState->_subExecPipeline->detachFromOperationContext();
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_subPipeline) {
        _subPipeline->detachFromOperationContext();
    }
}

void SetVariableFromSubPipelineStage::doDispose() {
    if (_sharedState->_subExecPipeline) {
        _sharedState->_subExecPipeline->dispose();
        _sharedState->_subExecPipeline.reset();
    }
    _subPipeline.reset();
}

DocumentSource::GetNextResult SetVariableFromSubPipelineStage::doGetNext() {
    if (_firstCallForInput) {
        tassert(6448002,
                "Expected to have already attached a cursor source to the pipeline",
                !_subPipeline->peekFront()->constraints().requiresInputDocSource);
        tassert(10713600,
                "Cannot create an execution pipeline when it already exists",
                !_sharedState->_subExecPipeline);
        _sharedState->_subExecPipeline = exec::agg::buildPipeline(_subPipeline->freeze());
        auto nextSubPipelineInput = _sharedState->_subExecPipeline->getNext();
        uassert(625296,
                "No document returned from $SetVariableFromSubPipeline subpipeline",
                nextSubPipelineInput);
        uassert(625297,
                "Multiple documents returned from $SetVariableFromSubPipeline subpipeline when "
                "only one expected",
                !_sharedState->_subExecPipeline->getNext());
        pExpCtx->variables.setReservedValue(_variableID, Value(*nextSubPipelineInput), true);
    }
    _firstCallForInput = false;
    return pSource->getNext();
}

}  // namespace mongo::exec::agg
