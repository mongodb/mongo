// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

/**
 * This class handles the execution part of the setVariableFromSubPipeline aggregation stage and is
 * part of the execution pipeline. Its construction is based on
 * DocumentSourceSetVariableFromSubPipeline, which handles the optimization part.
 */
class SetVariableFromSubPipelineStage final : public Stage {
public:
    SetVariableFromSubPipelineStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        std::shared_ptr<SetVariableFromSubPipelineSharedState> sharedState,
        std::shared_ptr<mongo::Pipeline> subPipeline,
        Variables::Id variableID);

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;
    bool validateOperationContext(const OperationContext* opCtx) const final;

private:
    void doDispose() final;
    GetNextResult doGetNext() final;

    const std::shared_ptr<SetVariableFromSubPipelineSharedState> _sharedState;
    std::shared_ptr<mongo::Pipeline> _subPipeline;
    Variables::Id _variableID;
    // $setVariableFromSubPipeline sets the value of $$SEARCH_META only on the first call to
    // doGetNext().
    bool _firstCallForInput = true;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
