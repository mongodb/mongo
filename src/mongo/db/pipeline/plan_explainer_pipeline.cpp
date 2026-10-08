// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/plan_explainer_pipeline.h"

#include "mongo/db/exec/agg/cursor_stage.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/util/assert_util.h"

#include <algorithm>


namespace mongo {
bool PlanExplainerPipeline::isSbeExplainer() const {
    if (!_pipeline->empty()) {
        if (auto docSourceCursor =
                dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().front().get())) {
            return docSourceCursor->isSbeExplainer();
        }
    }
    return false;
}

std::string PlanExplainerPipeline::getPlanSummary() const {
    if (auto cursorStage =
            dynamic_cast<exec::agg::CursorStage*>(_execPipeline->getStages().front().get())) {
        return cursorStage->getPlanSummaryStr();
    }

    return "";
}

void PlanExplainerPipeline::getSummaryStats(PlanSummaryStats* statsOut) const {
    tassert(9378603, "Encountered unexpected nullptr for PlanSummaryStats", statsOut);

    auto stage_it = _execPipeline->getStages().cbegin();
    if (auto cursorStage = dynamic_cast<exec::agg::CursorStage*>(stage_it->get())) {
        *statsOut = cursorStage->getPlanSummaryStats();
        ++stage_it;
    };

    PlanSummaryStatsVisitor visitor(*statsOut);
    std::for_each(stage_it, _execPipeline->getStages().cend(), [&](const auto& stage) {
        statsOut->usedDisk = statsOut->usedDisk || stage->usedDisk();
        if (auto specificStats = stage->getSpecificStats()) {
            specificStats->acceptVisitor(&visitor);
        }
    });

    statsOut->nReturned = _nReturned;
}

PlanExplainer::PlanStatsDetails PlanExplainerPipeline::getWinningPlanTrialStats() const {
    // We are not supposed to call this method on a pipeline explainer.
    MONGO_UNREACHABLE;
}

std::vector<ExplainPlanEntry> PlanExplainerPipeline::getPlanEntries(
    const ExplainPolicy& policy,
    PlanStatsFormat format,
    PlanSelectionStrategy decidingPlanRanker) const {
    // TODO SERVER-49808: Report execution stats for the pipeline.
    if (_pipeline->empty()) {
        return {};
    }

    if (auto docSourceCursor =
            dynamic_cast<DocumentSourceCursor*>(_pipeline->getSources().cbegin()->get())) {
        if (auto explainer = docSourceCursor->getPlanExplainer()) {
            auto entries = explainer->getPlanEntries(policy, format, decidingPlanRanker);
            // Report only the cursor's winning plan. The cursor stage may itself have been
            // multi-planned, but this explainer declares it has no rejected plans
            // (areThereRejectedPlansToExplain() is always false here).
            if (entries.size() > 1) {
                entries.resize(1);
            }
            return entries;
        }
    };
    return {};
}
}  // namespace mongo
