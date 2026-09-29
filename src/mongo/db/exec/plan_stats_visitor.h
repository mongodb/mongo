// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/tree_walker.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace sbe {

struct ScanStats;
struct FetchStats;
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
struct HashJoinStats;
struct WindowStats;
struct SearchStats;
struct TsBucketToBlockStats;
struct MergeJoinStats;
struct AndHashStats;
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
struct DocumentSourceMergeCursorsStats;
struct DocumentSourceLookupStats;
struct UnionWithStats;
struct DocumentSourceFacetStats;
struct UnpackTimeseriesBucketStats;
struct TimeseriesModifyStats;
struct SampleFromTimeseriesBucketStats;
struct SpoolStats;
struct EofStats;
struct DocumentSourceIdLookupStats;
struct DocumentSourceGraphLookupStats;
struct DocumentSourceBucketAutoStats;
struct DocumentSourceSetWindowFieldsStats;

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
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::FetchStats> stats) = 0;
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
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashJoinStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::WindowStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::SearchStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::TsBucketToBlockStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::MergeJoinStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, sbe::AndHashStats> stats) = 0;

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
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceMergeCursorsStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLookupStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, UnionWithStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceFacetStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, UnpackTimeseriesBucketStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, TimeseriesModifyStats> stats) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, SampleFromTimeseriesBucketStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, SpoolStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, EofStats> stats) = 0;
    virtual void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceIdLookupStats> stats) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceGraphLookupStats> stats) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceBucketAutoStats> stats) = 0;
    virtual void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceSetWindowFieldsStats> stats) = 0;
};

/**
 * This class provides null implementations for all visit methods so that a derived class can
 * override visit method(s) only for interested 'SpecificStats' types.
 */
template <bool IsConst>
struct PlanStatsVisitorBase : public PlanStatsVisitor<IsConst> {
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::ScanStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::FetchStats> stats) override {}
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
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::HashJoinStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::WindowStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::SearchStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::TsBucketToBlockStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::MergeJoinStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, sbe::AndHashStats> stats) override {}

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
    void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceMergeCursorsStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceLookupStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, UnionWithStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceFacetStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, UnpackTimeseriesBucketStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, TimeseriesModifyStats> stats) override {}
    void visit(
        tree_walker::MaybeConstPtr<IsConst, SampleFromTimeseriesBucketStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, SpoolStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, EofStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceIdLookupStats> stats) override {}
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceGraphLookupStats> stats) override {
    }
    void visit(tree_walker::MaybeConstPtr<IsConst, DocumentSourceBucketAutoStats> stats) override {}
    void visit(
        tree_walker::MaybeConstPtr<IsConst, DocumentSourceSetWindowFieldsStats> stats) override {}
};

using PlanStatsMutableVisitor = PlanStatsVisitor<false>;
using PlanStatsConstVisitor = PlanStatsVisitor<true>;
}  // namespace mongo
