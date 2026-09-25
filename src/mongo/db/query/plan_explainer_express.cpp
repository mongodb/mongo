// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_explainer_express.h"

#include "mongo/db/query/explain_policy.h"

namespace mongo {
using namespace std::literals::string_view_literals;
std::string PlanExplainerExpress::getPlanSummary() const {
    StackStringBuilder ssb;

    ssb << _iteratorStats->stageName();

    if (!_iteratorStats->indexKeyPattern().isEmpty()) {
        ssb << " " << KeyPattern{_iteratorStats->indexKeyPattern()};
    }

    if (!_writeOperationStats->stageName().empty()) {
        ssb << "," << _writeOperationStats->stageName();
    }
    return ssb.str();
}

void PlanExplainerExpress::getSummaryStats(PlanSummaryStats* statsOut) const {
    statsOut->nReturned = _planStats->numResults();
    _iteratorStats->populateSummaryStats(statsOut);
    if (_commonStats) {
        statsOut->executionTime = _commonStats->executionTime;
    }
}

void PlanExplainerExpress::_appendPlanStructure(BSONObjBuilder& bob) const {
    if (_writeOperationStats->stageName().empty()) {
        bob.append("stage"sv, _iteratorStats->stageName());
    } else {
        bob.append("stage"sv, _writeOperationStats->stageName());
    }
    _iteratorStats->appendDataAccessStats(bob);

    if (!_projection.isEmpty()) {
        bob.append("projection"sv, _projection);
        bob.append("projectionCovered"sv, _iteratorStats->projectionCovered());
    }
    // Express queries are ineligible for join optimization so if the knob is enabled, indicate
    // in the explain output that the join optimization was not applied.
    if (internalEnableJoinOptimization.load()) {
        bob.append("usedJoinOptimization", false);
    }
}

PlanExplainer::PlanStatsDetails PlanExplainerExpress::_formatPlanStats(
    const ExplainPolicy& explainPolicy, PlanStatsFormat format) const {
    const bool isV3 = format == PlanStatsFormat::kV3;
    BSONObjBuilder bob;

    // The V3 shape hoists 'isCached' out of the stats tree to the plan-object level whereas the
    // legacy shape carries it on the root node.
    if (!isV3) {
        bob.append("isCached", false);
    }

    _appendPlanStructure(bob);

    PlanSummaryStats stats;
    getSummaryStats(&stats);

    // The V3 node shape never fuses execution counters into the node: they are grouped under a
    // per-node "statistics" subobject.
    if (!isV3 && explainPolicy.hasExecStats()) {
        bob.appendNumber("nReturned", static_cast<long long>(stats.nReturned));
        if (_commonStats) {
            appendExecutionTimeFields(bob, _commonStats->executionTime);
        }
        bob.appendNumber("keysExamined", static_cast<long long>(stats.totalKeysExamined));
        bob.appendNumber("docsExamined", static_cast<long long>(stats.totalDocsExamined));
        if (!_writeOperationStats->stageName().empty()) {
            _writeOperationStats->populateExecStats(bob);
        }
    }

    return {bob.obj(), stats};
}

PlanExplainer::PlanStatsDetails PlanExplainerExpress::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    return _formatPlanStats(explainPolicyFor(verbosity), PlanStatsFormat::kLegacy);
}

std::vector<ExplainPlanEntry> PlanExplainerExpress::getPlanEntries(const ExplainPolicy& policy,
                                                                   PlanStatsFormat format,
                                                                   PlanSelectionStrategy) const {
    auto [planStatsTree, summary] = _formatPlanStats(policy, format);

    ExplainPlanEntry entry;
    entry.planStatsTree = std::move(planStatsTree);
    entry.hasTrialStats = false;
    entry.isCached = false;
    if (policy.hasExecStats() || policy.hasAllPlansStats()) {
        entry.summary = std::move(summary);
    }

    return std::vector<ExplainPlanEntry>{std::move(entry)};
}
}  // namespace mongo
