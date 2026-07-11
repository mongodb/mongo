// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_explainer_express.h"

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

PlanExplainer::PlanStatsDetails PlanExplainerExpress::getWinningPlanStats(
    ExplainOptions::Verbosity verbosity) const {
    BSONObjBuilder bob;

    bob.append("isCached", false);
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
    PlanSummaryStats stats;
    getSummaryStats(&stats);

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
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
}  // namespace mongo
