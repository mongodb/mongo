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

#include "mongo/db/query/tree_walker.h"

namespace mongo {
namespace sbe {

struct ScanStats;
struct ColumnScanStats;
struct IndexScanStats;
struct FilterStats;
struct LimitSkipStats;
struct UniqueStats;
struct BranchStats;
struct CheckBoundsStats;
struct LoopJoinStats;
struct TraverseStats;
struct HashAggStats;
struct HashLookupStats;
}  // namespace sbe

struct AndHashStats;
struct AndSortedStats;
struct CachedPlanStats;
struct CollectionScanStats;
struct CountStats;
struct CountScanStats;
struct DeleteStats;
struct DistinctScanStats;
struct FetchStats;
struct IDHackStats;
struct ReturnKeyStats;
struct IndexScanStats;
struct LimitStats;
struct MockStats;
struct MultiPlanStats;
struct OrStats;
struct ProjectionStats;
struct SortStats;
struct MergeSortStats;
struct ShardingFilterStats;
struct SkipStats;
struct NearStats;
struct UpdateStats;
struct TextMatchStats;
struct TextOrStats;
struct TrialStats;
struct GroupStats;
struct DocumentSourceCursorStats;
struct DocumentSourceLookupStats;
struct UnionWithStats;
struct DocumentSourceFacetStats;
struct UnpackTimeseriesBucketStats;
struct SampleFromTimeseriesBucketStats;

/**
 * Visitor pattern for PlanStageStats.
 *
 * This code is not responsible for traversing the PlanStageStats tree, only for performing the
 * double-dispatch.
 *
 * If the visitor doesn't intend to modify the plan stats, then the template argument 'IsConst'
 * should be set to 'true'. In this case all 'visit()' methods will take a const pointer to a
 * visiting node.
 */
template <bool IsConst>
class PlanStatsVisitor {
public:
    virtual ~PlanStatsVisitor() = default;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::ScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::ColumnScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::IndexScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::FilterStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::LimitSkipStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::UniqueStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::BranchStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::CheckBoundsStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::LoopJoinStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::TraverseStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashAggStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashLookupStats> stats) = 0;

    virtual void visit(tree_walker::MaybeConstPtr<IsConst, AndHashStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, AndSortedStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, CachedPlanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, CollectionScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, CountStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, CountScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DeleteStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DistinctScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, FetchStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, IDHackStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ReturnKeyStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, IndexScanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, LimitStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, MockStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, MultiPlanStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, OrStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, SortStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, MergeSortStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, ShardingFilterStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, SkipStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, NearStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, UpdateStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TextMatchStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TextOrStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TrialStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, GroupStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceCursorStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLookupStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, UnionWithStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceFacetStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, UnpackTimeseriesBucketStats> stats) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, SampleFromTimeseriesBucketStats> stats) = 0;
};

/**
 * This class provides null implementations for all visit methods so that a derived class can
 * override visit method(s) only for interested 'SpecificStats' types.
 */
template <bool IsConst>
struct PlanStatsVisitorBase : public PlanStatsVisitor<IsConst> {
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::ScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::ColumnScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::IndexScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::FilterStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::LimitSkipStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::UniqueStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::BranchStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::CheckBoundsStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::LoopJoinStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::TraverseStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashAggStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashLookupStats> stats) override {}

    void visit(tree_walker::MaybeConstPtr<IsConst, AndHashStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, AndSortedStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, CachedPlanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, CollectionScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, CountStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, CountScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DeleteStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DistinctScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, FetchStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, IDHackStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, ReturnKeyStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, IndexScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, LimitStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, MockStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, MultiPlanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, OrStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, ProjectionStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, SortStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, MergeSortStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, ShardingFilterStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, SkipStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, NearStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, UpdateStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, TextMatchStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, TextOrStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, TrialStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, GroupStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceCursorStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLookupStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, UnionWithStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceFacetStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, UnpackTimeseriesBucketStats> stats) override {}
    void visit(
        tree_walker::MaybeConstPtr<IsConst, SampleFromTimeseriesBucketStats> stats) override {}
};

using PlanStatsMutableVisitor = PlanStatsVisitor<false>;
using PlanStatsConstVisitor = PlanStatsVisitor<true>;
}  // namespace mongo
