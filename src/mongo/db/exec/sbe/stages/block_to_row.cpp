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

#include "mongo/db/exec/sbe/stages/block_to_row.h"

#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
BlockToRowStage::BlockToRowStage(std::unique_ptr<PlanStage> input,
                                 value::SlotVector blocks,
                                 value::SlotVector valsOut,
                                 value::SlotId bitmapSlotId,
                                 PlanNodeId nodeId,
                                 PlanYieldPolicy* yieldPolicy,
                                 bool participateInTrialRunTracking)
    : PlanStage("block_to_row"_sd, yieldPolicy, nodeId, participateInTrialRunTracking),
      _blockSlotIds(std::move(blocks)),
      _valsOutSlotIds(std::move(valsOut)),
      _bitmapSlotId(bitmapSlotId) {
    _children.emplace_back(std::move(input));
    invariant(_blockSlotIds.size() == _valsOutSlotIds.size());
}

void BlockToRowStage::freeDeblockedValueRuns() {
    if (_deblockedOwned) {
        for (auto& run : _deblockedValueRuns) {
            for (auto [t, v] : run) {
                value::releaseValue(t, v);
            }
        }
        _deblockedOwned = false;
    }
    _deblockedValueRuns.clear();
}

BlockToRowStage::~BlockToRowStage() {
    freeDeblockedValueRuns();
}

std::unique_ptr<PlanStage> BlockToRowStage::clone() const {
    return std::make_unique<BlockToRowStage>(_children[0]->clone(),
                                             _blockSlotIds,
                                             _valsOutSlotIds,
                                             _bitmapSlotId,
                                             _commonStats.nodeId,
                                             _yieldPolicy,
                                             _participateInTrialRunTracking);
}

void BlockToRowStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    for (auto& id : _blockSlotIds) {
        _blockAccessors.push_back(_children[0]->getAccessor(ctx, id));
    }

    _bitmapAccessor = _children[0]->getAccessor(ctx, _bitmapSlotId);

    _valsOutAccessors.resize(_blockSlotIds.size());
}

value::SlotAccessor* BlockToRowStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    for (size_t i = 0; i < _valsOutSlotIds.size(); ++i) {
        if (slot == _valsOutSlotIds[i]) {
            return &_valsOutAccessors[i];
        }
    }

    return _children[0]->getAccessor(ctx, slot);
}

void BlockToRowStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);
}

PlanState BlockToRowStage::getNextFromDeblockedValues() {
    if (_curIdx >= _deblockedValueRuns[0].size()) {
        return PlanState::IS_EOF;
    }

    for (size_t i = 0; i < _deblockedValueRuns.size(); ++i) {
        auto [t, v] = _deblockedValueRuns[i][_curIdx];
        _valsOutAccessors[i].reset(t, v);
    }

    ++_curIdx;
    return PlanState::ADVANCED;
}

// The underlying buffer for blocks has been updated after getNext() on the child, so we need to
// prepare deblocking the new blocks.
void BlockToRowStage::prepareDeblock() {
    freeDeblockedValueRuns();

    // Extract the value in the bitmap slot into a selectivity vector, to determine which indexes
    // should get passed along and which should be filtered out.
    std::vector<char> selectivityVector;
    boost::optional<size_t> onesInBitset;
    if (_bitmapAccessor) {
        auto [bitmapTag, bitmapValue] = _bitmapAccessor->getViewOfValue();
        tassert(8044671, "Bitmap must be a block type", bitmapTag == value::TypeTags::valueBlock);
        auto bitmapBlock = value::getValueBlock(bitmapValue);
        auto extractedBitmap = bitmapBlock->extract();

        selectivityVector.resize(extractedBitmap.count());
        onesInBitset = 0;
        for (size_t i = 0; i < extractedBitmap.count(); ++i) {
            auto [t, v] = extractedBitmap[i];
            tassert(8044672, "Bitmap must contain only booleans", t == value::TypeTags::Boolean);
            auto idxPasses = value::bitcastTo<bool>(v);
            *onesInBitset += idxPasses;
            selectivityVector[i] = static_cast<char>(idxPasses);
        }
    }

    for (auto acc : _blockAccessors) {
        auto [tag, val] = acc->getViewOfValue();
        tassert(8625724,
                "Expected a valueBlock or cellBlock",
                tag == value::TypeTags::valueBlock || tag == value::TypeTags::cellBlock);

        auto* valueBlock = tag == value::TypeTags::valueBlock
            ? value::getValueBlock(val)
            : &value::getCellBlock(val)->getValueBlock();

        auto deblocked = valueBlock->extract();
        tassert(8044674,
                "Bitmap must be same size as data blocks",
                selectivityVector.empty() || deblocked.count() == selectivityVector.size());

        // Apply the selectivity vector here, only taking the values which are included.
        std::vector<std::pair<value::TypeTags, value::Value>> tvVec;
        tvVec.resize(onesInBitset.get_value_or(deblocked.count()));

        {
            size_t idxInTvVec = 0;
            for (size_t i = 0; i < deblocked.count(); ++i) {
                if (selectivityVector.empty() || selectivityVector[i]) {
                    tvVec[idxInTvVec++] = std::pair(deblocked[i].first, deblocked[i].second);
                }
            }
        }

        _deblockedValueRuns.emplace_back(std::move(tvVec));

        tassert(7962151, "Block's count must always be same as count of deblocked values", [&] {
            if (auto optCnt = valueBlock->tryCount()) {
                return *optCnt == deblocked.count();
            } else {
                return true;
            }
        }());
        tassert(7962101,
                "All deblocked value runs for output must be same size",
                _deblockedValueRuns.back().size() == _deblockedValueRuns.front().size());
    }
}

PlanState BlockToRowStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    // We may produce multiple (potentially x1000) results for a block, so we need to check for
    // interrupts so that we don't hold the lock on the underlying collection for too long.
    checkForInterrupt(_opCtx);

    if (!_deblockedValueRuns.empty() && getNextFromDeblockedValues() == PlanState::ADVANCED) {
        return trackPlanState(PlanState::ADVANCED);
    }

    // Returns once we find a non empty block with a value to return. Otherwise we need to get new
    // blocks from our child.
    while (true) {
        // We're about to call getNext() on our child and replace any state we hold in
        // _deblockedValueRuns. If we happen to yield during this call, don't bother copying our
        // current state since we're going to replace it in prepareDeblock() anyway.
        disableSlotAccess();
        auto state = _children[0]->getNext();
        if (state == PlanState::IS_EOF) {
            return trackPlanState(state);
        }

        // Got new blocks from our child, so we need to start from the beginning of the blocks.
        _curIdx = 0;
        prepareDeblock();

        auto blockState = getNextFromDeblockedValues();
        if (blockState == PlanState::ADVANCED) {
            return trackPlanState(PlanState::ADVANCED);
        }

        // Current set of blocks have no results that we're interested in, move on to the next set
        // of blocks.
    }
}

void BlockToRowStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

void BlockToRowStage::doSaveState(bool relinquishCursor) {
    if (slotsAccessible() && !_deblockedOwned) {
        for (auto& run : _deblockedValueRuns) {
            // Copy the values which have not yet been returned, starting at _curIdx.
            for (size_t i = _curIdx; i < run.size(); ++i) {
                auto [t, v] = run[i];
                run[i - _curIdx] = value::copyValue(t, v);
            }
            run.resize(run.size() - _curIdx);
        }
        _deblockedOwned = true;
        _curIdx = 0;
    }
}

std::unique_ptr<PlanStageStats> BlockToRowStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* BlockToRowStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> BlockToRowStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("blocks[`"));
    for (size_t i = 0; i < _blockSlotIds.size(); ++i) {
        if (i) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _blockSlotIds[i]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("row[`"));
    for (size_t i = 0; i < _valsOutSlotIds.size(); ++i) {
        if (i) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _valsOutSlotIds[i]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addIdentifier(ret, _bitmapSlotId);

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t BlockToRowStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    return size;
}
}  // namespace mongo::sbe
