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

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/storage/column_store.h"

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

    // Time elapsed while working inside this stage. When this field is set to boost::none,
    // timing info will not be collected during query execution.
    //
    // The field must be populated when running explain or when running with the profiler on. It
    // must also be populated when multi planning, in order to gather stats stored in the plan
    // cache.
    boost::optional<long long> executionTimeMillis;

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

struct ColumnScanStats final : public SpecificStats {

    // Struct to hold relevant stats for ColumnCursor and ParentPathCursor
    // Note: `includeInOutput` field is only relevant for ColumnCursor
    struct CursorStats final {
        PathValue path;
        bool includeInOutput;
        size_t numNexts;
        size_t numSeeks;

        CursorStats(PathValue p, bool include)
            : path(p), includeInOutput(include), numNexts(0), numSeeks(0) {}
    };

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<ColumnScanStats>(*this);
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

    size_t numRowStoreFetches{0};

    // Lists holding all of the stats of current struct's cursors. These stats objects are owned
    // here, and referred to by the cursor execution objects.
    std::list<ColumnScanStats::CursorStats> cursorStats;
    std::list<ColumnScanStats::CursorStats> parentCursorStats;
};

struct IndexScanStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<IndexScanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
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
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<HashAggStats>(*this);
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

    bool usedDisk{false};
    long long spilledRecords{0};
    long long lastSpilledRecordSize{0};
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

    long long getSpilledRecords() const {
        return spilledHtRecords + spilledBuffRecords;
    }

    long long getSpilledBytesApprox() const {
        return spilledHtBytesOverAllRecords + spilledBuffBytesOverAllRecords;
    }

    bool usedDisk{false};
    long long spilledHtRecords{0};
    long long spilledHtBytesOverAllRecords{0};
    long long spilledBuffRecords{0};
    long long spilledBuffBytesOverAllRecords{0};
};

/**
 * Visitor for calculating the number of storage reads during plan execution.
 */
struct PlanStatsNumReadsVisitor : PlanStatsVisitorBase<true> {
    // To avoid overloaded-virtual warnings.
    using PlanStatsConstVisitor::visit;

    void visit(tree_walker::MaybeConstPtr<true, sbe::ScanStats> stats) override final {
        numReads += stats->numReads;
    }
    void visit(tree_walker::MaybeConstPtr<true, sbe::IndexScanStats> stats) override final {
        numReads += stats->numReads;
    }

    size_t numReads = 0;
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
