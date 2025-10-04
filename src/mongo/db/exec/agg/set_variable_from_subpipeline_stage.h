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

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"

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
        StringData stageName,
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
