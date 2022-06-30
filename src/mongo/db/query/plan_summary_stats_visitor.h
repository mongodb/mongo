/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/plan_summary_stats.h"

namespace mongo {
/**
 * Visitor for accumulating execution stats.
 */
class PlanSummaryStatsVisitor : public PlanStatsVisitorBase<true> {
public:
    // To avoid overloaded-virtual warnings.
    using PlanStatsConstVisitor::visit;

    explicit PlanSummaryStatsVisitor(PlanSummaryStats& summary) : _summary(summary) {}

    void visit(tree_walker::MaybeConstPtr<true, sbe::ScanStats> stats) override final {
        _summary.totalDocsExamined += stats->numReads;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::ColumnScanStats> stats) override final {
        _summary.totalDocsExamined += stats->numRowStoreFetches;
        for (auto const& stat : stats->cursorStats)
            _summary.totalKeysExamined += stat.numNexts + stat.numSeeks;
        for (auto const& stat : stats->parentCursorStats)
            _summary.totalKeysExamined += stat.numNexts + stat.numSeeks;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::IndexScanStats> stats) override final {
        _summary.totalKeysExamined += stats->keysExamined;
    }
    void visit(tree_walker::MaybeConstPtr<true, SortStats> stats) override final {
        _summary.hasSortStage = true;
        _summary.usedDisk = _summary.usedDisk || stats->spills > 0;
    }
    void visit(tree_walker::MaybeConstPtr<true, GroupStats> stats) override final {
        _summary.usedDisk = _summary.usedDisk || stats->spills > 0;
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceCursorStats> stats) override final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceLookupStats> stats) override final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, UnionWithStats> stats) override final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceFacetStats> stats) override final {
        accumulate(stats->planSummaryStats);
    }

private:
    /**
     * Helper method to accumulate the plan summary stats from the input source.
     */
    void accumulate(const PlanSummaryStats& statsIn) {
        // Attributes replanReason and fromMultiPlanner have been intentionally skipped as they
        // always describe the left-hand side (or "local") collection.
        // Consider $lookup case. $lookup runtime plan selection may happen against the foreign
        // collection an arbitrary number of times. A single value of 'replanReason' and
        // 'fromMultiPlanner' can't really report correctly on the behavior of arbitrarily many
        // occurrences of runtime planning for a single query.

        _summary.nReturned += statsIn.nReturned;
        _summary.totalKeysExamined += statsIn.totalKeysExamined;
        _summary.totalDocsExamined += statsIn.totalDocsExamined;
        _summary.collectionScans += statsIn.collectionScans;
        _summary.collectionScansNonTailable += statsIn.collectionScansNonTailable;
        _summary.hasSortStage |= statsIn.hasSortStage;
        _summary.usedDisk |= statsIn.usedDisk;
        _summary.planFailed |= statsIn.planFailed;
        _summary.indexesUsed.insert(statsIn.indexesUsed.begin(), statsIn.indexesUsed.end());
    }

    PlanSummaryStats& _summary;
};
}  // namespace mongo
