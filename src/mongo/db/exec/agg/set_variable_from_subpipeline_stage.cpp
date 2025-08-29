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

#include "mongo/db/exec/agg/set_variable_from_subpipeline_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"

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
    StringData stageName,
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
    // TODO SERVER-102417: Remove the following if-block when all sources are split into
    // QO and QE parts and the QO stage auto-disposes resources in destructor.
    if (_subPipeline && !_sharedState->_subExecPipeline) {
        // Create an execution pipeline to make sure the resources are correctly disposed.
        _sharedState->_subExecPipeline = exec::agg::buildPipeline(_subPipeline->freeze());
        _sharedState->_subExecPipeline->reattachToOperationContext(pExpCtx->getOperationContext());
    }
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
