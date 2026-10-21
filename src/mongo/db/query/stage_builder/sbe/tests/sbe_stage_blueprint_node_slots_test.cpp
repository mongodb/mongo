// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/hash_agg_accumulator.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/demangle.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

#include <absl/container/flat_hash_set.h>

namespace mongo::stage_builder {
namespace {
using sbe::value::SlotId;
using sbe::value::SlotVector;

// The index of 'T' among the node types of SbBlueprintNode.
template <typename T, size_t I = 0>
constexpr size_t nodeTypeIndex() {
    if constexpr (std::is_same_v<T, std::variant_alternative_t<I, SbBlueprintNode>>) {
        return I;
    } else {
        return nodeTypeIndex<T, I + 1>();
    }
}

SlotVector concat(SlotVector a, const SlotVector& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

SlotVector sortedUnique(SlotVector v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

/**
 * Hands out fresh slots for building nodes and checks what each node reports.
 */
class SlotMembershipChecker {
public:
    SlotId slot() {
        return _next++;
    }

    SbSlot sbSlot() {
        return SbSlot{slot()};
    }

    // An expression that reads a new slot.
    std::pair<SbExpr, SlotId> expr() {
        SlotId s = slot();
        return {SbExpr{SbSlot{s}}, s};
    }

    // A block aggregate whose expressions each read a new slot.
    std::pair<SbBlockAggExpr, SlotVector> blockAgg() {
        auto [init, initSlot] = expr();
        auto [blockAgg, blockAggSlot] = expr();
        auto [agg, aggSlot] = expr();
        return {SbBlockAggExpr{std::move(init), std::move(blockAgg), std::move(agg)},
                sbe::makeSV(initSlot, blockAggSlot, aggSlot)};
    }

    SbIndexInfoSlots indexInfo() {
        return SbIndexInfoSlots{sbSlot(), sbSlot(), sbSlot(), sbSlot()};
    }

    // Dummy index scan info.
    SbIndexInfoSlots indexInfoWithoutPattern() {
        return SbIndexInfoSlots{sbSlot(), sbSlot(), boost::none, sbSlot()};
    }

    // Asserts that 'node' defines exactly 'expectedDefs' and uses exactly 'expectedUses'.
    template <typename Node>
    void check(const Node& node, const SlotVector& expectedDefs, const SlotVector& expectedUses) {
        _checked.insert(nodeTypeIndex<Node>());
        SbSlotCollector defs;
        SbSlotCollector uses;
        node.def(defs);
        node.use(uses);
        ASSERT_EQ(sortedUnique(defs.slots()), sortedUnique(expectedDefs))
            << "defs of node type: " << demangleName(typeid(Node));
        ASSERT_EQ(sortedUnique(uses.slots()), sortedUnique(expectedUses))
            << "uses of node type: " << demangleName(typeid(Node));
    }

    size_t numCheckedNodeTypes() const {
        return _checked.size();
    }

private:
    SlotId _next{1000};
    absl::flat_hash_set<size_t> _checked;
};

SlotVector slotsOf(const SbIndexInfoSlots& s) {
    SlotVector out;
    for (const auto& slot :
         {s.indexIdentSlot, s.indexKeySlot, s.indexKeyPatternSlot, s.snapshotIdSlot}) {
        if (slot) {
            out.push_back(slot->getId());
        }
    }
    return out;
}

void checkCoScan(SlotMembershipChecker& c) {
    c.check(SbBlueprintCoScan{}, {}, {});
}

void checkVirtualScan(SlotMembershipChecker& c) {
    auto out = c.slot();
    c.check(SbBlueprintVirtualScan{.outSlot = out, .input = sbe::value::TagValueOwned::nothing()},
            {out},
            {});
}

void checkScan(SlotMembershipChecker& c) {
    auto record = c.slot();
    auto recordId = c.slot();
    auto field = c.slot();
    auto minBound = c.sbSlot();
    auto maxBound = c.sbSlot();
    auto indexInfo = c.indexInfo();
    c.check(SbBlueprintScan{.info = std::make_unique<SbBlueprintScanInfo>(
                                SbBlueprintScanInfo{.collectionUuid = UUID::gen(),
                                                    .scanBounds = SbScanBounds{minBound, maxBound},
                                                    .indexInfoSlots = indexInfo,
                                                    .recordSlot = record,
                                                    .recordIdSlot = recordId,
                                                    .scanFieldSlots = {field}})},
            {record, recordId, field},
            concat({minBound.getId(), maxBound.getId()}, slotsOf(indexInfo)));
}

void checkMultiRangeScan(SlotMembershipChecker& c) {
    auto record = c.slot();
    auto recordId = c.slot();
    auto field = c.slot();
    auto indexInfo = c.indexInfo();
    c.check(
        SbBlueprintMultiRangeScan{.info = std::make_unique<SbBlueprintMultiRangeScanInfo>(
                                      SbBlueprintMultiRangeScanInfo{.collectionUuid = UUID::gen(),
                                                                    .indexInfoSlots = indexInfo,
                                                                    .recordSlot = record,
                                                                    .recordIdSlot = recordId,
                                                                    .scanFieldSlots = {field}})},
        {record, recordId, field},
        slotsOf(indexInfo));
}

void checkSimpleIndexScan(SlotMembershipChecker& c) {
    auto recordId = c.slot();
    auto key = c.slot();
    auto [lowKey, lowKeySlot] = c.expr();
    auto [highKey, highKeySlot] = c.expr();
    auto indexInfo = c.indexInfoWithoutPattern();
    c.check(SbBlueprintSimpleIndexScan{.info = std::make_unique<SbBlueprintSimpleIndexScanInfo>(
                                           SbBlueprintSimpleIndexScanInfo{
                                               .collectionUuid = UUID::gen(),
                                               .lowKeyExpr = std::move(lowKey),
                                               .highKeyExpr = std::move(highKey),
                                               .recordIdSlot = recordId,
                                               .indexKeySlots = {key},
                                               .indexInfoSlots = indexInfo})},
            concat({recordId, key}, slotsOf(indexInfo)),
            {lowKeySlot, highKeySlot});
}

void checkGenericIndexScan(SlotMembershipChecker& c) {
    auto recordId = c.slot();
    auto key = c.slot();
    auto [bounds, boundsSlot] = c.expr();
    auto indexInfo = c.indexInfoWithoutPattern();
    c.check(SbBlueprintGenericIndexScan{.info = std::make_unique<SbBlueprintGenericIndexScanInfo>(
                                            SbBlueprintGenericIndexScanInfo{
                                                .collectionUuid = UUID::gen(),
                                                .boundsExpr = std::move(bounds),
                                                .recordIdSlot = recordId,
                                                .indexKeySlots = {key},
                                                .indexInfoSlots = indexInfo})},
            concat({recordId, key}, slotsOf(indexInfo)),
            {boundsSlot});
}

void checkFilter(SlotMembershipChecker& c) {
    auto [cond, condSlot] = c.expr();
    c.check(SbBlueprintFilter{.condition = std::move(cond)}, {}, {condSlot});
}

void checkProject(SlotMembershipChecker& c) {
    auto exprSlot = c.sbSlot();
    auto outSlot = c.sbSlot();
    SbBlueprintProject project;
    project.projects.emplace_back(SbExpr{exprSlot}, outSlot);
    c.check(project, {outSlot.getId()}, {exprSlot.getId()});
}

void checkLimitSkip(SlotMembershipChecker& c) {
    auto [limit, limitSlot] = c.expr();
    auto [skip, skipSlot] = c.expr();
    c.check(SbBlueprintLimitSkip{.limitExpr = std::move(limit), .skipExpr = std::move(skip)},
            {},
            {limitSlot, skipSlot});
}

void checkSort(SlotMembershipChecker& c) {
    auto orderBy = c.slot();
    auto forwarded = c.slot();
    auto [limit, limitSlot] = c.expr();
    c.check(SbBlueprintSort{.orderBy = {orderBy},
                            .forwardedSlots = {forwarded},
                            .limitExpr = std::move(limit)},
            {},
            {orderBy, forwarded, limitSlot});
}

void checkUnique(SlotMembershipChecker& c) {
    auto key1 = c.slot();
    auto key2 = c.slot();
    c.check(SbBlueprintUnique{.keys = {key1, key2}}, {}, {key1, key2});
}

void checkUniqueRoaring(SlotMembershipChecker& c) {
    auto key = c.slot();
    c.check(SbBlueprintUniqueRoaring{.key = key}, {}, {key});
}

void checkUnwind(SlotMembershipChecker& c) {
    auto in = c.slot();
    auto out = c.slot();
    auto idx = c.slot();
    c.check(SbBlueprintUnwind{.inSlot = in, .outSlot = out, .idxSlot = idx}, {out, idx}, {in});
}

void checkUnion(SlotMembershipChecker& c) {
    auto in1 = c.slot();
    auto in2 = c.slot();
    auto out = c.slot();
    c.check(SbBlueprintUnion{.children = {SbBlueprintNodeIdx{0}, SbBlueprintNodeIdx{1}},
                             .inputSlots = {{in1}, {in2}},
                             .outputSlots = {out}},
            {out},
            {in1, in2});
}

void checkSortedMerge(SlotMembershipChecker& c) {
    auto key = c.slot();
    auto val = c.slot();
    auto out = c.slot();
    c.check(
        SbBlueprintSortedMerge{
            .info = std::make_unique<SbBlueprintSortedMergeInfo>(SbBlueprintSortedMergeInfo{
                .inputKeys = {{key}}, .inputVals = {{val}}, .outputVals = {out}})},
        {out},
        {key, val});
}

void checkBranch(SlotMembershipChecker& c) {
    auto [cond, condSlot] = c.expr();
    auto thenSlot = c.slot();
    auto elseSlot = c.slot();
    auto out = c.slot();
    c.check(SbBlueprintBranch{.conditionExpr = std::move(cond),
                              .thenSlots = {thenSlot},
                              .elseSlots = {elseSlot},
                              .outputSlots = {out}},
            {out},
            {condSlot, thenSlot, elseSlot});
}

void checkLoopJoin(SlotMembershipChecker& c) {
    auto outerProj = c.slot();
    auto outerCorr = c.slot();
    auto innerProj = c.slot();
    auto [predicate, predicateSlot] = c.expr();
    c.check(SbBlueprintLoopJoin{.outerProjects = {outerProj},
                                .outerCorrelated = {outerCorr},
                                .innerProjects = {innerProj},
                                .predicate = std::move(predicate)},
            {},
            {outerProj, outerCorr, innerProj, predicateSlot});
}

void checkHashJoin(SlotMembershipChecker& c) {
    auto outerCond = c.slot();
    auto outerProj = c.slot();
    auto innerCond = c.slot();
    auto innerProj = c.slot();
    auto collator = c.slot();
    c.check(SbBlueprintHashJoin{.info = std::make_unique<SbBlueprintHashJoinInfo>(
                                    SbBlueprintHashJoinInfo{.outerCondSlots = {outerCond},
                                                            .outerProjectSlots = {outerProj},
                                                            .innerCondSlots = {innerCond},
                                                            .innerProjectSlots = {innerProj},
                                                            .collatorSlot = collator})},
            {},
            {outerCond, outerProj, innerCond, innerProj, collator});
}

void checkMergeJoin(SlotMembershipChecker& c) {
    auto outerKey = c.slot();
    auto outerProj = c.slot();
    auto innerKey = c.slot();
    auto innerProj = c.slot();
    c.check(SbBlueprintMergeJoin{.info = std::make_unique<SbBlueprintMergeJoinInfo>(
                                     SbBlueprintMergeJoinInfo{
                                         .outerKeySlots = {outerKey},
                                         .outerProjectSlots = {outerProj},
                                         .innerKeySlots = {innerKey},
                                         .innerProjectSlots = {innerProj},
                                         .dirs = {sbe::value::SortDirection::Ascending}})},
            {},
            {outerKey, outerProj, innerKey, innerProj});
}

void checkHashAgg(SlotMembershipChecker& c) {
    auto groupBy = c.slot();
    auto collator = c.slot();
    auto compiledOut = c.slot();
    auto compiledSpill = c.slot();
    auto [init, initSlot] = c.expr();
    auto [agg, aggSlot] = c.expr();
    auto [merge, mergeSlot] = c.expr();
    auto nativeOut = c.slot();
    auto nativeSpill = c.slot();
    auto [transform, transformSlot] = c.expr();
    SbBlueprintHashAgg hashAgg;
    hashAgg.groupBySlots = {groupBy};
    hashAgg.collatorSlot = collator;
    hashAgg.accumulators.emplace_back(
        compiledOut,
        compiledSpill,
        SbHashAggCompiledAccumulator{std::move(init), std::move(agg), std::move(merge)});
    hashAgg.accumulators.emplace_back(
        nativeOut,
        nativeSpill,
        SbHashAggSinglePurposeScalarAccumulator<sbe::AddToSetHashAggAccumulator>{
            std::move(transform)});
    c.check(hashAgg,
            {compiledOut, compiledSpill, nativeOut, nativeSpill},
            {groupBy, collator, initSlot, aggSlot, mergeSlot, transformSlot});
}

void checkBlockHashAgg(SlotMembershipChecker& c) {
    auto groupBy = c.slot();
    auto selectivityBitmap = c.slot();
    auto blockAccArg = c.slot();
    auto bitmapInternal = c.slot();
    auto accumulatorData = c.slot();
    auto aggOut = c.slot();
    auto [blockAgg, blockAggSlots] = c.blockAgg();
    auto [merging, mergingSlot] = c.expr();
    auto mergingOut = c.sbSlot();
    auto info = std::make_unique<SbBlueprintBlockHashAggInfo>();
    info->groupBySlots = {groupBy};
    info->selectivityBitmapSlot = selectivityBitmap;
    info->blockAccArgSlots = {blockAccArg};
    info->bitmapInternalSlot = bitmapInternal;
    info->accumulatorDataSlots = {accumulatorData};
    info->aggs.emplace_back(aggOut, std::move(blockAgg));
    info->mergingExprs.emplace_back(std::move(merging), mergingOut);
    c.check(SbBlueprintBlockHashAgg{.info = std::move(info)},
            {aggOut, mergingOut.getId(), bitmapInternal, accumulatorData},
            concat({groupBy, selectivityBitmap, blockAccArg, mergingSlot}, blockAggSlots));
}

void checkAggProject(SlotMembershipChecker& c) {
    auto aggOut = c.slot();
    auto [blockAgg, blockAggSlots] = c.blockAgg();
    SbBlueprintAggProject aggProject;
    aggProject.aggs.emplace_back(aggOut, std::move(blockAgg));
    c.check(aggProject, {aggOut}, blockAggSlots);
}

void checkBlockToRow(SlotMembershipChecker& c) {
    auto bitmap = c.slot();
    auto block = c.slot();
    auto out = c.slot();
    c.check(
        SbBlueprintBlockToRow{.bitmapSlot = bitmap, .blockSlots = {block}, .outputSlots = {out}},
        {out},
        {bitmap, block});
}

void checkTsBucketToCellBlock(SlotMembershipChecker& c) {
    auto bucket = c.slot();
    auto bitmap = c.slot();
    auto topLevel = c.slot();
    auto traverse = c.slot();
    c.check(
        SbBlueprintTsBucketToCellBlock{
            .info = std::make_unique<SbBlueprintTsBucketToCellBlockInfo>(
                SbBlueprintTsBucketToCellBlockInfo{.bucketSlot = bucket,
                                                   .bitmapSlot = bitmap,
                                                   .topLevelSlots = {topLevel},
                                                   .traverseSlots = {traverse}})},
        {bitmap, topLevel, traverse},
        {bucket});
}

void checkAndHash(SlotMembershipChecker& c) {
    auto outerCond = c.slot();
    auto outerProj = c.slot();
    auto innerCond = c.slot();
    auto innerProj = c.slot();
    auto collator = c.slot();
    c.check(SbBlueprintAndHash{.info = std::make_unique<SbBlueprintAndHashInfo>(
                                   SbBlueprintAndHashInfo{.outerCondSlots = {outerCond},
                                                          .outerProjectSlots = {outerProj},
                                                          .innerCondSlots = {innerCond},
                                                          .innerProjectSlots = {innerProj},
                                                          .collatorSlot = collator})},
            {},
            {outerCond, outerProj, innerCond, innerProj, collator});
}

void checkHashLookup(SlotMembershipChecker& c) {
    auto localKey = c.slot();
    auto foreignKey = c.slot();
    auto foreignRecord = c.slot();
    auto out = c.slot();
    auto [agg, aggSlot] = c.expr();
    auto collator = c.slot();
    c.check(SbBlueprintHashLookup{.localKeySlot = localKey,
                                  .foreignKeySlot = foreignKey,
                                  .foreignRecordSlot = foreignRecord,
                                  .outSlot = out,
                                  .aggExpr = std::move(agg),
                                  .collatorSlot = collator},
            {out},
            {localKey, foreignKey, foreignRecord, aggSlot, collator});
}

void checkHashLookupUnwind(SlotMembershipChecker& c) {
    auto localKey = c.slot();
    auto foreignKey = c.slot();
    auto foreignRecord = c.slot();
    auto out = c.slot();
    auto collator = c.slot();
    auto index = c.slot();
    c.check(SbBlueprintHashLookupUnwind{.localKeySlot = localKey,
                                        .foreignKeySlot = foreignKey,
                                        .foreignRecordSlot = foreignRecord,
                                        .outSlot = out,
                                        .collatorSlot = collator,
                                        .indexSlot = index},
            {out, index},
            {localKey, foreignKey, foreignRecord, collator});
}

void checkFetch(SlotMembershipChecker& c) {
    auto seek = c.slot();
    auto record = c.slot();
    auto recordId = c.slot();
    auto field = c.slot();
    auto indexInfo = c.indexInfo();
    c.check(SbBlueprintFetch{.info = std::make_unique<SbBlueprintFetchInfo>(
                                 SbBlueprintFetchInfo{.collectionUuid = UUID::gen(),
                                                      .seekSlot = seek,
                                                      .indexInfoSlots = indexInfo,
                                                      .recordSlot = record,
                                                      .recordIdSlot = recordId,
                                                      .scanFieldSlots = {field}})},
            {record, recordId, field},
            concat({seek}, slotsOf(indexInfo)));
}

void checkExtractFieldPaths(SlotMembershipChecker& c) {
    auto in = c.slot();
    auto out = c.slot();
    const sbe::value::Path path{sbe::value::Get{.field = "a"}, sbe::value::Id{}};
    c.check(SbBlueprintExtractFieldPaths{.inputs = {{path, in}}, .outputs = {{path, out}}},
            {out},
            {in});
}

// Check that every node type reports its def() and use() correctly.
TEST(SbBlueprintNodeSlotMembership, EverySlotMemberIsReported) {
    SlotMembershipChecker c;
    checkCoScan(c);
    checkVirtualScan(c);
    checkScan(c);
    checkMultiRangeScan(c);
    checkSimpleIndexScan(c);
    checkGenericIndexScan(c);
    checkFilter(c);
    checkProject(c);
    checkLimitSkip(c);
    checkSort(c);
    checkUnique(c);
    checkUniqueRoaring(c);
    checkUnwind(c);
    checkUnion(c);
    checkSortedMerge(c);
    checkBranch(c);
    checkLoopJoin(c);
    checkHashJoin(c);
    checkMergeJoin(c);
    checkHashAgg(c);
    checkBlockHashAgg(c);
    checkAggProject(c);
    checkBlockToRow(c);
    checkTsBucketToCellBlock(c);
    checkAndHash(c);
    checkHashLookup(c);
    checkHashLookupUnwind(c);
    checkFetch(c);
    checkExtractFieldPaths(c);

    // SbBlueprintLowered is a tombstone with no slots, so it has no case.
    ASSERT_EQ(c.numCheckedNodeTypes(), std::variant_size_v<SbBlueprintNode> - 1)
        << "a node type has no case in this test";
}
}  // namespace
}  // namespace mongo::stage_builder
