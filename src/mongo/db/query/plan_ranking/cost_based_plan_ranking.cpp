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

#include "mongo/db/query/plan_ranking/cost_based_plan_ranking.h"

#include "mongo/base/status_with.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates.h"
#include "mongo/db/query/plan_ranking/cbr_plan_ranking.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryCE

namespace mongo {
namespace plan_ranking {

using namespace cost_based_ranker;

// TODO SERVER-115642 Implement a complete cost model for sampling CE
CostEstimate estimateCBRCost(const CanonicalQuery& query,
                             const std::vector<std::unique_ptr<QuerySolution>>& solutions) {
    const auto& qkc = query.getExpCtx()->getQueryKnobConfiguration();
    auto sampleSize = ce::SamplingEstimatorImpl::calculateSampleSize(
        qkc.getConfidenceInterval(), qkc.getSamplingMarginOfError());

    const auto randomSampleInc = CostCoefficient{CostCoefficientType{1400.0_ms}};
    const auto matchExprInc = CostCoefficient{CostCoefficientType{160.0_ms}};
    const CardinalityEstimate sampleCE{CardinalityType{static_cast<double>(sampleSize)},
                                       EstimationSource::Code};

    auto samplingCost = randomSampleInc * sampleCE;

    // The cost of CBR is proportional to:
    // - the number of plans
    // - the number of nodes per plan that will be estimated
    // - the size of the sample
    // - the cost of estimating one node against one sample document
    // We assume that each plan has 5 nodes that require estimation, and their estimation cost
    // is roughly the same as that of a MatchExpression.
    size_t numPlans = solutions.size();
    constexpr size_t numNodesPerPlan = 5;
    constexpr double matchExprChildCount = 2;
    auto estimationCost =
        numPlans * numNodesPerPlan * sampleCE * matchExprInc * matchExprChildCount;

    return samplingCost + estimationCost;
}

/**
 * This function can be called after any number of multi-plan trial works to pick the best plan
 * based on whatever information was collected so far. The function is mostly a wrapper for
 * pickBestPlan, and the logic that extracts the best plan from the MultiPlanner.
 */
StatusWith<QueryPlanner::PlanRankingResult> getBestMPPlan(
    classic_runtime_planner::MultiPlanner& mp) {
    auto status = mp.pickBestPlan();
    if (!status.isOK()) {
        return status;
    }
    QueryPlanner::PlanRankingResult out;
    auto soln = mp.extractQuerySolution();
    tassert(11306811, "Expected multi-planner to have returned a solution!", soln);
    out.solutions.push_back(std::move(soln));
    return out;
}

StatusWith<QueryPlanner::PlanRankingResult> getBestCBRPlan(
    OperationContext* opCtx,
    CanonicalQuery& query,
    QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections) {
    CBRPlanRankingStrategy cbrStrategy;
    plannerParams.planRankerMode = QueryPlanRankerModeEnum::kSamplingCE;
    auto result = cbrStrategy.rankPlans(opCtx, query, plannerParams, yieldPolicy, collections);
    plannerParams.planRankerMode = QueryPlanRankerModeEnum::kAutomaticCE;
    return result;
}

StatusWith<QueryPlanner::PlanRankingResult> CostBasedPlanRankingStrategy::rankPlans(
    OperationContext* opCtx,
    CanonicalQuery& query,
    QueryPlannerParams& plannerParams,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    const MultipleCollectionAccessor& collections,
    PlannerData plannerData) {
    // TODO SERVER-115496 refactor and move to plan_ranking
    auto topLevelSampleFieldNames =
        ce::extractTopLevelFieldsFromMatchExpression(query.getPrimaryMatchExpression());
    auto statusWithMultiPlanSolns =
        QueryPlanner::plan(query, plannerParams, topLevelSampleFieldNames);
    if (!statusWithMultiPlanSolns.isOK()) {
        return statusWithMultiPlanSolns.getStatus().withContext(
            str::stream() << "error processing query: " << query.toStringForErrorMsg()
                          << " planner returned error");
    }

    std::vector<std::unique_ptr<QuerySolution>> solutions =
        std::move(statusWithMultiPlanSolns.getValue());
    size_t numSolutions = solutions.size();

    if (solutions.size() == 1) {
        // TODO SERVER-115496 Make sure this short circuit logic is also taken to main plan_ranking
        // so it applies everywhere. Only one solution, no need to rank.
        QueryPlanner::PlanRankingResult out;
        out.solutions.push_back(std::move(solutions.front()));
        _ws = std::move(plannerData.workingSet);
        return out;
    }

    // Analyze all solutions for some structural properties
    size_t skipCount = 0;
    // TODO SERVER-115645 use the child of LIMIT/SORT nodes to estimate plan productivity
    // bool hasBlockingStage = false;
    for (auto& soln : solutions) {
        // TODO: for blocking stages use the productivity of the child node
        // if (soln->hasBlockingStage) {
        //     hasBlockingStage = true;
        // }
        if (soln->root()->getType() == STAGE_SKIP) {
            const auto skipNode = static_cast<const SkipNode*>(soln->root());
            skipCount = skipNode->skip;
        }
    }

    // Estimate the cost of CBR to generate a sample and estimate all plans against that sample.
    // This is done before we move 'solutions' into the new MultiPlanner below.
    const auto cbrCost = estimateCBRCost(query, solutions);
    tassert(11306808, "CBR cannot have 0 cost", cbrCost > zeroCost);

    auto mp = classic_runtime_planner::MultiPlanner(std::move(plannerData),
                                                    std::move(solutions),
                                                    // Empty PlanRakingResult used as an out param
                                                    // to return rejected plans.
                                                    QueryPlanner::PlanRankingResult{});

    auto trialConfig = mp.getTrialPhaseConfig();
    // These are the trial limits based on MP defaults or user-set requirements.
    const auto numWorksPerPlanMP = trialConfig.maxNumWorksPerPlan;
    const auto numResultsMP = trialConfig.targetNumResults;

    // Number of works that each plan should do in order to collect enough execution stats.
    size_t numWorksPerPlanEst = internalQueryNumWorksPerPlanForMPEstimation.load();
    // TODO SERVER-115645 use the child of LIMIT/SORT nodes to estimate plan productivity
    // see comment in MultiPlanStage::estimateAllPlans
    if (skipCount > 0) {
        constexpr double guessedProductivity = 0.3;
        // TODO: Add extra works for the skip, but not too many. Can be made smarter.
        size_t extraSkipWorks = std::min(skipCount / guessedProductivity, 5.0 * numWorksPerPlanEst);
        numWorksPerPlanEst += extraSkipWorks;
    }
    // Typically (numWorksPerPlanEst << numWorksPerPlanMP), but some tests may set the number of
    // works to a very small value.
    numWorksPerPlanEst = std::min(numWorksPerPlanEst, numWorksPerPlanMP);

    ON_BLOCK_EXIT([&] { _ws = mp.extractWorkingSet(); });

    // Run a brief MP trial phase to collect execution stats.
    trialConfig.maxNumWorksPerPlan = numWorksPerPlanEst;
    auto trialStatus = mp.runTrials(trialConfig);
    if (!trialStatus.isOK()) {
        return trialStatus;
    }
    auto stats = mp.getSpecificStats();
    if (stats->earlyExit) {
        // TODO: We choose MP in order to avoid planning time regressions.
        // Choosing MP due to full batch may miss good plans due to data skew or blocking plans.
        LOGV2_INFO(11306807,
                   "AutomaticCE chooses MP (1)",
                   "Reason"_attr = " because of EOF or full batch");
        return getBestMPPlan(mp);
    }

    // Compare the cost of MP vs CBR and decide which strategy to use to estimate all plans.
    tassert(11306806,
            "numWorksPerPlanEst != actual number of works",
            stats->totalWorks / numSolutions == numWorksPerPlanEst);

    // Estimate the cost of MP based on the "estimation" trial.
    auto estRes = mp.estimateAllPlans();
    tassert(11306805,
            "The MP estimation phase should not have filled a full batch",
            numResultsMP > estRes.bestPlanNumResults);

    LOGV2_INFO(11093900,
               "AutomaticCE begin: ",
               "numWorksPerPlanMP"_attr = numWorksPerPlanMP,
               "numResultsMP"_attr = numResultsMP,
               "numWorksPerPlanEst"_attr = numWorksPerPlanEst,
               "cbrCost"_attr = cbrCost.toString(),
               "estRes.totalCost"_attr = estRes.totalCost.toString(),
               "estRes.bestPlanProductivity"_attr = estRes.bestPlanProductivity,
               "estRes.bestPlanNumResults"_attr = estRes.bestPlanNumResults);

    // If productivity is worse than this ratio, it is guaranteed that MP can not fill a batch
    // within numWorksPerPlanMP works (default 10k). Choose CBR in this case.
    const double minProductivityForMP = static_cast<double>(numResultsMP) / numWorksPerPlanMP;
    if (estRes.bestPlanProductivity <= minProductivityForMP) {
        LOGV2_INFO(11306804,
                   "AutomaticCE chooses CBR (2)",
                   "Reason"_attr = "very low productivity",
                   "Condition"_attr = "estRes.bestPlanProductivity < minProductivityForMP",
                   "minProductivityForMPAdjusted"_attr = minProductivityForMP,
                   "minProductivityForMP"_attr =
                       static_cast<double>(numResultsMP) / numWorksPerPlanMP);
        return getBestCBRPlan(opCtx, query, plannerParams, yieldPolicy, collections);
    }

    // Remaining number of documents to fill a batch, that is, to end MP.
    size_t numDocsRem = numResultsMP - estRes.bestPlanNumResults;
    // Estimate the number of works needed to fill a batch.
    double remNumWorks = estRes.bestPlanProductivity > 0.0
        ? numDocsRem / estRes.bestPlanProductivity
        : numWorksPerPlanMP - numWorksPerPlanEst;
    // Total estimated MP cost until a full batch, including the work done during the estimation
    // trial (estRes.totalCost).
    auto totalCostPerEstWork = estRes.totalCost * (1.0 / numWorksPerPlanEst);
    // The cost that will be incurred by MP if run until it fills a batch, starting where the
    // previous run stopped.
    auto remMPCost = remNumWorks * totalCostPerEstWork;

    double minRequiredImprovementRatio =
        internalQueryMinRequiredImprovementRatioForCostBasedRankerChoice.load();
    double maxAchievableImprovementRatio = remMPCost.toDouble() / cbrCost.toDouble();
    LOGV2_INFO(11306803,
               "Comparing MP with CBR:",
               "remMPCost"_attr = remMPCost.toString(),
               "remNumWorks"_attr = remNumWorks,
               "maxAchievableImprovementRatio"_attr = maxAchievableImprovementRatio,
               "minRequiredImprovementRatio"_attr = minRequiredImprovementRatio);

    if (maxAchievableImprovementRatio < minRequiredImprovementRatio) {
        // CBR is not sufficiently better than remaining MP - use MP
        size_t remainingWorks = numWorksPerPlanMP - numWorksPerPlanEst;
        trialConfig.maxNumWorksPerPlan = remainingWorks;
        trialConfig.targetNumResults = numResultsMP;
        trialStatus = mp.runTrials(trialConfig);
        if (!trialStatus.isOK()) {
            return trialStatus;
        }
        stats = mp.getSpecificStats();
        LOGV2_INFO(11306802,
                   "AutomaticCE chooses MP (3)",
                   "Reason"_attr = "the required improvement is not achievable",
                   "Condition"_attr =
                       "maxAchievableImprovementRatio < minRequiredImprovementRatio");
        LOGV2_INFO(11306801,
                   "Result from finishing MP: ",
                   "remainingWorks"_attr = remainingWorks,
                   "actualNumWorksPerPlanRem"_attr = stats->totalWorks / numSolutions,
                   "exitStatus"_attr = stats->earlyExit);
        return getBestMPPlan(mp);
    }

    // CBR is substantially more efficient than the remaining MP, choose the best plan using CBR
    LOGV2_INFO(11306800, "AutomaticCE chooses CBR (4)", "Reason"_attr = "it is cheaper than MP");
    return getBestCBRPlan(opCtx, query, plannerParams, yieldPolicy, collections);
}

std::unique_ptr<WorkingSet> CostBasedPlanRankingStrategy::extractWorkingSet() {
    tassert(11306810, "WorkingSet is not initialized", _ws);
    auto result = std::move(_ws);
    _ws = nullptr;
    return result;
}

}  // namespace plan_ranking
}  // namespace mongo
