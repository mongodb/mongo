// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * Stage Builder Blueprints
 *
 * A blueprint is the not-yet-lowered description of an SBE PlanStage: its children, the slots it
 * reads and writes, its SbExprs (not yet lowered to sbe::EExpression), and its stage-specific
 * configuration. Each SBE stage type has a corresponding "SbBlueprint*" struct, and
 * SbBlueprintNode is the variant over all of them.
 *
 * The nodes of a blueprint tree are stored in an SbBlueprint, and children are referenced by
 * index (SbBlueprintNodeIdx) into its node vector rather than by pointer. An SbStage is a handle
 * to one of these nodes.
 *
 * Output slots in a blueprint are pre-allocated by the caller via StageBuilderState::slotId().
 * Slots are stored as plain slot IDs since lowering does not need their type signatures.
 *
 * SbBuilder methods append the blueprint for the stage they create to the SbBlueprint owned by
 * StageBuilderState and return a handle to it. The expressions of a node are finalized (optimized
 * with the caller's VariableTypes) when the node is added. The whole tree is lowered to
 * sbe::PlanStages once after the stage builders have finished, which
 * leaves room for tree-wide passes over the blueprint before lowering.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/sbe/stages/extract_field_paths.h"
#include "mongo/db/exec/sbe/stages/fetch.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/values/path_request.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {

using SbBlueprintChildVector = absl::InlinedVector<SbBlueprintNodeIdx, 2>;
using SbSortDirectionVector = absl::InlinedVector<sbe::value::SortDirection, 4>;

/**
 * Collects the slots a blueprint node defines or uses. Every node type implements
 *
 *   void def(SbSlotCollector& out) const;  // the slots the node exposes that its children do not
 *   void use(SbSlotCollector& out) const;  // the slots the node reads, including from expressions
 *
 * A slot that a stage reads and re-exposes under the same id (a sort's forwarded slots, a hash
 * aggregation's group-by slots) is a use only. Slots used but defined by no node come from the
 * RuntimeEnvironment.
 */
class SbSlotCollector {
public:
    void add(sbe::value::SlotId slot) {
        _slots.push_back(slot);
    }

    void add(const boost::optional<sbe::value::SlotId>& slot) {
        if (slot) {
            add(*slot);
        }
    }

    void add(const boost::optional<SbSlot>& slot) {
        if (slot) {
            add(slot->getId());
        }
    }

    void add(const sbe::value::SlotVector& slots) {
        _slots.insert(_slots.end(), slots.begin(), slots.end());
    }

    void add(const SbExpr& expr) {
        expr.visitReferencedSlots([&](SbSlot slot) { add(slot.getId()); });
    }

    void add(const SbBlockAggExpr& expr) {
        add(expr.init);
        add(expr.blockAgg);
        add(expr.agg);
    }

    const sbe::value::SlotVector& slots() const {
        return _slots;
    }

private:
    sbe::value::SlotVector _slots;
};

// Index info slots produced by a scan-type stage. 'indexKeyPatternSlot' is a RuntimeEnvironment
// slot registered by SbBuilder::allocateIndexInfoSlots(), not an output of the stage.
inline void addIndexInfoDefs(const SbIndexInfoSlots& slots, SbSlotCollector& out) {
    out.add(slots.indexIdentSlot);
    out.add(slots.indexKeySlot);
    out.add(slots.snapshotIdSlot);
}

// Index info slots read by a fetch-type stage to validate the record it fetches.
inline void addIndexInfoUses(const SbIndexInfoSlots& slots, SbSlotCollector& out) {
    out.add(slots.indexIdentSlot);
    out.add(slots.indexKeySlot);
    out.add(slots.indexKeyPatternSlot);
    out.add(slots.snapshotIdSlot);
}

// ============================================================
// Leaf blueprints
// ============================================================

/**
 * CoScanStage: produces an infinite stream of advances with no output slots.
 */
struct SbBlueprintCoScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector&) const {}

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * VirtualScanStage (test-only): injects a static array of values, which the blueprint owns.
 */
struct SbBlueprintVirtualScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId outSlot{0};
    sbe::value::TagValueOwned input;

    void def(SbSlotCollector& out) const {
        out.add(outSlot);
    }

    void use(SbSlotCollector&) const {}

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&&) {}
};

struct SbBlueprintScanInfo {
    UUID collectionUuid;
    DatabaseName dbName;
    bool forward{true};
    std::vector<std::string> scanFieldNames;
    SbScanBounds scanBounds;
    SbIndexInfoSlots indexInfoSlots;
    sbe::ScanOpenCallback scanOpenCallback{nullptr};

    sbe::value::SlotId recordSlot{0};
    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector scanFieldSlots;

    void def(SbSlotCollector& out) const {
        out.add(recordSlot);
        out.add(recordIdSlot);
        out.add(scanFieldSlots);
    }

    void use(SbSlotCollector& out) const {
        // A bounded scan reads its record id bounds; a seek scan also reads the index info slots to
        // validate the record it lands on.
        out.add(scanBounds.minRecordIdSlot);
        out.add(scanBounds.maxRecordIdSlot);
        addIndexInfoUses(indexInfoSlots, out);
    }

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * ScanStage / GenericScanStage: reads documents from a collection.
 *
 * If scanBounds has a minRecordIdSlot or maxRecordIdSlot a bounded ScanStage is emitted;
 * otherwise a GenericScanStage is emitted.
 */
struct SbBlueprintScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintScanInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

struct SbBlueprintMultiRangeScanInfo {
    UUID collectionUuid;
    DatabaseName dbName;
    bool forward{true};
    std::vector<std::string> scanFieldNames;
    RecordIdRangeList scanBounds;
    SbIndexInfoSlots indexInfoSlots;
    sbe::ScanOpenCallback scanOpenCallback{nullptr};

    sbe::value::SlotId recordSlot{0};
    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector scanFieldSlots;

    void def(SbSlotCollector& out) const {
        out.add(recordSlot);
        out.add(recordIdSlot);
        out.add(scanFieldSlots);
    }

    void use(SbSlotCollector& out) const {
        // The stage seeks to the start of each range and reads the index info slots to validate
        // the record it lands on.
        addIndexInfoUses(indexInfoSlots, out);
    }

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * MultiRangeClusteredScanStage: clustered collection scan over multiple disjoint RecordId ranges.
 */
struct SbBlueprintMultiRangeScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintMultiRangeScanInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

struct SbBlueprintSimpleIndexScanInfo {
    UUID collectionUuid;
    DatabaseName dbName;
    std::string indexName;
    bool forward{true};
    SbExpr lowKeyExpr;
    SbExpr highKeyExpr;
    sbe::IndexKeysInclusionSet indexKeysToInclude;

    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector indexKeySlots;
    SbIndexInfoSlots indexInfoSlots;

    void def(SbSlotCollector& out) const {
        out.add(recordIdSlot);
        out.add(indexKeySlots);
        addIndexInfoDefs(indexInfoSlots, out);
    }

    void use(SbSlotCollector& out) const {
        out.add(lowKeyExpr);
        out.add(highKeyExpr);
    }

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&& fn) {
        fn(lowKeyExpr);
        fn(highKeyExpr);
    }
};

/**
 * SimpleIndexScanStage: seeks by a simple [lowKey, highKey] range expression.
 */
struct SbBlueprintSimpleIndexScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintSimpleIndexScanInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

struct SbBlueprintGenericIndexScanInfo {
    UUID collectionUuid;
    DatabaseName dbName;
    std::string indexName;
    BSONObj keyPattern;
    bool forward{true};
    SbExpr boundsExpr;
    key_string::Version version{key_string::Version::kLatestVersion};
    Ordering ordering{Ordering::make(BSONObj{})};
    sbe::IndexKeysInclusionSet indexKeysToInclude;

    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector indexKeySlots;
    SbIndexInfoSlots indexInfoSlots;

    void def(SbSlotCollector& out) const {
        out.add(recordIdSlot);
        out.add(indexKeySlots);
        addIndexInfoDefs(indexInfoSlots, out);
    }

    void use(SbSlotCollector& out) const {
        out.add(boundsExpr);
    }

    template <typename F>
    void visitChildren(F&&) {}

    template <typename F>
    void visitExprs(F&& fn) {
        fn(boundsExpr);
    }
};

/**
 * GenericIndexScanStage: uses a generic IndexBounds expression.
 */
struct SbBlueprintGenericIndexScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintGenericIndexScanInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

// ============================================================
// Blueprints with children
// ============================================================

/**
 * FilterStage<IsConst, IsEof>.
 */
struct SbBlueprintFilter {
    enum class FilterType {
        kStandard,
        // A "cfilter": the condition is evaluated once, at open().
        kConst,
        // An "efilter": returns EOF the first time the condition is false.
        kEofOnFirstFalse,
    };

    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExpr condition;
    FilterType filterType{FilterType::kStandard};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(condition);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(condition);
    }
};

/**
 * ProjectStage. Each element of 'projects' is (expression, outSlot).
 */
struct SbBlueprintProject {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExprSlotVector projects;

    void def(SbSlotCollector& out) const {
        for (const auto& [expr, slot] : projects) {
            out.add(slot.getId());
        }
    }

    void use(SbSlotCollector& out) const {
        for (const auto& [expr, slot] : projects) {
            out.add(expr);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        for (auto& [expr, slot] : projects) {
            fn(expr);
        }
    }
};

/**
 * LimitSkipStage. Either limitExpr or skipExpr (or both) may be null.
 */
struct SbBlueprintLimitSkip {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExpr limitExpr;
    SbExpr skipExpr;

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(limitExpr);
        out.add(skipExpr);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(limitExpr);
        fn(skipExpr);
    }
};

/**
 * SortStage. 'orderBy' and 'forwardedSlots' together determine which slots survive the
 * binding-reflector boundary.
 */
struct SbBlueprintSort {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector orderBy;
    SbSortDirectionVector dirs;
    sbe::value::SlotVector forwardedSlots;
    SbExpr limitExpr;
    size_t memoryLimit{0};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(orderBy);
        out.add(forwardedSlots);
        out.add(limitExpr);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(limitExpr);
    }
};

/**
 * UniqueStage (hash-based deduplication).
 */
struct SbBlueprintUnique {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector keys;

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(keys);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * UniqueRoaringStage (roaring-bitmap deduplication over a single integer key).
 */
struct SbBlueprintUniqueRoaring {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId key{0};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(key);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * UnwindStage.
 */
struct SbBlueprintUnwind {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId inSlot{0};
    sbe::value::SlotId outSlot{0};
    sbe::value::SlotId idxSlot{0};
    bool preserveNullAndEmptyArrays{false};

    void def(SbSlotCollector& out) const {
        out.add(outSlot);
        out.add(idxSlot);
    }

    void use(SbSlotCollector& out) const {
        out.add(inSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * UnionStage. 'children[i]' contributes the slots in 'inputSlots[i]', which are remapped to
 * 'outputSlots'.
 */
struct SbBlueprintUnion {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbBlueprintChildVector children;
    std::vector<sbe::value::SlotVector> inputSlots;
    sbe::value::SlotVector outputSlots;

    void def(SbSlotCollector& out) const {
        out.add(outputSlots);
    }

    void use(SbSlotCollector& out) const {
        for (const auto& slots : inputSlots) {
            out.add(slots);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        for (auto& c : children) {
            fn(c);
        }
    }

    template <typename F>
    void visitExprs(F&&) {}
};

struct SbBlueprintSortedMergeInfo {
    SbBlueprintChildVector children;
    std::vector<sbe::value::SlotVector> inputKeys;
    SbSortDirectionVector dirs;
    std::vector<sbe::value::SlotVector> inputVals;
    sbe::value::SlotVector outputVals;

    void def(SbSlotCollector& out) const {
        out.add(outputVals);
    }

    void use(SbSlotCollector& out) const {
        for (const auto& keys : inputKeys) {
            out.add(keys);
        }
        for (const auto& vals : inputVals) {
            out.add(vals);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        for (auto& c : children) {
            fn(c);
        }
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * SortedMergeStage: an order-preserving union of children that are each sorted on
 * 'inputKeys[i]'. Each child's value slots ('inputVals[i]') are remapped to 'outputVals'.
 */
struct SbBlueprintSortedMerge {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintSortedMergeInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

/**
 * BranchStage. Evaluates 'conditionExpr' at open() and routes rows from either 'thenChild' or
 * 'elseChild'.
 */
struct SbBlueprintBranch {
    SbBlueprintNodeIdx thenChild;
    SbBlueprintNodeIdx elseChild;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExpr conditionExpr;
    sbe::value::SlotVector thenSlots;
    sbe::value::SlotVector elseSlots;
    sbe::value::SlotVector outputSlots;

    void def(SbSlotCollector& out) const {
        out.add(outputSlots);
    }

    void use(SbSlotCollector& out) const {
        out.add(conditionExpr);
        out.add(thenSlots);
        out.add(elseSlots);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(thenChild);
        fn(elseChild);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(conditionExpr);
    }
};

/**
 * LoopJoinStage (nested-loop join).
 */
struct SbBlueprintLoopJoin {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::JoinType joinType{sbe::JoinType::Inner};
    sbe::value::SlotVector outerProjects;
    sbe::value::SlotVector outerCorrelated;
    sbe::value::SlotVector innerProjects;
    SbExpr predicate;
    sbe::IntermediateTrackingBehavior intermediateTracking{
        sbe::IntermediateTrackingBehavior::kNone};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(outerProjects);
        out.add(outerCorrelated);
        out.add(innerProjects);
        out.add(predicate);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(outer);
        fn(inner);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(predicate);
    }
};

struct SbBlueprintHashJoinInfo {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    sbe::value::SlotVector outerCondSlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerCondSlots;
    sbe::value::SlotVector innerProjectSlots;
    boost::optional<sbe::value::SlotId> collatorSlot;
    boost::optional<size_t> estimatedBuildCardinality;
    sbe::IntermediateTrackingBehavior intermediateTracking{
        sbe::IntermediateTrackingBehavior::kNone};

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(outerCondSlots);
        out.add(outerProjectSlots);
        out.add(innerCondSlots);
        out.add(innerProjectSlots);
        out.add(collatorSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(outer);
        fn(inner);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * HashJoinStage.
 */
struct SbBlueprintHashJoin {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintHashJoinInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

struct SbBlueprintMergeJoinInfo {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    sbe::value::SlotVector outerKeySlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerKeySlots;
    sbe::value::SlotVector innerProjectSlots;
    SbSortDirectionVector dirs;

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(outerKeySlots);
        out.add(outerProjectSlots);
        out.add(innerKeySlots);
        out.add(innerProjectSlots);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(outer);
        fn(inner);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * MergeJoinStage: joins 'outer' and 'inner', which are both sorted on their key slots, emitting
 * the rows whose keys match. Not to be confused with SbBlueprintSortedMerge, which is an
 * order-preserving union.
 */
struct SbBlueprintMergeJoin {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintMergeJoinInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

/**
 * One accumulator of an SbBlueprintHashAgg. 'implementation' holds the (not yet lowered)
 * expressions that define the accumulator.
 */
struct SbBlueprintHashAggAccumulator {
    sbe::value::SlotId outSlot{0};
    sbe::value::SlotId spillSlot{0};
    decltype(SbHashAggAccumulator::implementation) implementation;
};

/**
 * HashAggStage (group-by aggregation). 'groupBySlots' must already be de-duplicated.
 */
struct SbBlueprintHashAgg {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector groupBySlots;
    std::vector<SbBlueprintHashAggAccumulator> accumulators;
    boost::optional<sbe::value::SlotId> collatorSlot;
    bool forceIncreasedSpilling{false};

    void def(SbSlotCollector& out) const {
        for (const auto& acc : accumulators) {
            out.add(acc.outSlot);
            // The stage fills the spill slot with a partial aggregate recovered from a spill.
            out.add(acc.spillSlot);
        }
    }

    void use(SbSlotCollector& out) const {
        out.add(groupBySlots);
        out.add(collatorSlot);
        for (const auto& acc : accumulators) {
            std::visit(OverloadedVisitor{[&](const SbHashAggCompiledAccumulator& impl) {
                                             out.add(impl.init);
                                             out.add(impl.agg);
                                             out.add(impl.merge);
                                         },
                                         [&](const auto& impl) {
                                             out.add(impl.transform);
                                         }},
                       acc.implementation);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        for (auto& acc : accumulators) {
            std::visit(OverloadedVisitor{[&](SbHashAggCompiledAccumulator& impl) {
                                             fn(impl.init);
                                             fn(impl.agg);
                                             fn(impl.merge);
                                         },
                                         [&](auto& impl) {
                                             fn(impl.transform);
                                         }},
                       acc.implementation);
        }
    }
};

struct SbBlueprintBlockHashAggInfo {
    SbBlueprintNodeIdx child;
    sbe::value::SlotVector groupBySlots;
    sbe::value::SlotId selectivityBitmapSlot{0};
    sbe::value::SlotVector blockAccArgSlots;
    sbe::value::SlotId bitmapInternalSlot{0};
    sbe::value::SlotVector accumulatorDataSlots;
    std::vector<std::pair<sbe::value::SlotId, SbBlockAggExpr>> aggs;
    SbExprSlotVector mergingExprs;
    bool forceIncreasedSpilling{false};

    void def(SbSlotCollector& out) const {
        for (const auto& [outSlot, agg] : aggs) {
            out.add(outSlot);
        }
        for (const auto& [expr, slot] : mergingExprs) {
            out.add(slot.getId());
        }
        out.add(bitmapInternalSlot);
        out.add(accumulatorDataSlots);
    }

    void use(SbSlotCollector& out) const {
        out.add(groupBySlots);
        out.add(selectivityBitmapSlot);
        out.add(blockAccArgSlots);
        for (const auto& [outSlot, agg] : aggs) {
            out.add(agg);
        }
        for (const auto& [expr, slot] : mergingExprs) {
            out.add(expr);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        for (auto& [slot, agg] : aggs) {
            fn(agg.init);
            fn(agg.blockAgg);
            fn(agg.agg);
        }
        for (auto& [expr, slot] : mergingExprs) {
            fn(expr);
        }
    }
};

/**
 * BlockHashAggStage. 'groupBySlots' must already be de-duplicated. Each element of 'aggs' is
 * (outSlot, expressions).
 */
struct SbBlueprintBlockHashAgg {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintBlockHashAggInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

/**
 * AggProjectStage: a projection whose expressions are accumulator expressions, evaluated in
 * aggregate context. Each element of 'aggs' is (outSlot, expressions).
 */
struct SbBlueprintAggProject {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::vector<std::pair<sbe::value::SlotId, SbBlockAggExpr>> aggs;

    void def(SbSlotCollector& out) const {
        for (const auto& [outSlot, agg] : aggs) {
            out.add(outSlot);
        }
    }

    void use(SbSlotCollector& out) const {
        for (const auto& [outSlot, agg] : aggs) {
            out.add(agg);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        for (auto& [slot, agg] : aggs) {
            fn(agg.init);
            fn(agg.blockAgg);
            fn(agg.agg);
        }
    }
};

/**
 * BlockToRowStage. Converts block-vectorized rows back to scalar rows. 'outputSlots' is parallel
 * to 'blockSlots'.
 */
struct SbBlueprintBlockToRow {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId bitmapSlot{0};
    sbe::value::SlotVector blockSlots;
    sbe::value::SlotVector outputSlots;

    void def(SbSlotCollector& out) const {
        out.add(outputSlots);
    }

    void use(SbSlotCollector& out) const {
        out.add(blockSlots);
        out.add(bitmapSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

struct SbBlueprintTsBucketToCellBlockInfo {
    SbBlueprintNodeIdx child;
    sbe::value::SlotId bucketSlot{0};
    std::vector<sbe::value::PathRequest> topLevelReqs;
    std::vector<sbe::value::PathRequest> traverseReqs;
    std::string timeField;
    sbe::value::SlotId bitmapSlot{0};
    sbe::value::SlotVector topLevelSlots;
    sbe::value::SlotVector traverseSlots;

    void def(SbSlotCollector& out) const {
        out.add(bitmapSlot);
        out.add(topLevelSlots);
        out.add(traverseSlots);
    }

    void use(SbSlotCollector& out) const {
        out.add(bucketSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * TsBucketToCellBlockStage (time-series bucket decomposition).
 */
struct SbBlueprintTsBucketToCellBlock {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintTsBucketToCellBlockInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

struct SbBlueprintAndHashInfo {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    sbe::value::SlotVector outerCondSlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerCondSlots;
    sbe::value::SlotVector innerProjectSlots;
    boost::optional<sbe::value::SlotId> collatorSlot;

    void def(SbSlotCollector&) const {}

    void use(SbSlotCollector& out) const {
        out.add(outerCondSlots);
        out.add(outerProjectSlots);
        out.add(innerCondSlots);
        out.add(innerProjectSlots);
        out.add(collatorSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(outer);
        fn(inner);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * AndHashStage (set intersection via hashing).
 */
struct SbBlueprintAndHash {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintAndHashInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

/**
 * HashLookupStage ($lookup implementation via hashing).
 */
struct SbBlueprintHashLookup {
    SbBlueprintNodeIdx localStage;
    SbBlueprintNodeIdx foreignStage;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId localKeySlot{0};
    sbe::value::SlotId foreignKeySlot{0};
    sbe::value::SlotId foreignRecordSlot{0};
    sbe::value::SlotId outSlot{0};
    SbExpr aggExpr;
    boost::optional<sbe::value::SlotId> collatorSlot;

    void def(SbSlotCollector& out) const {
        out.add(outSlot);
    }

    void use(SbSlotCollector& out) const {
        out.add(localKeySlot);
        out.add(foreignKeySlot);
        out.add(foreignRecordSlot);
        out.add(collatorSlot);
        out.add(aggExpr);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(localStage);
        fn(foreignStage);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        fn(aggExpr);
    }
};

/**
 * HashLookupUnwindStage ($lookup with inline unwind).
 */
struct SbBlueprintHashLookupUnwind {
    SbBlueprintNodeIdx localStage;
    SbBlueprintNodeIdx foreignStage;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::JoinType joinType{sbe::JoinType::Inner};
    sbe::value::SlotId localKeySlot{0};
    sbe::value::SlotId foreignKeySlot{0};
    sbe::value::SlotId foreignRecordSlot{0};
    sbe::value::SlotId outSlot{0};
    boost::optional<sbe::value::SlotId> collatorSlot;
    boost::optional<sbe::value::SlotId> indexSlot;

    void def(SbSlotCollector& out) const {
        out.add(outSlot);
        out.add(indexSlot);
    }

    void use(SbSlotCollector& out) const {
        out.add(localKeySlot);
        out.add(foreignKeySlot);
        out.add(foreignRecordSlot);
        out.add(collatorSlot);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(localStage);
        fn(foreignStage);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

struct SbBlueprintFetchInfo {
    SbBlueprintNodeIdx child;
    UUID collectionUuid;
    DatabaseName dbName;
    sbe::value::SlotId seekSlot{0};
    SbIndexInfoSlots indexInfoSlots;
    std::vector<std::string> scanFieldNames;
    sbe::FetchCallbacks scanCallbacks;

    sbe::value::SlotId recordSlot{0};
    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector scanFieldSlots;

    void def(SbSlotCollector& out) const {
        out.add(recordSlot);
        out.add(recordIdSlot);
        out.add(scanFieldSlots);
    }

    void use(SbSlotCollector& out) const {
        out.add(seekSlot);
        addIndexInfoUses(indexInfoSlots, out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

/**
 * FetchStage (fetches full documents by record ID from a collection).
 */
struct SbBlueprintFetch {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::unique_ptr<SbBlueprintFetchInfo> info;

    void def(SbSlotCollector& out) const {
        info->def(out);
    }

    void use(SbSlotCollector& out) const {
        info->use(out);
    }

    template <typename F>
    void visitChildren(F&& fn) {
        info->visitChildren(fn);
    }

    template <typename F>
    void visitExprs(F&& fn) {
        info->visitExprs(fn);
    }
};

/**
 * ExtractFieldPathsStage. 'inputs' and 'outputs' are (path, slotId) pairs.
 */
struct SbBlueprintExtractFieldPaths {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::vector<sbe::PathSlot> inputs;
    std::vector<sbe::PathSlot> outputs;

    void def(SbSlotCollector& out) const {
        for (const auto& [path, slot] : outputs) {
            out.add(slot);
        }
    }

    void use(SbSlotCollector& out) const {
        for (const auto& [path, slot] : inputs) {
            out.add(slot);
        }
    }

    template <typename F>
    void visitChildren(F&& fn) {
        fn(child);
    }

    template <typename F>
    void visitExprs(F&&) {}
};

// ============================================================
// SbBlueprintNode
// ============================================================

/**
 * Left in an SbBlueprintNodeVector in place of a node that has already been lowered.
 */
struct SbBlueprintLowered {
    void def(SbSlotCollector&) const {
        tasserted(13627000, "Blueprint node has already been lowered");
    }

    void use(SbSlotCollector&) const {
        tasserted(13627001, "Blueprint node has already been lowered");
    }

    template <typename F>
    void visitChildren(F&&) {
        tasserted(13621602, "Blueprint node has already been lowered");
    }

    template <typename F>
    void visitExprs(F&&) {
        tasserted(13621603, "Blueprint node has already been lowered");
    }
};

/**
 * The discriminated union of all blueprint types.
 */
using SbBlueprintNode = std::variant<SbBlueprintLowered,
                                     SbBlueprintCoScan,
                                     SbBlueprintVirtualScan,
                                     SbBlueprintScan,
                                     SbBlueprintMultiRangeScan,
                                     SbBlueprintSimpleIndexScan,
                                     SbBlueprintGenericIndexScan,
                                     SbBlueprintFilter,
                                     SbBlueprintProject,
                                     SbBlueprintLimitSkip,
                                     SbBlueprintSort,
                                     SbBlueprintUnique,
                                     SbBlueprintUniqueRoaring,
                                     SbBlueprintUnwind,
                                     SbBlueprintUnion,
                                     SbBlueprintSortedMerge,
                                     SbBlueprintBranch,
                                     SbBlueprintLoopJoin,
                                     SbBlueprintHashJoin,
                                     SbBlueprintMergeJoin,
                                     SbBlueprintHashAgg,
                                     SbBlueprintBlockHashAgg,
                                     SbBlueprintAggProject,
                                     SbBlueprintBlockToRow,
                                     SbBlueprintTsBucketToCellBlock,
                                     SbBlueprintAndHash,
                                     SbBlueprintHashLookup,
                                     SbBlueprintHashLookupUnwind,
                                     SbBlueprintFetch,
                                     SbBlueprintExtractFieldPaths>;

/**
 * Every blueprint node type implements four things:
 *
 *   visitChildren(fn)/visitExprs(fn) call 'fn' on each of the node's children (in child order)
 *       and on each SbExpr it owns (including null ones). Both pass references, so the function
 *       may modify what it is given.
 *   def(out)/use(out) report the slots the node defines and reads (see SbSlotCollector).
 */
template <typename T>
concept SbBlueprintNodeType = requires(T& node, const T& constNode, SbSlotCollector& out) {
    node.visitChildren([](SbBlueprintNodeIdx&) {});
    node.visitExprs([](SbExpr&) {});
    constNode.def(out);
    constNode.use(out);
};

template <typename Variant>
constexpr bool kAllSbBlueprintNodeTypes = false;
template <typename... Ts>
constexpr bool kAllSbBlueprintNodeTypes<std::variant<Ts...>> = (SbBlueprintNodeType<Ts> && ...);
static_assert(kAllSbBlueprintNodeTypes<SbBlueprintNode>,
              "Every blueprint node type must implement visitChildren(), visitExprs(), def() and "
              "use()");

// The node types 128 bytes or larger keep their fields in a separately allocated *Info struct,
// so that a node is little more than the largest small node type.

/**
 * Holds the nodes of a blueprint tree. Children are referenced by their index in the vector.
 */

using SbBlueprintNodeVector = std::vector<SbBlueprintNode>;

/**
 * Appends 'node' to 'nodes' and returns its index.
 */
inline SbBlueprintNodeIdx addBlueprintNode(SbBlueprintNodeVector& nodes, SbBlueprintNode node) {
    nodes.push_back(std::move(node));
    return SbBlueprintNodeIdx{static_cast<uint32_t>(nodes.size() - 1)};
}

/**
 * The blueprint tree of a plan under construction, owned by StageBuilderState. Nodes are appended
 * as the stage builders create stages and are referenced by index, so the node vector is
 * append-only; lowering takes a node out and leaves SbBlueprintLowered in its place.
 */
class SbBlueprint {
public:
    SbBlueprintNodeIdx add(SbBlueprintNode node) {
        auto idx = addBlueprintNode(_nodes, std::move(node));
        _trialRunTrackingDisabled.push_back(_noTrialRunTrackingOnAdd);
        return idx;
    }

    size_t size() const {
        return _nodes.size();
    }

    SbBlueprintNode& operator[](SbBlueprintNodeIdx idx) {
        tassert(12703001, "Invalid blueprint node index", idx.value < _nodes.size());
        return _nodes[idx.value];
    }

    const SbBlueprintNode& operator[](SbBlueprintNodeIdx idx) const {
        tassert(12703002, "Invalid blueprint node index", idx.value < _nodes.size());
        return _nodes[idx.value];
    }

    /**
     * Moves the node at 'idx' out for lowering, leaving SbBlueprintLowered behind. A node can only
     * be taken once.
     */
    SbBlueprintNode takeForLowering(SbBlueprintNodeIdx idx) {
        tassert(12702000, "Invalid blueprint node index", idx.value < _nodes.size());
        auto node = std::exchange(_nodes[idx.value], SbBlueprintLowered{});
        tassert(12702001,
                "Blueprint node has already been lowered",
                !std::holds_alternative<SbBlueprintLowered>(node));
        return node;
    }

    /**
     * Asserts that every node added since the previous call has been lowered, i.e. was part of
     * the tree that was just lowered rather than built and dropped. Each node is checked once.
     */
    void assertAddedNodesLowered() {
        tassert(12702002,
                "Expected every blueprint node to be part of the lowered tree",
                std::all_of(_nodes.begin() + _loweredPrefix, _nodes.end(), [](const auto& node) {
                    return std::holds_alternative<SbBlueprintLowered>(node);
                }));
        _loweredPrefix = _nodes.size();
    }

    bool isTrialRunTrackingDisabled(SbBlueprintNodeIdx idx) const {
        tassert(12703003, "Invalid blueprint node index", idx.value < _nodes.size());
        return _trialRunTrackingDisabled[idx.value];
    }

private:
    friend class NoTrialRunTrackingGuard;

    SbBlueprintNodeVector _nodes;

    // Parallel to '_nodes'.
    std::vector<bool> _trialRunTrackingDisabled;

    // While set (by a NoTrialRunTrackingGuard), add() marks nodes as not participating in trial
    // run tracking.
    bool _noTrialRunTrackingOnAdd{false};

    // Every node before this index has been lowered.
    size_t _loweredPrefix{0};
};

/**
 * While an instance is alive, every node added to 'blueprint' is marked as not participating in
 * trial run tracking; lowering applies the mark to the stage. Guards do not nest.
 */
class NoTrialRunTrackingGuard {
public:
    explicit NoTrialRunTrackingGuard(SbBlueprint& blueprint) : _blueprint(blueprint) {
        tassert(12703004,
                "Trial run tracking guards do not nest",
                !_blueprint._noTrialRunTrackingOnAdd);
        _blueprint._noTrialRunTrackingOnAdd = true;
    }
    NoTrialRunTrackingGuard(const NoTrialRunTrackingGuard&) = delete;
    NoTrialRunTrackingGuard& operator=(const NoTrialRunTrackingGuard&) = delete;
    ~NoTrialRunTrackingGuard() {
        _blueprint._noTrialRunTrackingOnAdd = false;
    }

private:
    SbBlueprint& _blueprint;
};

// ============================================================
// Helpers
// ============================================================

/**
 * Returns the PlanNodeId of the QSN that 'node' was built for.
 */
inline PlanNodeId getNodeId(const SbBlueprintNode& node) {
    return std::visit(OverloadedVisitor{[](const SbBlueprintLowered&) -> PlanNodeId {
                                            tasserted(13621601,
                                                      "Blueprint node has already been lowered");
                                        },
                                        [](const auto& n) -> PlanNodeId {
                                            return n.nodeId;
                                        }},
                      node);
}

}  // namespace mongo::stage_builder
