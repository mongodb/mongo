// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::plan_explainer_factory {

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash = boost::none,
                                    boost::optional<std::string> replanReason = boost::none);

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash,
                                    boost::optional<std::string> replanReason,
                                    boost::optional<PlanExplainerData> maybeExplainData);

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    const PlanEnumeratorExplainInfo& enumeratorInfo);

/**
 * Factory function used to create a PlanExplainer for classic multiplanner + SBE execution. It
 * requires a pointer to a classic multiplanner stage from which a classic PlanExplainer can be
 * created.
 * 'nss' is the NamespaceString for the main collection (the collection the original query was
 * written against).
 */
std::unique_ptr<PlanExplainer> make(
    sbe::PlanStage* root,
    const NamespaceString& nss,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfo,
    std::unique_ptr<PlanStage> classicRuntimePlannerStage,
    RemoteExplainVector* remoteExplains = nullptr,
    bool usedJoinOpt = false,
    cost_based_ranker::EstimateMap estimates = {},
    std::vector<JoinOptPlan> rejectedPlans = {},
    boost::optional<PlanExplainerData> maybeExplainData = boost::none);
}  // namespace mongo::plan_explainer_factory
