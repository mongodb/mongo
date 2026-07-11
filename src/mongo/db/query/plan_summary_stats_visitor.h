// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_stats/data_bearing_node_metrics.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * Visitor for accumulating execution stats.
 */
class PlanSummaryStatsVisitor : public PlanStatsVisitorBase<true> {
public:
    // To avoid overloaded-virtual warnings.
    using PlanStatsConstVisitor::visit;

    explicit PlanSummaryStatsVisitor(PlanSummaryStats& summary) : _summary(summary) {}

    void visit(tree_walker::MaybeConstPtr<true, sbe::ScanStats> stats) final {
        _summary.totalDocsExamined += stats->numReads;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::FetchStats> stats) final {
        _summary.totalDocsExamined += stats->numReads;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::IndexScanStats> stats) final {
        _summary.totalKeysExamined += stats->keysExamined;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::HashAggStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::GROUP].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::WindowStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::SET_WINDOW_FIELDS]
                .accumulate(stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, NearStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::GEO_NEAR].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, SortStats> stats) final {
        _summary.hasSortStage = true;
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::SORT].accumulate(
                stats->spillingStats);
        }
        _summary.sortTotalDataSizeBytes += stats->totalDataSizeBytes;
        _summary.keysSorted += stats->keysSorted;
    }
    void visit(tree_walker::MaybeConstPtr<true, GroupStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::GROUP].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, TextOrStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::TEXT_OR].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::HashLookupStats> stats) final {
        SpillingStats hashLookupSpillingStatsTotal = stats->getTotalSpillingStats();
        if (hashLookupSpillingStatsTotal.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::HASH_LOOKUP].accumulate(
                hashLookupSpillingStatsTotal);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::HashJoinStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::HASH_JOIN].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceCursorStats> stats) final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceMergeCursorsStats> stats) final {
        accumulate(stats->planSummaryStats);
        accumulate(stats->dataBearingNodeMetrics);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceLookupStats> stats) final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, UnionWithStats> stats) final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceFacetStats> stats) final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceIdLookupStats> stats) final {
        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceGraphLookupStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::GRAPH_LOOKUP]
                .accumulate(stats->spillingStats);
        }

        accumulate(stats->planSummaryStats);
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceBucketAutoStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::BUCKET_AUTO].accumulate(
                stats->spillingStats);
        }
    }
    void visit(tree_walker::MaybeConstPtr<true, DocumentSourceSetWindowFieldsStats> stats) final {
        if (stats->spillingStats.getSpills() > 0) {
            _summary.usedDisk = true;
            _summary.spillingStatsPerStage[PlanSummaryStats::SpillingStage::SET_WINDOW_FIELDS]
                .accumulate(stats->spillingStats);
        }
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
        _summary.sortTotalDataSizeBytes += statsIn.sortTotalDataSizeBytes;
        _summary.keysSorted += statsIn.keysSorted;
        _summary.planFailed |= statsIn.planFailed;
        _summary.indexesUsed.insert(statsIn.indexesUsed.begin(), statsIn.indexesUsed.end());
        for (const auto& [stageName, spillingStats] : statsIn.spillingStatsPerStage) {
            _summary.spillingStatsPerStage[stageName].accumulate(spillingStats);
        }
    }

    /**
     * Helper method to accumulate the plan summary stats from remote data-bearing node metrics.
     */
    void accumulate(const query_stats::DataBearingNodeMetrics& metricsIn) {
        _summary.totalKeysExamined += metricsIn.keysExamined;
        _summary.totalDocsExamined += metricsIn.docsExamined;
        _summary.hasSortStage |= metricsIn.hasSortStage;
        _summary.usedDisk |= metricsIn.usedDisk;
    }

    void accumulate(const boost::optional<query_stats::DataBearingNodeMetrics>& metricsIn) {
        if (metricsIn) {
            accumulate(*metricsIn);
        }
    }

    PlanSummaryStats& _summary;
};
}  // namespace mongo
