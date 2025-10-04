/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"

namespace mongo::classic_runtime_planner {

SubPlanner::SubPlanner(PlannerData plannerData) : ClassicPlannerInterface(std::move(plannerData)) {
    SubplanStage::PlanSelectionCallbacks callbacks{
        // This callback is invoked on a per $or branch basis. The callback is constructed in the
        // "sometimes cache" mode. We currently do not support cached plan replanning for rooted $or
        // queries. Therefore, we must be more conservative about putting a potentially bad plan
        // into the cache in the subplan path.
        //
        // TODO SERVER-18777: Support replanning for rooted $or queries.
        .onPickPlanForBranch =
            plan_cache_util::ConditionalClassicPlanCacheWriter{
                plan_cache_util::ConditionalClassicPlanCacheWriter::Mode::SometimesCache,
                opCtx(),
                collections().getMainCollectionPtrOrAcquisition(),
                false /* executeInSbe */},

        .onPickPlanWholeQuery =
            plan_cache_util::ClassicPlanCacheWriter{
                opCtx(),
                collections().getMainCollectionPtrOrAcquisition(),
                false /* executeInSbe */},
    };

    auto root = std::make_unique<SubplanStage>(cq()->getExpCtxRaw(),
                                               collections().getMainCollectionPtrOrAcquisition(),
                                               ws(),
                                               cq(),
                                               std::move(callbacks));
    _subplanStage = root.get();
    setRoot(std::move(root));
}

Status SubPlanner::doPlan(PlanYieldPolicy* planYieldPolicy) {
    return _subplanStage->pickBestPlan(plannerParams(), planYieldPolicy);
}

std::unique_ptr<QuerySolution> SubPlanner::extractQuerySolution() {
    return nullptr;
}
}  // namespace mongo::classic_runtime_planner
