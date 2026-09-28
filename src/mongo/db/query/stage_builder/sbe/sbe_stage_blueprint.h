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
 * The nodes of a blueprint tree are stored in an SbBlueprintNodeVector, and children are
 * referenced by index (SbBlueprintNodeIdx) into that vector rather than by pointer.
 *
 * Output slots in a blueprint are pre-allocated by the caller via StageBuilderState::slotId().
 * Slots are stored as plain slot IDs since lowering does not need their type signatures.
 *
 * SbBuilder methods build the blueprint for the stage they create and immediately lower it
 * (sbe_stage_lowering.h/.cpp) to a unique_ptr<sbe::PlanStage>. This mirrors how SbExpr::lower()
 * works for expressions.
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
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/uuid.h"

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

/**
 * Index of a node within an SbBlueprintNodeVector.
 */
struct SbBlueprintNodeIdx {
    uint32_t value{0};

    bool operator==(const SbBlueprintNodeIdx&) const = default;
};

using SbBlueprintChildVector = absl::InlinedVector<SbBlueprintNodeIdx, 2>;
using SbSortDirectionVector = absl::InlinedVector<sbe::value::SortDirection, 4>;

// ============================================================
// Leaf blueprints
// ============================================================

/**
 * Wraps an already-lowered sbe::PlanStage so that it can be used as the child of a blueprint.
 * Lowering returns the wrapped stage unchanged.
 */
struct SbBlueprintPassthrough {
    SbStage stage;
};

/**
 * CoScanStage: produces an infinite stream of advances with no output slots.
 */
struct SbBlueprintCoScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
};

/**
 * VirtualScanStage (test-only): injects a static array of values, which the blueprint owns.
 */
struct SbBlueprintVirtualScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId outSlot{0};
    sbe::value::TagValueOwned input;
};

/**
 * ScanStage / GenericScanStage: reads documents from a collection.
 *
 * If scanBounds has a minRecordIdSlot or maxRecordIdSlot a bounded ScanStage is emitted;
 * otherwise a GenericScanStage is emitted.
 */
struct SbBlueprintScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
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
};

/**
 * SimpleIndexScanStage: seeks by a simple [lowKey, highKey] range expression.
 */
struct SbBlueprintSimpleIndexScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
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
};

/**
 * GenericIndexScanStage: uses a generic IndexBounds expression.
 */
struct SbBlueprintGenericIndexScan {
    PlanNodeId nodeId{kEmptyPlanNodeId};
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
};

/**
 * ProjectStage. Each element of 'projects' is (expression, outSlot).
 */
struct SbBlueprintProject {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExprSlotVector projects;
};

/**
 * LimitSkipStage. Either limitExpr or skipExpr (or both) may be null.
 */
struct SbBlueprintLimitSkip {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbExpr limitExpr;
    SbExpr skipExpr;
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
};

/**
 * UniqueStage (hash-based deduplication).
 */
struct SbBlueprintUnique {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector keys;
};

/**
 * UniqueRoaringStage (roaring-bitmap deduplication over a single integer key).
 */
struct SbBlueprintUniqueRoaring {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId key{0};
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
};

/**
 * SortedMergeStage: an order-preserving union of children that are each sorted on
 * 'inputKeys[i]'. Each child's value slots ('inputVals[i]') are remapped to 'outputVals'.
 */
struct SbBlueprintSortedMerge {
    PlanNodeId nodeId{kEmptyPlanNodeId};
    SbBlueprintChildVector children;
    std::vector<sbe::value::SlotVector> inputKeys;
    SbSortDirectionVector dirs;
    std::vector<sbe::value::SlotVector> inputVals;
    sbe::value::SlotVector outputVals;
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
};

/**
 * HashJoinStage.
 */
struct SbBlueprintHashJoin {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector outerCondSlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerCondSlots;
    sbe::value::SlotVector innerProjectSlots;
    boost::optional<sbe::value::SlotId> collatorSlot;
    boost::optional<size_t> estimatedBuildCardinality;
};

/**
 * MergeJoinStage: joins 'outer' and 'inner', which are both sorted on their key slots, emitting
 * the rows whose keys match. Not to be confused with SbBlueprintSortedMerge, which is an
 * order-preserving union.
 */
struct SbBlueprintMergeJoin {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector outerKeySlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerKeySlots;
    sbe::value::SlotVector innerProjectSlots;
    SbSortDirectionVector dirs;
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
};

/**
 * BlockHashAggStage. 'groupBySlots' must already be de-duplicated. Each element of 'aggs' is
 * (outSlot, expressions).
 */
struct SbBlueprintBlockHashAgg {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector groupBySlots;
    sbe::value::SlotId selectivityBitmapSlot{0};
    sbe::value::SlotVector blockAccArgSlots;
    sbe::value::SlotId bitmapInternalSlot{0};
    sbe::value::SlotVector accumulatorDataSlots;
    std::vector<std::pair<sbe::value::SlotId, SbBlockAggExpr>> aggs;
    SbExprSlotVector mergingExprs;
    bool forceIncreasedSpilling{false};
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
};

/**
 * TsBucketToCellBlockStage (time-series bucket decomposition).
 */
struct SbBlueprintTsBucketToCellBlock {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotId bucketSlot{0};
    std::vector<sbe::value::PathRequest> topLevelReqs;
    std::vector<sbe::value::PathRequest> traverseReqs;
    std::string timeField;
    sbe::value::SlotId bitmapSlot{0};
    sbe::value::SlotVector topLevelSlots;
    sbe::value::SlotVector traverseSlots;
};

/**
 * AndHashStage (set intersection via hashing).
 */
struct SbBlueprintAndHash {
    SbBlueprintNodeIdx outer;
    SbBlueprintNodeIdx inner;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    sbe::value::SlotVector outerCondSlots;
    sbe::value::SlotVector outerProjectSlots;
    sbe::value::SlotVector innerCondSlots;
    sbe::value::SlotVector innerProjectSlots;
    boost::optional<sbe::value::SlotId> collatorSlot;
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
};

/**
 * FetchStage (fetches full documents by record ID from a collection).
 */
struct SbBlueprintFetch {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    UUID collectionUuid;
    DatabaseName dbName;
    sbe::value::SlotId seekSlot{0};
    SbIndexInfoSlots indexInfoSlots;
    std::vector<std::string> scanFieldNames;
    sbe::FetchCallbacks scanCallbacks;

    sbe::value::SlotId recordSlot{0};
    sbe::value::SlotId recordIdSlot{0};
    sbe::value::SlotVector scanFieldSlots;
};

/**
 * ExtractFieldPathsStage. 'inputs' and 'outputs' are (path, slotId) pairs.
 */
struct SbBlueprintExtractFieldPaths {
    SbBlueprintNodeIdx child;
    PlanNodeId nodeId{kEmptyPlanNodeId};
    std::vector<sbe::PathSlot> inputs;
    std::vector<sbe::PathSlot> outputs;
};

// ============================================================
// SbBlueprintNode
// ============================================================

/**
 * Left in an SbBlueprintNodeVector in place of a node that has already been lowered.
 */
struct SbBlueprintLowered {};

/**
 * The discriminated union of all blueprint types.
 */
using SbBlueprintNode = std::variant<SbBlueprintLowered,
                                     SbBlueprintPassthrough,
                                     SbBlueprintCoScan,
                                     SbBlueprintVirtualScan,
                                     SbBlueprintScan,
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
                                     SbBlueprintBlockToRow,
                                     SbBlueprintTsBucketToCellBlock,
                                     SbBlueprintAndHash,
                                     SbBlueprintHashLookup,
                                     SbBlueprintHashLookupUnwind,
                                     SbBlueprintFetch,
                                     SbBlueprintExtractFieldPaths>;

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

}  // namespace mongo::stage_builder
