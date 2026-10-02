// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"

#include "mongo/db/exec/sbe/stages/and_hash.h"
#include "mongo/db/exec/sbe/stages/block_hashagg.h"
#include "mongo/db/exec/sbe/stages/block_to_row.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/extract_field_paths.h"
#include "mongo/db/exec/sbe/stages/fetch.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/hash_lookup.h"
#include "mongo/db/exec/sbe/stages/hash_lookup_unwind.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/stages/virtual_scan.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/string_listset.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <variant>

namespace mongo::stage_builder {
namespace {

/**
 * Lowerer consumes a blueprint subtree bottom-up and produces a concrete sbe::PlanStage tree by
 * calling makeS<T>() for each node. Each node is moved out of the vector as it is lowered.
 */
struct Lowerer {
    SbBlueprint& _blueprint;
    StageBuilderState& _state;
    const VariableTypes* _varTypes = nullptr;

    // ----------------------------------------------------------------
    // Recursive entry point: lower the subtree rooted at 'idx'. The node is replaced by
    // SbBlueprintLowered in the blueprint.
    // ----------------------------------------------------------------
    std::unique_ptr<sbe::PlanStage> lower(SbBlueprintNodeIdx idx) {
        auto node = _blueprint.takeForLowering(idx);
        return std::visit(
            [&](auto&& n) -> std::unique_ptr<sbe::PlanStage> { return (*this)(std::move(n)); },
            std::move(node));
    }

    // ----------------------------------------------------------------
    // Expression helpers
    // ----------------------------------------------------------------
    std::unique_ptr<sbe::EExpression> le(SbExpr& e) {
        return e.lower(_state, _varTypes);
    }

    sbe::SlotExprPairVector lowerSlotExprPair(SbExprSlotVector& esv,
                                              const VariableTypes* varTypes) {
        sbe::SlotExprPairVector result;
        result.reserve(esv.size());
        for (auto& [expr, slot] : esv) {
            result.emplace_back(slot.getId(), expr.lower(_state, varTypes));
        }
        return result;
    }

    static std::vector<sbe::value::SortDirection> toStdVector(const SbSortDirectionVector& dirs) {
        return {dirs.begin(), dirs.end()};
    }

    static boost::optional<sbe::value::SlotId> lowerSlotId(const boost::optional<SbSlot>& s) {
        return s ? boost::make_optional(s->getId()) : boost::none;
    }

    // ----------------------------------------------------------------
    // Leaf nodes
    // ----------------------------------------------------------------

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintLowered) {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintPassthrough node) {
        return std::move(node.stage);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintCoScan node) {
        return sbe::makeS<sbe::CoScanStage>(node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintVirtualScan node) {
        return sbe::makeS<sbe::VirtualScanStage>(
            node.nodeId, node.outSlot, sbe::value::TagValueMaybeOwned(std::move(node.input)));
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintScan node) {
        if (node.scanBounds.minRecordIdSlot || node.scanBounds.maxRecordIdSlot) {
            return sbe::makeS<sbe::ScanStage>(node.collectionUuid,
                                              std::move(node.dbName),
                                              node.recordSlot,
                                              node.recordIdSlot,
                                              lowerSlotId(node.indexInfoSlots.snapshotIdSlot),
                                              lowerSlotId(node.indexInfoSlots.indexIdentSlot),
                                              lowerSlotId(node.indexInfoSlots.indexKeySlot),
                                              lowerSlotId(node.indexInfoSlots.indexKeyPatternSlot),
                                              std::move(node.scanFieldNames),
                                              std::move(node.scanFieldSlots),
                                              lowerSlotId(node.scanBounds.minRecordIdSlot),
                                              lowerSlotId(node.scanBounds.maxRecordIdSlot),
                                              node.forward,
                                              _state.yieldPolicy,
                                              node.nodeId,
                                              std::move(node.scanOpenCallback),
                                              true /* participateInTrialRunTracking */,
                                              node.scanBounds.includeScanStartRecordId,
                                              node.scanBounds.includeScanEndRecordId);
        }

        return sbe::makeS<sbe::GenericScanStage>(
            node.collectionUuid,
            std::move(node.dbName),
            node.recordSlot,
            node.recordIdSlot,
            lowerSlotId(node.indexInfoSlots.snapshotIdSlot),
            lowerSlotId(node.indexInfoSlots.indexIdentSlot),
            lowerSlotId(node.indexInfoSlots.indexKeySlot),
            lowerSlotId(node.indexInfoSlots.indexKeyPatternSlot),
            std::move(node.scanFieldNames),
            std::move(node.scanFieldSlots),
            node.forward,
            _state.yieldPolicy,
            node.nodeId,
            std::move(node.scanOpenCallback),
            true /* participateInTrialRunTracking */);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintSimpleIndexScan node) {
        return sbe::makeS<sbe::SimpleIndexScanStage>(
            std::move(node.collectionUuid),
            std::move(node.dbName),
            node.indexName,
            node.forward,
            lowerSlotId(node.indexInfoSlots.indexKeySlot),
            node.recordIdSlot,
            lowerSlotId(node.indexInfoSlots.snapshotIdSlot),
            lowerSlotId(node.indexInfoSlots.indexIdentSlot),
            std::move(node.indexKeysToInclude),
            std::move(node.indexKeySlots),
            le(node.lowKeyExpr),
            le(node.highKeyExpr),
            _state.yieldPolicy,
            node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintGenericIndexScan node) {
        const int direction = node.forward ? 1 : -1;
        sbe::GenericIndexScanStageParams params{
            le(node.boundsExpr), node.keyPattern, direction, node.version, node.ordering};

        return sbe::makeS<sbe::GenericIndexScanStage>(
            std::move(node.collectionUuid),
            std::move(node.dbName),
            node.indexName,
            std::move(params),
            lowerSlotId(node.indexInfoSlots.indexKeySlot),
            node.recordIdSlot,
            lowerSlotId(node.indexInfoSlots.snapshotIdSlot),
            lowerSlotId(node.indexInfoSlots.indexIdentSlot),
            std::move(node.indexKeysToInclude),
            std::move(node.indexKeySlots),
            _state.yieldPolicy,
            node.nodeId);
    }

    // ----------------------------------------------------------------
    // Unary stages
    // ----------------------------------------------------------------

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintFilter node) {
        auto child = lower(node.child);
        auto cond = le(node.condition);
        switch (node.filterType) {
            case SbBlueprintFilter::FilterType::kStandard:
                return sbe::makeS<sbe::FilterStage<false>>(
                    std::move(child), std::move(cond), node.nodeId);
            case SbBlueprintFilter::FilterType::kConst:
                return sbe::makeS<sbe::FilterStage<true>>(
                    std::move(child), std::move(cond), node.nodeId);
            case SbBlueprintFilter::FilterType::kEofOnFirstFalse:
                return sbe::makeS<sbe::FilterStage<false, true>>(
                    std::move(child), std::move(cond), node.nodeId);
        }
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintProject node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::ProjectStage>(
            std::move(child), lowerSlotExprPair(node.projects, _varTypes), node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintLimitSkip node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::LimitSkipStage>(
            std::move(child), le(node.limitExpr), le(node.skipExpr), node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintSort node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::SortStage>(std::move(child),
                                          std::move(node.orderBy),
                                          toStdVector(node.dirs),
                                          std::move(node.forwardedSlots),
                                          le(node.limitExpr),
                                          node.memoryLimit,
                                          _state.allowDiskUse,
                                          _state.yieldPolicy,
                                          node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintUnique node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::UniqueStage>(std::move(child), std::move(node.keys), node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintUniqueRoaring node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::UniqueRoaringStage>(std::move(child), node.key, node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintUnwind node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::UnwindStage>(std::move(child),
                                            node.inSlot,
                                            node.outSlot,
                                            node.idxSlot,
                                            node.preserveNullAndEmptyArrays,
                                            node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintHashAgg node) {
        auto child = lower(node.child);

        const auto& knobConfig = _state.expCtx->getQueryKnobConfiguration();

        std::vector<std::unique_ptr<sbe::HashAggAccumulator>> loweredAccumulators;
        loweredAccumulators.reserve(node.accumulators.size());

        for (auto& acc : node.accumulators) {
            auto lowered = std::visit(
                OverloadedVisitor{
                    [&](SbHashAggCompiledAccumulator& impl)
                        -> std::unique_ptr<sbe::HashAggAccumulator> {
                        return std::make_unique<sbe::CompiledHashAggAccumulator>(acc.outSlot,
                                                                                 acc.spillSlot,
                                                                                 le(impl.agg),
                                                                                 le(impl.merge),
                                                                                 le(impl.init));
                    },
                    [&](SbHashAggSinglePurposeScalarAccumulator<sbe::AddToSetHashAggAccumulator>&
                            impl) -> std::unique_ptr<sbe::HashAggAccumulator> {
                        return std::make_unique<sbe::AddToSetHashAggAccumulator>(
                            acc.outSlot,
                            acc.spillSlot,
                            le(impl.transform),
                            node.collatorSlot,
                            knobConfig.getMaxAddToSetBytes());
                    },
                    [&](SbHashAggSinglePurposeScalarAccumulator<sbe::PushHashAggAccumulator>& impl)
                        -> std::unique_ptr<sbe::HashAggAccumulator> {
                        return std::make_unique<sbe::PushHashAggAccumulator>(
                            acc.outSlot,
                            acc.spillSlot,
                            le(impl.transform),
                            node.collatorSlot,
                            knobConfig.getMaxPushBytes());
                    },
                    [&]<class Impl>(SbHashAggSinglePurposeScalarAccumulator<Impl>& impl)
                        -> std::unique_ptr<sbe::HashAggAccumulator> {
                        return std::make_unique<Impl>(
                            acc.outSlot, acc.spillSlot, le(impl.transform), node.collatorSlot);
                    }},
                acc.implementation);

            loweredAccumulators.push_back(std::move(lowered));
        }

        return sbe::makeS<sbe::HashAggStage>(std::move(child),
                                             std::move(node.groupBySlots),
                                             std::move(loweredAccumulators),
                                             true /* optimized close */,
                                             node.collatorSlot,
                                             _state.allowDiskUse,
                                             _state.yieldPolicy,
                                             node.nodeId,
                                             true /* participateInTrialRunTracking */,
                                             node.forceIncreasedSpilling);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintBlockHashAgg node) {
        auto child = lower(node.child);

        sbe::BlockAggExprTupleVector aggs;
        aggs.reserve(node.aggs.size());
        for (auto& [outSlot, sbExpr] : node.aggs) {
            std::unique_ptr<sbe::EExpression> init, blockAgg, agg;
            if (sbExpr.init) {
                init = le(sbExpr.init);
            }
            if (sbExpr.blockAgg) {
                blockAgg = le(sbExpr.blockAgg);
            }
            agg = le(sbExpr.agg);

            aggs.emplace_back(
                outSlot,
                sbe::BlockAggExprTuple{std::move(init), std::move(blockAgg), std::move(agg)});
        }

        // The merging expressions are lowered without type information.
        auto mergingExprs = lowerSlotExprPair(node.mergingExprs, nullptr);

        return sbe::makeS<sbe::BlockHashAggStage>(std::move(child),
                                                  std::move(node.groupBySlots),
                                                  node.selectivityBitmapSlot,
                                                  std::move(node.blockAccArgSlots),
                                                  std::move(node.accumulatorDataSlots),
                                                  node.bitmapInternalSlot,
                                                  std::move(aggs),
                                                  _state.allowDiskUse,
                                                  std::move(mergingExprs),
                                                  _state.yieldPolicy,
                                                  node.nodeId,
                                                  true /* participateInTrialRunTracking */,
                                                  node.forceIncreasedSpilling);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintBlockToRow node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::BlockToRowStage>(std::move(child),
                                                std::move(node.blockSlots),
                                                std::move(node.outputSlots),
                                                node.bitmapSlot,
                                                node.nodeId,
                                                _state.yieldPolicy);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintTsBucketToCellBlock node) {
        auto child = lower(node.child);

        // Combine topLevel and traverse path requests and slots.
        std::vector<sbe::value::PathRequest> allReqs = std::move(node.topLevelReqs);
        allReqs.insert(allReqs.end(), node.traverseReqs.begin(), node.traverseReqs.end());

        sbe::value::SlotVector allCellSlots = std::move(node.topLevelSlots);
        allCellSlots.insert(
            allCellSlots.end(), node.traverseSlots.begin(), node.traverseSlots.end());

        return sbe::makeS<sbe::TsBucketToCellBlockStage>(std::move(child),
                                                         node.bucketSlot,
                                                         std::move(allReqs),
                                                         std::move(allCellSlots),
                                                         boost::none,  // metaSlot
                                                         node.bitmapSlot,
                                                         node.timeField,
                                                         node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintFetch node) {
        auto child = lower(node.child);

        auto stageState = std::make_shared<sbe::FetchStageState>(
            node.seekSlot,
            lowerSlotId(node.indexInfoSlots.snapshotIdSlot),
            lowerSlotId(node.indexInfoSlots.indexIdentSlot),
            lowerSlotId(node.indexInfoSlots.indexKeySlot),
            lowerSlotId(node.indexInfoSlots.indexKeyPatternSlot),
            node.recordSlot,
            node.recordIdSlot,
            StringListSet(node.scanFieldNames),
            std::move(node.scanFieldSlots),
            std::move(node.scanCallbacks));

        return sbe::makeS<sbe::FetchStage>(std::move(child),
                                           std::move(node.collectionUuid),
                                           std::move(node.dbName),
                                           std::move(stageState),
                                           _state.yieldPolicy,
                                           node.nodeId,
                                           true /* participateInTrialRunTracking */);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintExtractFieldPaths node) {
        auto child = lower(node.child);
        return sbe::makeS<sbe::ExtractFieldPathsStage>(
            std::move(child), std::move(node.inputs), std::move(node.outputs), node.nodeId);
    }

    // ----------------------------------------------------------------
    // Binary and n-ary stages
    // ----------------------------------------------------------------

    sbe::PlanStage::Vector lowerChildren(const SbBlueprintChildVector& children) {
        sbe::PlanStage::Vector stages;
        stages.reserve(children.size());
        for (auto child : children) {
            stages.push_back(lower(child));
        }
        return stages;
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintUnion node) {
        return sbe::makeS<sbe::UnionStage>(lowerChildren(node.children),
                                           std::move(node.inputSlots),
                                           std::move(node.outputSlots),
                                           node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintSortedMerge node) {
        return sbe::makeS<sbe::SortedMergeStage>(lowerChildren(node.children),
                                                 std::move(node.inputKeys),
                                                 toStdVector(node.dirs),
                                                 std::move(node.inputVals),
                                                 std::move(node.outputVals),
                                                 node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintBranch node) {
        auto thenStage = lower(node.thenChild);
        auto elseStage = lower(node.elseChild);
        return sbe::makeS<sbe::BranchStage>(std::move(thenStage),
                                            std::move(elseStage),
                                            le(node.conditionExpr),
                                            std::move(node.thenSlots),
                                            std::move(node.elseSlots),
                                            std::move(node.outputSlots),
                                            node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintLoopJoin node) {
        auto outer = lower(node.outer);
        auto inner = lower(node.inner);
        return sbe::makeS<sbe::LoopJoinStage>(std::move(outer),
                                              std::move(inner),
                                              std::move(node.outerProjects),
                                              std::move(node.outerCorrelated),
                                              std::move(node.innerProjects),
                                              le(node.predicate),
                                              node.joinType,
                                              node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintHashJoin node) {
        auto outer = lower(node.outer);
        auto inner = lower(node.inner);
        return sbe::makeS<sbe::HashJoinStage>(std::move(outer),
                                              std::move(inner),
                                              std::move(node.outerCondSlots),
                                              std::move(node.outerProjectSlots),
                                              std::move(node.innerCondSlots),
                                              std::move(node.innerProjectSlots),
                                              node.collatorSlot,
                                              _state.allowDiskUse,
                                              _state.yieldPolicy,
                                              node.nodeId,
                                              node.estimatedBuildCardinality);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintMergeJoin node) {
        auto outer = lower(node.outer);
        auto inner = lower(node.inner);
        return sbe::makeS<sbe::MergeJoinStage>(std::move(outer),
                                               std::move(inner),
                                               std::move(node.outerKeySlots),
                                               std::move(node.outerProjectSlots),
                                               std::move(node.innerKeySlots),
                                               std::move(node.innerProjectSlots),
                                               toStdVector(node.dirs),
                                               node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintAndHash node) {
        auto outer = lower(node.outer);
        auto inner = lower(node.inner);
        return sbe::makeS<sbe::AndHashStage>(std::move(outer),
                                             std::move(inner),
                                             std::move(node.outerCondSlots),
                                             std::move(node.outerProjectSlots),
                                             std::move(node.innerCondSlots),
                                             std::move(node.innerProjectSlots),
                                             node.collatorSlot,
                                             _state.yieldPolicy,
                                             node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintHashLookup node) {
        auto localStage = lower(node.localStage);
        auto foreignStage = lower(node.foreignStage);

        sbe::SlotExprPair agg{node.outSlot, le(node.aggExpr)};

        return sbe::makeS<sbe::HashLookupStage>(std::move(localStage),
                                                std::move(foreignStage),
                                                node.localKeySlot,
                                                node.foreignKeySlot,
                                                node.foreignRecordSlot,
                                                std::move(agg),
                                                node.collatorSlot,
                                                node.nodeId);
    }

    std::unique_ptr<sbe::PlanStage> operator()(SbBlueprintHashLookupUnwind node) {
        auto localStage = lower(node.localStage);
        auto foreignStage = lower(node.foreignStage);

        return sbe::makeS<sbe::HashLookupUnwindStage>(std::move(localStage),
                                                      std::move(foreignStage),
                                                      node.localKeySlot,
                                                      node.foreignKeySlot,
                                                      node.foreignRecordSlot,
                                                      node.outSlot,
                                                      node.collatorSlot,
                                                      node.joinType,
                                                      node.indexSlot,
                                                      node.nodeId);
    }
};

}  // namespace

SbStage lowerSbeBlueprint(SbBlueprint& blueprint,
                          SbBlueprintNodeIdx root,
                          StageBuilderState& state,
                          const VariableTypes* varTypes) {
    Lowerer lowerer{blueprint, state, varTypes};
    auto stage = lowerer.lower(root);
    blueprint.assertAddedNodesLowered();
    return stage;
}

}  // namespace mongo::stage_builder
