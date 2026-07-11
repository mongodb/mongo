// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_explainer_factory.h"

#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_explainer_sbe.h"

#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo::plan_explainer_factory {

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash,
                                    boost::optional<std::string> replanReason) {
    return make(root, cachedPlanHash, std::move(replanReason), boost::none);
}

std::unique_ptr<PlanExplainer> make(PlanStage* root,
                                    boost::optional<size_t> cachedPlanHash,
                                    boost::optional<std::string> replanReason,
                                    boost::optional<PlanExplainerData> maybeExplainData) {
    return std::make_unique<PlanExplainerImpl>(
        root, cachedPlanHash, std::move(replanReason), std::move(maybeExplainData));
}

std::unique_ptr<PlanExplainer> make(PlanStage* root, const PlanEnumeratorExplainInfo& explainInfo) {
    return std::make_unique<PlanExplainerImpl>(root, explainInfo);
}

std::unique_ptr<PlanExplainer> make(
    sbe::PlanStage* root,
    const NamespaceString& nss,
    const stage_builder::PlanStageData* data,
    const QuerySolution* solution,
    bool isMultiPlan,
    bool isFromPlanCache,
    boost::optional<size_t> cachedPlanHash,
    std::shared_ptr<const plan_cache_debug_info::DebugInfoSBE> debugInfoSBE,
    std::unique_ptr<PlanStage> classicRuntimePlannerStage,
    RemoteExplainVector* remoteExplains,
    bool usedJoinOpt,
    cost_based_ranker::EstimateMap estimates,
    std::vector<JoinOptPlan> rejectedPlans,
    boost::optional<PlanExplainerData> maybeExplainData) {
    if (!debugInfoSBE) {
        debugInfoSBE = std::make_shared<const plan_cache_debug_info::DebugInfoSBE>(
            plan_cache_util::buildDebugInfo(nss, solution));
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
        remoteExplains,
        usedJoinOpt,
        std::move(estimates),
        std::move(rejectedPlans),
        std::move(maybeExplainData));
}
}  // namespace mongo::plan_explainer_factory
