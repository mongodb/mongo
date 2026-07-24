// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
/**
 * A PlanExplainer implementation for classic execution plans.
 *
 * For classic execution plans all information required to generate explain output in various
 * formats is stored in the execution tree itself, so having access to the root stage of the
 * execution tree this PlanExplainer should obtain all plan details and execution stats.
 */
class PlanExplainerImpl final : public PlanExplainer {
public:
    PlanExplainerImpl(PlanStage* root, const PlanEnumeratorExplainInfo& explainInfo)
        : PlanExplainer{explainInfo}, _root{root} {}

    /**
     * 'isExplain' states whether the executor serves an explain command. For explains whose
     * MultiPlanStage is still in the execution tree (pure multiplanning), the constructor - which
     * runs after plan selection and before the explained query executes - snapshots the trial
     * statistics into _explainData, into the same slots the ranking strategies populate on the
     * other paths; normal queries skip that stats-tree copy.
     *
     * TODO SERVER-132012: replace the flag with an explain-specialized subclass chosen at the
     * factory.
     */
    PlanExplainerImpl(PlanStage* root,
                      boost::optional<size_t> cachedPlanHash,
                      boost::optional<std::string> replanReason,
                      boost::optional<PlanExplainerData> maybeExplainData,
                      bool isExplain);

    bool isSbeExplainer() const final {
        return false;
    }
    bool areThereRejectedPlansToExplain() const final;
    std::string getPlanSummary() const final;
    void getSummaryStats(PlanSummaryStats* statsOut) const final;
    PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const final;
    PlanStatsDetails getWinningPlanTrialStats() const final;
    std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const final;
    std::vector<ExplainPlanEntry> getPlanEntries(const ExplainPolicy& policy,
                                                 PlanStatsFormat format,
                                                 PlanRankerMethod decidingPlanRanker) const final;
    std::vector<PlanStatsDetails> getCachedPlanStats(const plan_cache_debug_info::DebugInfo&,
                                                     ExplainOptions::Verbosity) const;

    boost::optional<StringMap<cost_based_ranker::SamplingMetadata>> getCeSamplingMetadata()
        const override {
        if (_explainData.ceSamplingMetadata.empty()) {
            return boost::none;
        }
        return _explainData.ceSamplingMetadata;
    }

private:
    /**
     * The shared per-plan formatting core: serializes one plan's stats tree (depending on 'policy')
     * and collects its summary stats. 'solutionHash', when present, is the plan's QuerySolution
     * hash. It is used to decide the "isCached" flag and to emit "solutionHashUnstable". Reused by
     * the winning-, rejected-, and per-plan-entry accessors.
     */
    PlanStatsDetails _formatPlanStats(const PlanStageStats* stats,
                                      const ExplainPolicy& policy,
                                      boost::optional<size_t> planIdx,
                                      boost::optional<double> score,
                                      boost::optional<size_t> solutionHash) const;

    std::vector<ExplainPlanEntry> _getPlanEntriesLegacy(const ExplainPolicy& policy) const;
    std::vector<ExplainPlanEntry> _getPlanEntriesV3(const ExplainPolicy& policy,
                                                    PlanRankerMethod decidingPlanRanker) const;

    PlanStage* const _root;
    boost::optional<size_t> _cachedPlanHash;
    boost::optional<std::string> _replanReason;
    PlanExplainerData _explainData;
};

/**
 * Converts the stats tree 'stats' into a BSON object in the V3 explain node shape:
 * structural fields (stage, planNodeId, keyPattern, indexBounds, filter, ...) stay flat on the
 * node, children always nest as the "inputStages" array (no single-child "inputStage" object,
 * unlike the legacy shape), and the statistics are grouped per node under a sparse "statistics"
 * subobject -
 * "costBased" holds the cost-based ranker's estimates (present iff the estimate map has an entry
 * for the node's QSN) and "multiPlan" holds the multi-planning trial counters (present iff
 * 'isTrialTree' and the policy requests per-candidate statistics). 'isTrialTree' states whether
 * this stats tree carries multi-planning trial counters; it applies to the whole tree. If there is
 * a MultiPlanStage node, it is skipped, following the subplan at 'planIdx' (like the legacy
 * serializer). 'topLevelBob' tracks the size of the overall explain object for the size guard;
 * it is only read.
 */
void statsToBsonV3(const stage_builder::PlanStageToQsnMap& planStageQsnMap,
                   const cost_based_ranker::EstimateMap& estimates,
                   const PlanStageStats& stats,
                   const ExplainPolicy& explainPolicy,
                   bool isTrialTree,
                   boost::optional<size_t> planIdx,
                   BSONObjBuilder* bob,
                   const BSONObjBuilder* topLevelBob);

/**
 * Retrieves the first stage of a given type from the plan tree, or nullptr if no such stage is
 * found.
 */
PlanStage* getStageByType(PlanStage* root, StageType type);

/**
 * Returns filtered plan stats from the debugInfo object for different verbosity levels.
 */
std::vector<PlanExplainer::PlanStatsDetails> getCachedPlanStats(
    const plan_cache_debug_info::DebugInfo& debugInfo, ExplainOptions::Verbosity verbosity);


/**
 * Collects and aggregates execution stats summary (totalKeysExamined and totalDocsExamined) by
 * traversing the stats tree. Skips the top-level MultiPlanStage when it is at the top of the plan,
 * and extracts stats from its child according to 'planIdx'.
 */
PlanSummaryStats collectExecutionStatsSummary(const PlanStageStats* stats,
                                              boost::optional<size_t> planIdx);

/**
 * Adds the path-level multikey information to the explain output in a field called "multiKeyPaths".
 * The value associated with the "multiKeyPaths" field is an object with keys equal to those in the
 * index key pattern and values equal to an array of strings corresponding to paths that cause the
 * index to be multikey.
 *
 * For example, with the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause the
 * index to be multikey, we'd have {'multiKeyPaths': {'a.b': ['a', 'a.b'], 'a.c': ['a']}}.
 *
 * This function should only be called if the associated index supports path-level multikey
 * tracking.
 */
void appendMultikeyPaths(const BSONObj& keyPattern,
                         const MultikeyPaths& multikeyPaths,
                         BSONObjBuilder* bob);
}  // namespace mongo
