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

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

namespace mongo::plan_explainer_factory {

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash = boost::none);

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash,
                                    QueryPlanner::CostBasedRankerResult cbrResult,
                                    stage_builder::PlanStageToQsnMap planStageQsnMap,
                                    std::vector<std::unique_ptr<PlanStage>> cbrRejectedPlanStages);

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    const PlanEnumeratorExplainInfo& enumeratorInfo);

/**
 * Factory function used to create a PlanExplainer for classic multiplanner + SBE execution. It
 * requires a pointer to a classic multiplanner stage from which a classic PlanExplainer can be
 * created.
 */
std::unique_ptr<PlanExplainer> make(
    sbe::PlanStage* root,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo,
    std::unique_ptr<PlanStage> classicRuntimePlannerStage,
    RemoteExplainVector* remoteExplains = nullptr);
}  // namespace mongo::plan_explainer_factory
