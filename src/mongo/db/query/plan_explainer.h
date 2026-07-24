// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/estimates_storage.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_ranking/plan_ranker_method.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/stage_builder/classic_stage_builder.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstdint>

namespace mongo {

/**
 * A struct to hold a QuerySolution along with its corresponding PlanStage tree.
 * It is intended to hold root nodes both for the solution and also its corresponding execution tree
 * for explain purposes.
 */
struct SolutionWithPlanStage {
    std::unique_ptr<QuerySolution> solution;
    std::unique_ptr<PlanStage> planStage;
    // True when 'planStage' is the multi-planner's trial tree for this (rejected) candidate, so
    // its counters are trial statistics. False for plans that never ran (e.g. CBR-rejected):
    // their stage tree is built after the fact, for explain display only. Both kinds are stored
    // in the same list (rejectedPlansWithStages), and this flag is the only thing that tells
    // them apart short of guessing from the counter values.
    //
    // This flag describes stored rejected plans only. The V3 explain answers the same question
    // - "did this plan run a trial?" - for the other kinds of plans from their own sources
    // (the winning plan from its exported trial snapshot, candidates still inside a
    // MultiPlanStage trivially so), and reports the per-plan answer, whatever its source, as
    // ExplainPlanEntry::hasTrialStats.
    bool ranTrial = false;
    // The candidate's final ranking score (trial score plus tie-breaking heuristics bonuses),
    // when the multi-planner's ranking decision covered this plan. Sorting by it descending
    // reproduces PlanRankingDecision::candidateOrder; QuerySolution::score remains the displayed
    // (bonus-free) trial score.
    boost::optional<double> adjustedScore;
};

struct PlanExplainerData {
    std::unique_ptr<WorkingSet> workingSetForRejectedPlansExplain;
    std::vector<SolutionWithPlanStage> rejectedPlansWithStages;
    std::unique_ptr<mongo::PlanStageStats> multiPlannerWinningPlanTrialStats;
    boost::optional<double> multiPlannerWinningPlanScore;
    stage_builder::PlanStageToQsnMap planStageQsnMap;
    cost_based_ranker::EstimateMap estimates;
    // Namespace-keyed map of sampling metadata emitted under queryPlanner.ceSamplingMetadata.
    // Populated on the explain path when CBR used a sampling estimator.
    StringMap<cost_based_ranker::SamplingMetadata> ceSamplingMetadata;
    bool fromPlanCache = false;
    // Hash of the join plan cache key. Populated on the explain path of a join eligible query.
    boost::optional<uint32_t> joinPlanCacheKeyHash;
};

inline PlanExplainerData& operator<<(PlanExplainerData& lhs, PlanExplainerData&& rhs) {
    lhs.rejectedPlansWithStages.insert(lhs.rejectedPlansWithStages.end(),
                                       std::make_move_iterator(rhs.rejectedPlansWithStages.begin()),
                                       std::make_move_iterator(rhs.rejectedPlansWithStages.end()));
    lhs.planStageQsnMap.insert(rhs.planStageQsnMap.begin(), rhs.planStageQsnMap.end());
    for (auto& [k, v] : rhs.estimates) {
        lhs.estimates.insert_or_assign(k, std::move(v));
    }
    for (auto& [ns, meta] : rhs.ceSamplingMetadata) {
        tassert(12433204,
                "ceSamplingMetadata already has an entry for namespace during merge",
                !lhs.ceSamplingMetadata.contains(ns));
        lhs.ceSamplingMetadata.emplace(ns, std::move(meta));
    }
    return lhs;
}

inline boost::optional<PlanExplainerData>& operator<<(boost::optional<PlanExplainerData>& lhs,
                                                      boost::optional<PlanExplainerData>&& rhs) {
    if (!rhs) {
        return lhs;
    }
    if (!lhs) {
        lhs = std::move(rhs);
        return lhs;
    }
    *lhs << std::move(*rhs);
    return lhs;
}

/**
 * The output shape produced by PlanExplainer::getPlanEntries().
 *
 * The format is an explicit parameter because it is deliberately not recoverable from the
 * enumerator's other input, the ExplainPolicy: the V3 execStats policy is by design the same
 * flag set as legacy allPlansExecution (that identity is what keeps the retained executionStats
 * section information-identical to the legacy one), so "is this V3?" cannot be inferred from the
 * policy. The caller, which has the verbosity, decides; passing the verbosity itself instead would
 * re-derive content decisions from a verbosity inside content code, the coupling the ExplainPolicy
 * seam exists to prevent.
 *
 * Nor does the reported version (getVersion()) determine the format: "3" is deliberately
 * reported for legacy-shaped output throughout the mixed-fidelity windows (planSummary /
 * plannerChoice until SERVER-131451, SBE/Express until SERVER-132033) - version reporting is
 * uniform by design while output fidelity varies per path, so the two must stay
 * decoupled.
 *
 * Not a transitional concept: legacy verbosities remain supported indefinitely and, since the
 * per-plan accessor consolidation, obtain their legacy-shaped entries through this same
 * enumerator as kLegacy. SERVER-132033 (engine parity) widens the enum's use to SBE/Express
 * while deleting the legacy per-plan virtuals; it does not retire the format dimension.
 */
enum class PlanStatsFormat {
    // The legacy explain node shape: structural fields and execution counters fused per node, the
    // winner's tree read from the live root (post-execution at the execution verbosities), plans
    // after the winner in enumeration order. Exactly the semantics of the legacy winning/rejected
    // accessors.
    kLegacy,
    // The V3 node shape: per-node statistics{costBased, multiPlan} grouping, the
    // winner's tree sourced from the pre-execution trial snapshot (trial statistics, never
    // final-execution statistics), plans after the winner ordered by the deciding ranker's metric.
    kV3,
};

/**
 * One entry per-plan returned by PlanExplainer::getPlanEntries(): a single candidate plan's
 * serialized stats tree plus its optional summary. Unlike the winner/rejected- shaped accessors,
 * plan entries form one sequence, which is the shape the V3 explain "plans[]" output is built
 * over. The winning (or sole) plan is always the first entry - its position is the contract;
 * there is no per-entry winner flag.
 */
struct ExplainPlanEntry {
    // The plan's serialized stats tree, produced by the same per-plan formatting core as the legacy
    // winning/rejected accessors.
    BSONObj planStatsTree;
    // Present when the explain policy requests execution stats; mirrors PlanStatsDetails::second.
    boost::optional<PlanSummaryStats> summary;
    // True when this plan ran a multi-planning trial: its stats tree carries trial counters and
    // 'summary' holds the plan-level trial totals. This is the output-side contract the V3
    // queryPlanner assembler consumes to emit the plan-level "multiPlanStats" (it cannot be
    // inferred from 'summary', which is also collected for never-ran plans). The enumeration
    // derives it from one of three provenance sources per plan: the winner's trial snapshot,
    // membership in an in-tree MultiPlanStage, or SolutionWithPlanStage::ranTrial for stored
    // rejected plans.
    bool hasTrialStats = false;
    // Whether this plan is the one in the plan cache: the same
    // 'cachedPlanHash == QuerySolution::hash()' comparison the legacy format computes, but where
    // legacy emits the resulting "isCached" inside the plan's stats tree (its root node), the V3
    // shape hoists it to the plan-object level - statsToBsonV3() does not emit it - so the
    // value must cross this API for the assembler to place it.
    bool isCached = false;
    // The plan's QuerySolution hash, when known. Emitted as "solutionHashUnstable" at plan level
    // in the V3 output when the forced-plan-by-hash knob is enabled.
    boost::optional<size_t> solutionHash;
    // TODO SERVER-131545: attach additional per-plan data (e.g. planningResult,
    // terminationReason, score) via a per-candidate record.
};


/**
 * This interface defines an API to provide information on the execution plans generated by the
 * query planner for a user query in various formats.
 */
class PlanExplainer {
public:
    /**
     * A version of the explain format:
     * - "1" is used for the classic engine
     * - "2" for SBE stagebuilders
     * - "3" for the V3 explain format, requested via one of the V3 verbosity modes.
     *   Unlike "1"/"2", "3" is not determined by the execution engine.
     */
    using ExplainVersion = std::string;

    /**
     * This pair holds a serialized BSON document that details the plan selected by the query
     * planner, and optional summary stats for an execution tree if the verbosity level for the
     * generated stats is 'executionStats' or higher. The format of these stats are opaque to the
     * caller, and different implementations may choose to provide different stats.
     */
    using PlanStatsDetails = std::pair<BSONObj, boost::optional<PlanSummaryStats>>;

    PlanExplainer() {}
    PlanExplainer(const QuerySolution* solution)
        : _solution(solution),
          _enumeratorExplainInfo{_solution ? _solution->_enumeratorExplainInfo
                                           : PlanEnumeratorExplainInfo{}} {}
    PlanExplainer(const PlanEnumeratorExplainInfo& info) : _enumeratorExplainInfo{info} {}

    virtual ~PlanExplainer() = default;

    /**
     * Returns 'true' if this explainer describes an SBE plan (explain version "2"), or 'false' for
     * the classic engine (explain version "1").
     */
    virtual bool isSbeExplainer() const = 0;

    /**
     * Returns the explain version to report for the given 'verbosity'. When any of the V3 verbosity
     * modes is requested this is "3"; otherwise it is the engine-determined version ("2" for SBE,
     * "1" for the classic engine, as reported by isSbeExplainer()). Centralizing the decision here
     * ensures the V3 rule cannot be bypassed by, or diverge across, explainer implementations.
     */
    const ExplainVersion& getVersion(ExplainOptions::Verbosity verbosity) const {
        static const ExplainVersion kV1{"1"};
        static const ExplainVersion kV2{"2"};
        static const ExplainVersion kV3{"3"};

        if (ExplainOptions::isV3Verbosity(verbosity)) {
            return kV3;
        }
        return isSbeExplainer() ? kV2 : kV1;
    }

    /**
     * Returns 'true' if this PlanExplainer can provide information on the winning plan and rejected
     * candidate plans, meaning that the QueryPlanner generated multiple candidate plans and the
     * winning plan was chosen by the multi-planner.
     */
    virtual bool areThereRejectedPlansToExplain() const = 0;

    /**
     * Returns a short string, suitable for the logs, which summarizes the execution plan.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] virtual std::string getPlanSummary() const = 0;

    /**
     * Fills out 'statsOut' with summary stats collected during the execution of the underlying
     * plan. This is a lightweight alternative which is useful when operations want to request a
     * summary of the available debug information without generating complete explain output.
     *
     * The summary stats are consumed by debug mechanisms such as the profiler and the slow query
     * log.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] virtual void getSummaryStats(
        PlanSummaryStats* statsOut) const = 0;

    /**
     * Fills out 'statsOut' for the secondary collection 'secondaryColl'. Subclasses may
     * override this function if the summary stats for secondary collections need to be reported
     * separately.
     */
    virtual void getSecondarySummaryStats(const NamespaceString& secondaryColl,
                                          PlanSummaryStats* statsOut) const {}

    /**
     * Returns statistics that detail the winning plan selected by the multi-planner, or, if no
     * multi-planning has been performed, for the single plan selected by the QueryPlanner.
     *
     * The 'verbosity' level parameter determines the amount of information to be returned.
     *
     * TODO SERVER-132033: the classic implementation is a thin wrapper over
     * getPlanEntries(); once SBE/Express route through getPlanEntries() too, remove this virtual
     * (and getRejectedPlansStats()) from the interface, completing the per-plan accessor
     * consolidation.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] virtual PlanStatsDetails getWinningPlanStats(
        ExplainOptions::Verbosity verbosity) const = 0;

    virtual PlanStatsDetails getWinningPlanStatsQueryPlanner(bool /*printBytecode*/) const {
        return getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    }

    /**
     * Returns statistics for the trial period of the winning plan selected by the multi-planner.
     */
    virtual PlanStatsDetails getWinningPlanTrialStats() const = 0;

    /**
     * Returns statistics that detail candidate plans rejected by the multi-planner. If no
     * multi-planning has been performed, an empty vector is returned.
     *
     * The 'verbosity' level parameter determines the amount of information to be returned.
     *
     * TODO SERVER-132033: remove together with getWinningPlanStats() once
     * SBE/Express route through getPlanEntries(); see the note there.
     */
    virtual std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const = 0;

    /**
     * Returns a uniform per-plan view of the candidate plans: one ExplainPlanEntry per plan, the
     * winning (or sole) plan first, followed by the remaining candidates. The content depends on
     * 'policy' and the node shape, winner-tree source, and candidate ordering are selected by
     * 'format' (see PlanStatsFormat).
     *
     * 'decidingPlanRanker' names the ranker that chose the winning plan (the value recorded in
     * OpDebug::planRankerMethod by the plan ranking strategies) and determines the kV3 ordering of
     * the plans after the winner: the deciding ranker's metric is the primary sort key (root cost
     * estimate ascending when the cost-based ranker decided; trial score descending, then cost-only
     * plans by cost ascending, when the multi-planner decided), with enumeration order breaking
     * ties. It must be threaded from that single write point, never inferred from which statistics
     * are present. It is ignored in kLegacy format (enumeration order, exactly the legacy
     * accessors' behavior); pass PlanRankerMethod::kNone when unknown (single plan, cached plan).
     *
     * This is the accessor the V3 explain output builds its "plans[]" array
     * over. The default implementation returns no entries. Explainers whose per-plan V3
     * parity is deferred (SBE, Express; SERVER-132033) and explainers without
     * candidate plans (the pipeline explainer) inherit it. PlanExplainerImpl provides the real
     * implementation.
     */
    virtual std::vector<ExplainPlanEntry> getPlanEntries(
        const ExplainPolicy& policy,
        PlanStatsFormat format,
        PlanRankerMethod decidingPlanRanker) const {
        return {};
    }

    /**
     * Returns an object containing what query knobs the planner hit during plan enumeration. This
     * is specific to classic.
     */
    PlanEnumeratorExplainInfo getEnumeratorInfo() const {
        return _enumeratorExplainInfo;
    }
    void updateEnumeratorExplainInfo(const PlanEnumeratorExplainInfo& other) {
        _enumeratorExplainInfo.merge(other);
    }

    void setQuerySolution(const QuerySolution* qs) {
        _solution = qs;
    }

    /**
     * Returns the per-collection sampling metadata to be emitted under
     * queryPlanner.ceSamplingMetadata in explain output. Returns boost::none if no sampling
     * metadata is available (e.g., CBR was not used, or this is not a classic-engine plan).
     */
    virtual boost::optional<StringMap<cost_based_ranker::SamplingMetadata>> getCeSamplingMetadata()
        const {
        return boost::none;
    }

    /**
     * Returns a hash of the join plan cache key. Returns boost::none if the query is not eligible
     * for join optimization.
     */
    virtual boost::optional<uint32_t> getJoinPlanCacheKeyHash() const {
        return boost::none;
    }

protected:
    const QuerySolution* _solution{nullptr};
    PlanEnumeratorExplainInfo _enumeratorExplainInfo;
};
}  // namespace mongo
