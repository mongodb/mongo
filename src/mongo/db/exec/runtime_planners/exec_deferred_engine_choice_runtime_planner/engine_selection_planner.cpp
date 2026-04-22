/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/exec/runtime_planners/exec_deferred_engine_choice_runtime_planner/planner_interface.h"
#include "mongo/db/query/engine_selection.h"

namespace mongo::exec_deferred_engine_choice {
namespace {

// Returns true if multiplanning reached EOF and the query is fully answered, so we can
// skip engine selection and use classic directly. This avoids unnecessarily lowering to
// SBE when the query has already been completed during multiplanning.
bool useEofOptimization(const PlanRankingResult& result,
                        const CanonicalQuery* cq,
                        const Pipeline* pipeline) {
    if (!result.execState) {
        return false;
    }
    auto* classicExecState = result.execState->peekExecState<ClassicExecState>();
    if (!classicExecState) {
        return false;
    }
    const PlanStage* planStage = classicExecState->root.get();
    if (!planStage || planStage->stageType() != STAGE_MULTI_PLAN) {
        return false;
    }
    auto mps = static_cast<const MultiPlanStage*>(planStage);
    return mps && mps->bestSolutionEof() &&
        // If explain is used, go through regular engine selection to display which engine is
        // targeted if EOF is not reached during multiplanning.
        !cq->getExpCtxRaw()->getExplain() &&
        // We can't use EOF optimization if pipeline is present. Because we may need to execute
        // the pipeline part in SBE, we have to rebuild and rerun the whole query.
        // Use `pipeline` since the CQ pipeline won't be populated at this point.
        (!pipeline || pipeline->empty()) &&
        // We want more coverage for engine selection in debug builds.
        !kDebugBuild;
}

}  // namespace

EngineSelectionPlanner::EngineSelectionPlanner(std::unique_ptr<PlannerInterface> innerPlanner,
                                               OperationContext* opCtx,
                                               CanonicalQuery* cq,
                                               Pipeline* pipeline,
                                               const MultipleCollectionAccessor& collections) {
    _result = innerPlanner->extractPlanRankingResult();

    // Engine selection is only needed for non-trivial cases. ID hack and cached planners already
    // set engineSelection/usedIdhack.
    if (_result.usedIdhack || _result.engineSelection) {
        return;
    }

    auto& solution = _result.solutions[0];
    // There are two EOF-related optimizations:
    //     - If a non-existent collection is queried, we just create an EOF plan.
    //     - If multiplanning was used and a plan reached an EOF state, the query
    //       has been fully answered and there's no more execution that needs to
    //       happen. In this case we return a classic executor so that we don't
    //       have to restart work using SBE.
    if (solution->root()->getType() == STAGE_EOF || useEofOptimization(_result, cq, pipeline)) {
        _result.engineSelection = EngineChoice::kClassic;
        return;
    }

    _result.engineSelection = extendSolutionAndSelectEngine(
        solution, opCtx, cq, pipeline, collections, *_result.plannerParams);
}

PlanRankingResult EngineSelectionPlanner::extractPlanRankingResult() {
    return std::move(_result);
}

}  // namespace mongo::exec_deferred_engine_choice
