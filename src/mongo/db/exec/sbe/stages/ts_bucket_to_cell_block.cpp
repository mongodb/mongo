// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
TsBucketToCellBlockStage::TsBucketToCellBlockStage(std::unique_ptr<PlanStage> input,
                                                   value::SlotId bucketSlot,
                                                   std::vector<value::PathRequest> pathReqs,
                                                   value::SlotVector blocksOut,
                                                   boost::optional<value::SlotId> metaOut,
                                                   value::SlotId bitmapOutSlotId,
                                                   const std::string& timeField,
                                                   PlanNodeId nodeId,
                                                   bool participateInTrialRunTracking)
    : PlanStage("ts_bucket_to_cellblock"sv,
                nullptr /* yieldPolicy */,
                nodeId,
                participateInTrialRunTracking),
      _bucketSlotId(bucketSlot),
      _pathReqs(pathReqs),
      _blocksOutSlotId(std::move(blocksOut)),
      _metaOutSlotId(metaOut),
      _bitmapOutSlotId(bitmapOutSlotId),
      _timeField(timeField),
      _pathExtractor(pathReqs, _timeField) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> TsBucketToCellBlockStage::clone() const {
    return std::make_unique<TsBucketToCellBlockStage>(_children[0]->clone(),
                                                      _bucketSlotId,
                                                      _pathReqs,
                                                      _blocksOutSlotId,
                                                      _metaOutSlotId,
                                                      _bitmapOutSlotId,
                                                      _timeField,
                                                      _commonStats.nodeId,
                                                      participateInTrialRunTracking());
}

void TsBucketToCellBlockStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Gets the incoming accessor for buckets.
    _bucketAccessor = _children[0]->getAccessor(ctx, _bucketSlotId);

    _blocksOutAccessor.resize(_pathReqs.size());
}

value::SlotAccessor* TsBucketToCellBlockStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (slot == _bitmapOutSlotId) {
        return &_bitmapOutAccessor;
    }

    if (_metaOutSlotId && slot == *_metaOutSlotId) {
        return &_metaOutAccessor;
    }

    for (size_t i = 0; i < _pathReqs.size(); ++i) {
        if (slot == _blocksOutSlotId[i]) {
            return &_blocksOutAccessor[i];
        }
    }

    return _children[0]->getAccessor(ctx, slot);
}

void TsBucketToCellBlockStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);

    // Until we have valid data, we disable access to slots.
    disableSlotAccess();
}

PlanState TsBucketToCellBlockStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // We are about to call getNext() on our child so do not bother saving our internal state in
    // case it yields as the state will be completely overwritten after the getNext() call.
    disableSlotAccess();

    // Before throwing away the TSBlocks we currently hold onto, count how many of them were
    // decompressed.
    for (size_t i = 0; i < _tsBlockStorage.size(); ++i) {
        _specificStats.numStorageBlocks++;
        _specificStats.numStorageBlocksDecompressed += _tsBlockStorage[i]->decompressed();
    }

    for (auto& acc : _blocksOutAccessor) {
        acc.reset();
    }

    auto state = _children[0]->getNext();
    if (state == PlanState::IS_EOF) {
        return trackPlanState(state);
    }
    state = trackPlanState(state);

    initCellBlocks();

    _specificStats.numCellBlocksProduced += _blocksOutAccessor.size();

    return state;
}

void TsBucketToCellBlockStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> TsBucketToCellBlockStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("numCellBlocksProduced",
                         static_cast<long long>(_specificStats.numCellBlocksProduced));
        bob.appendNumber("numStorageBlocks",
                         static_cast<long long>(_specificStats.numStorageBlocks));
        bob.appendNumber("numStorageBlocksDecompressed",
                         static_cast<long long>(_specificStats.numStorageBlocksDecompressed));

        ret->debugInfo = bob.obj();
    }
    return ret;
}

const SpecificStats* TsBucketToCellBlockStage::getSpecificStats() const {
    return &_specificStats;
}

void TsBucketToCellBlockStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                            DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addIdentifier(ret, _bucketSlotId);

    ret.emplace_back(DebugPrinter::Block("pathReqs[`"));
    for (size_t idx = 0; idx < _pathReqs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _blocksOutSlotId[idx]);
        ret.emplace_back("=");

        ret.emplace_back(_pathReqs[idx].toString());
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    if (_metaOutSlotId) {
        ret.emplace_back("meta =");
        DebugPrinter::addIdentifier(ret, *_metaOutSlotId);
    }

    ret.emplace_back("bitmap =");
    DebugPrinter::addIdentifier(ret, _bitmapOutSlotId);

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t TsBucketToCellBlockStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    return size;
}

void TsBucketToCellBlockStage::doSaveState() {
    if (!slotsAccessible()) {
        return;
    }

    for (size_t i = 0; i < _blocksOutAccessor.size(); ++i) {
        // Copy the CellBlock, which will force any data that's unowned by SBE (owned by storage)
        // memory to be copied. This also means that any already decompressed data will get copied,
        // and will not need to be decompressed again.
        auto [cellBlockTag, cellBlockVal] = _blocksOutAccessor[i].getViewOfValue();

        _blocksOutAccessor[i].reset(
            value::TagValueOwned::fromRaw(value::copyValue(cellBlockTag, cellBlockVal)));
    }

    if (_metaOutSlotId) {
        prepareForYielding(_metaOutAccessor, slotsAccessible());
    }
}

void TsBucketToCellBlockStage::initCellBlocks() {
    auto [bucketTag, bucketVal] = _bucketAccessor->getViewOfValue();
    tassert(11093509, "Expected bsonObject tag type", bucketTag == value::TypeTags::bsonObject);

    BSONObj bucketObj(value::getRawPointerView(bucketVal));
    if (_metaOutSlotId) {
        auto metaElt = bucketObj[timeseries::kBucketMetaFieldName];
        auto [metaTag, metaVal] = bson::convertToView(metaElt);
        _metaOutAccessor.reset(value::TagValueView{metaTag, metaVal});
    }

    auto [nMeasurements, tsBlocks, cellBlocks] = _pathExtractor.extractCellBlocks(bucketObj);
    _tsBlockStorage = std::move(tsBlocks);
    tassert(11093510,
            "Number of cell blocks doesn't match the number of accessors",
            cellBlocks.size() == _blocksOutAccessor.size());
    for (size_t i = 0; i < cellBlocks.size(); ++i) {
        _blocksOutAccessor[i].reset(value::TagValueOwned::fromRaw(
            value::TypeTags::cellBlock,
            value::bitcastFrom<value::CellBlock*>(cellBlocks[i].release())));
    }

    // Initialize an all-1s bitset.
    _bitmapOutAccessor.reset(value::TagValueOwned::fromRaw(
        value::TypeTags::valueBlock,
        value::bitcastFrom<value::ValueBlock*>(
            std::make_unique<value::MonoBlock>(
                nMeasurements, value::TypeTags::Boolean, value::bitcastFrom<bool>(true))
                .release())));
}
}  // namespace mongo::sbe
