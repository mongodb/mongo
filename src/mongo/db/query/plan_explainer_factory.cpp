/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/query/plan_explainer_factory.h"

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_explainer_sbe.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo::plan_explainer_factory {

std::unique_ptr<PlanExplainer> make(PlanStage* root, boost::optional<size_t> cachedPlanHash) {
    return make(root,
                cachedPlanHash,
                QueryPlanner::CostBasedRankerResult{},
                stage_builder::PlanStageToQsnMap{},
                {} /* cbrRejectedPlanStages */);
}

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash,
                                    QueryPlanner::CostBasedRankerResult cbrResult,
                                    stage_builder::PlanStageToQsnMap planStageQsnMap,
                                    std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages) {
    return std::make_unique<PlanExplainerImpl>(root,
                                               cachedPlanHash,
                                               std::move(cbrResult),
                                               std::move(planStageQsnMap),
                                               std::move(cbrRejectedPlanStages));
}

std::unique_ptr<PlanExplainer> make(PlanStage* root, const PlanEnumeratorExplainInfo& explainInfo) {
    return std::make_unique<PlanExplainerImpl>(root, explainInfo);
}

std::unique_ptr<PlanExplainer> make(
    sbe::PlanStage* root,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfoSBE,
    std::unique_ptr<PlanStage> classicRuntimePlannerStage,
    RemoteExplainVector* remoteExplains) {
    if (!debugInfoSBE) {
        debugInfoSBE = std::make_shared<const plan_cache_debug_info::DebugInfoSBE>(
            plan_cache_util::buildDebugInfo(solution));
    }
    return std::make_unique<PlanExplainerClassicRuntimePlannerForSBE>(
        root,
        data,
        solution,
        isMultiPlan,
        isFromPlanCache,
        cachedPlanHash,
        debugInfoSBE,
        std::move(classicRuntimePlannerStage),
        remoteExplains);
}
}  // namespace mongo::plan_explainer_factory
