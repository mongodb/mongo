/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/ts_bucket_to_cell_block.h"

#include <cstddef>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
TsBucketToCellBlockStage::TsBucketToCellBlockStage(
    std::unique_ptr<PlanStage> input,
    value::SlotId bucketSlot,
    std::vector<value::CellBlock::PathRequest> pathReqs,
    value::SlotVector blocksOut,
    boost::optional<value::SlotId> metaOut,
    const std::string& timeField,
    PlanNodeId nodeId,
    bool participateInTrialRunTracking)
    : PlanStage("ts_bucket_to_cellblock"_sd,
                nullptr /* yieldPolicy */,
                nodeId,
                participateInTrialRunTracking),
      _bucketSlotId(bucketSlot),
      _pathReqs(pathReqs),
      _blocksOutSlotId(std::move(blocksOut)),
      _metaOutSlotId(metaOut),
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
                                                      _timeField,
                                                      _commonStats.nodeId,
                                                      _participateInTrialRunTracking);
}

void TsBucketToCellBlockStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Gets the incoming accessor for buckets.
    _bucketAccessor = _children[0]->getAccessor(ctx, _bucketSlotId);

    _blocksOutAccessor.resize(_pathReqs.size());
}

value::SlotAccessor* TsBucketToCellBlockStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
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

    for (auto& acc : _blocksOutAccessor) {
        acc.reset();
    }

    auto state = _children[0]->getNext();
    if (state == PlanState::IS_EOF) {
        return trackPlanState(state);
    }
    state = trackPlanState(state);

    initCellBlocks();

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
    return ret;
}

const SpecificStats* TsBucketToCellBlockStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> TsBucketToCellBlockStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

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
        DebugPrinter::addIdentifier(ret, *_metaOutSlotId);
        ret.emplace_back("= meta");
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t TsBucketToCellBlockStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    return size;
}

void TsBucketToCellBlockStage::doSaveState(bool) {
    if (!slotsAccessible()) {
        return;
    }

    for (size_t i = 0; i < _blocksOutAccessor.size(); ++i) {
        // Copy the CellBlock, which will force any data that's unowned by SBE (owned by storage)
        // memory to be copied. This also means that any already decompressed data will get copied,
        // and will not need to be decompressed again.
        auto [cellBlockTag, cellBlockVal] = _blocksOutAccessor[i].getViewOfValue();

        auto [cpyTag, cpyVal] = value::copyValue(cellBlockTag, cellBlockVal);
        _blocksOutAccessor[i].reset(true, cpyTag, cpyVal);
    }
}

void TsBucketToCellBlockStage::initCellBlocks() {
    auto [bucketTag, bucketVal] = _bucketAccessor->getViewOfValue();
    invariant(bucketTag == value::TypeTags::bsonObject);

    BSONObj bucketObj(value::getRawPointerView(bucketVal));
    if (_metaOutSlotId) {
        auto metaElt = bucketObj[timeseries::kBucketMetaFieldName];
        auto [metaTag, metaVal] = bson::convertFrom<true>(metaElt);
        _metaOutAccessor.reset(false, metaTag, metaVal);
    }

    auto [tsBlocks, cellBlocks] = _pathExtractor.extractCellBlocks(bucketObj);
    _tsBlockStorage = std::move(tsBlocks);
    invariant(cellBlocks.size() == _blocksOutAccessor.size());
    for (size_t i = 0; i < cellBlocks.size(); ++i) {
        _blocksOutAccessor[i].reset(true,
                                    value::TypeTags::cellBlock,
                                    value::bitcastFrom<value::CellBlock*>(cellBlocks[i].release()));
    }
}
}  // namespace mongo::sbe
