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
#include "mongo/util/str.h"

namespace mongo::sbe {
BlockToRowStage::BlockToRowStage(std::unique_ptr<PlanStage> input,
                                 value::SlotVector blocks,
                                 value::SlotVector valsOut,
                                 PlanNodeId nodeId,
                                 bool participateInTrialRunTracking)
    : PlanStage("block_to_row"_sd, nodeId, participateInTrialRunTracking),
      _blockSlotIds(std::move(blocks)),
      _valsOutSlotIds(std::move(valsOut)) {
    _children.emplace_back(std::move(input));
    invariant(_blockSlotIds.size() == _valsOutSlotIds.size());
}

std::unique_ptr<PlanStage> BlockToRowStage::clone() const {
    return std::make_unique<BlockToRowStage>(_children[0]->clone(),
                                             _blockSlotIds,
                                             _valsOutSlotIds,
                                             _commonStats.nodeId,
                                             _participateInTrialRunTracking);
}

void BlockToRowStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    for (auto& id : _blockSlotIds) {
        _blockAccessors.push_back(_children[0]->getAccessor(ctx, id));
    }

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

PlanState BlockToRowStage::getNextFromCurrentBlocks() {
    bool allDone = true;
    for (size_t i = 0; i < _blockCursors.size(); ++i) {
        auto optTagVal = _blockCursors[i]->next();
        if (!optTagVal) {
            _valsOutAccessors[i].reset();
            continue;
        }

        allDone = false;
        auto [tag, val] = *optTagVal;
        _valsOutAccessors[i].reset(tag, val);
    }

    if (allDone) {
        return PlanState::IS_EOF;
    }
    return PlanState::ADVANCED;
}

PlanState BlockToRowStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (!_blockCursors.empty() && getNextFromCurrentBlocks() == PlanState::ADVANCED) {
        return trackPlanState(PlanState::ADVANCED);
    }

    // Returns once we find a non empty block with a value to return.
    while (true) {
        // Otherwise we need to get new blocks from our child.
        _blockCursors.clear();

        auto state = _children[0]->getNext();
        if (state == PlanState::IS_EOF) {
            return trackPlanState(state);
        }

        // Initializes the block cursors.
        for (auto acc : _blockAccessors) {
            auto [tag, val] = acc->getViewOfValue();
            invariant(tag == value::TypeTags::valueBlock || tag == value::TypeTags::cellBlock);

            const auto& valueBlock = tag == value::TypeTags::valueBlock
                ? *value::getValueBlock(val)
                : value::getCellBlock(val)->getValueBlock();
            _blockCursors.push_back(valueBlock.cursor());
        }

        auto blockState = getNextFromCurrentBlocks();
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

    ret.emplace_back(DebugPrinter::Block("vals[`"));
    for (size_t i = 0; i < _valsOutSlotIds.size(); ++i) {
        if (i) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _valsOutSlotIds[i]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

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
