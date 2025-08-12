/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/tree_walker.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
struct CommonStats {
    CommonStats() = delete;

    CommonStats(StringData stageType, PlanNodeId nodeId) : stageType{stageType}, nodeId{nodeId} {}

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    const StringData stageType;

    // An identifier for the node, or zero if the idenfier was not provided. Useful for displaying
    // debug output such as explain.
    //
    // These identifiers are not necessarily unique. For example, they can be used by code
    // constructing the SBE plan to construct groups of nodes with the same id, e.g. if a group of
    // PlanStages corresponds to an MQL operation specified by the user.
    const PlanNodeId nodeId;

    // Time elapsed while working inside this stage.
    //
    // The field must be populated when running explain or when running with the profiler on. It
    // must also be populated when multi planning, in order to gather stats stored in the plan
    // cache.  This struct includes the execution time and its precision/unit.
    QueryExecTime executionTime;

    size_t advances{0};
    size_t opens{0};
    size_t closes{0};
    size_t yields{0};
    size_t unyields{0};
    bool isEOF{false};
};

using PlanStageStats = BasePlanStageStats<CommonStats>;

struct ScanStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<ScanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t numReads{0};
};

struct IndexScanStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<IndexScanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const override {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t seeks{0};
    size_t keysExamined{0};
    size_t keyCheckSkipped{0};
    size_t numReads{0};
};

struct FilterStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<FilterStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t numTested{0};
};

struct LimitSkipStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<LimitSkipStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    boost::optional<long long> limit;
    boost::optional<long long> skip;
};

struct UniqueStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<UniqueStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t dupsTested = 0;
    size_t dupsDropped = 0;
};

struct BranchStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<BranchStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t numTested{0};
    size_t thenBranchOpens{0};
    size_t thenBranchCloses{0};
    size_t elseBranchOpens{0};
    size_t elseBranchCloses{0};
};

struct CheckBoundsStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<CheckBoundsStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t seeks{0};
};

struct LoopJoinStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<LoopJoinStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t innerOpens{0};
    size_t innerCloses{0};
};

struct TraverseStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<TraverseStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t innerOpens{0};
    size_t innerCloses{0};
};

struct HashAggStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const override {
        return std::make_unique<HashAggStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const override {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const override {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) override {
        visitor->visit(this);
    }

    bool usedDisk{false};
    SpillingStats spillingStats;

    // The maximum amount of memory that was used.
    uint64_t peakTrackedMemBytes = 0u;
};

struct BlockHashAggStats : public HashAggStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<BlockHashAggStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    /*
     * We use block accumulators (which do bulk operations) when the groupby keys for BlockHashAgg
     * don't have many unique values. If there are lots of unique values (ie highly partitioned), we
     * prefer an element-wise approach, applying the accumulator to each data point one at a time.
     *
     * These metrics keep track of how many times each type of accumulator is used.
     * `blockAccumulations` and `elementWiseAccumulations` indicate how many times we chose the
     * corresponding accumulation method per block. `blockAccumulatorTotalCalls` indicates how many
     * times the block accumulators were invoked, which may be more than once per block depending
     * on how many partitions there are.
     */
    long long blockAccumulations{0};
    long long blockAccumulatorTotalCalls{0};
    long long elementWiseAccumulations{0};
};

struct HashLookupStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<HashLookupStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    SpillingStats getTotalSpillingStats() const {
        SpillingStats stats = spillingHtStats;
        stats.accumulate(spillingBuffStats);
        return stats;
    }

    bool usedDisk{false};
    SpillingStats spillingHtStats;
    SpillingStats spillingBuffStats;

    // The maximum amount of memory that was used.
    uint64_t peakTrackedMemBytes = 0u;
};

struct WindowStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<WindowStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Whether the window buffer was spilled.
    bool usedDisk{false};
    SpillingStats spillingStats;
};

/**
 * Visitor for calculating the number of storage reads during plan execution.
 */
struct PlanStatsNumReadsVisitor : PlanStatsVisitorBase<true> {
    // To avoid overloaded-virtual warnings.
    using PlanStatsConstVisitor::visit;

    void visit(tree_walker::MaybeConstPtr<true, sbe::ScanStats> stats) final {
        numReads += stats->numReads;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::IndexScanStats> stats) final {
        numReads += stats->numReads;
    }

    size_t numReads = 0;
};

struct SearchStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<SearchStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    long long msWaitingForMongot{0};
    long long batchNum{0};
};

struct TsBucketToBlockStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<TsBucketToBlockStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const final {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t numStorageBlocksDecompressed = 0;
    size_t numStorageBlocks = 0;
    size_t numCellBlocksProduced = 0;
};

/**
 * Calculates the total number of physical reads in the given plan stats tree. If a stage can do
 * a physical read (e.g. COLLSCAN or IXSCAN), then its 'numReads' stats is added to the total.
 */
size_t calculateNumberOfReads(const PlanStageStats* root);

/**
 * Accumulates the summary of all execution statistics by walking over the specific-stats of stages.
 */
PlanSummaryStats collectExecutionStatsSummary(const PlanStageStats& root);
}  // namespace mongo::sbe
