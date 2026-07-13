// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mongo {
/**
 * A PlanExplainer implementation for aggregation pipelines.
 */
class PlanExplainerPipeline final : public PlanExplainer {
public:
    PlanExplainerPipeline(const Pipeline* pipeline, const exec::agg::Pipeline* execPipeline)
        : _pipeline{pipeline}, _execPipeline(execPipeline) {}

    bool areThereRejectedPlansToExplain() const final {
        return false;
    }

    std::string getPlanSummary() const final;
    void getSummaryStats(PlanSummaryStats* statsOut) const final;
    PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const final;
    PlanStatsDetails getWinningPlanTrialStats() const final;
    std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const final;

    void incrementNReturned() {
        ++_nReturned;
    }

    bool isSbeExplainer() const final;

private:
    const Pipeline* const _pipeline;
    const exec::agg::Pipeline* const _execPipeline;
    size_t _nReturned{0};
};
}  // namespace mongo
