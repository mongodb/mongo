// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/plan_stats.h"

#include "mongo/db/exec/plan_stats_walker.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/query/tree_walker.h"

namespace mongo::sbe {
size_t calculateNumberOfReads(const PlanStageStats* root) {
    auto visitor = PlanStatsNumReadsVisitor{};
    auto walker = PlanStageStatsWalker<true, CommonStats>(nullptr, nullptr, &visitor);
    tree_walker::walk<true, PlanStageStats>(root, &walker);
    return visitor.numReads;
}

PlanSummaryStats collectExecutionStatsSummary(const PlanStageStats& root) {
    PlanSummaryStats summary;
    summary.nReturned = root.common.advances;

    summary.executionTime = root.common.executionTime;

    auto visitor = PlanSummaryStatsVisitor(summary);
    auto walker = PlanStageStatsWalker<true, CommonStats>(nullptr, nullptr, &visitor);
    tree_walker::walk<true, PlanStageStats>(&root, &walker);
    return summary;
}
}  // namespace mongo::sbe
