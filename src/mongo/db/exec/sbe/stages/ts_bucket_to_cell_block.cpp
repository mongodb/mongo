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

#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/empty_cell_block.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/ts_block.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::sbe {
TsBucketToCellBlockStage::TsBucketToCellBlockStage(std::unique_ptr<PlanStage> input,
                                                   value::SlotId bucketSlot,
                                                   std::vector<std::string> topLevelPaths,
                                                   value::SlotVector blocksOut,
                                                   boost::optional<value::SlotId> metaOut,
                                                   bool hasMetaField,
                                                   const std::string& timeField,
                                                   PlanNodeId nodeId,
                                                   bool participateInTrialRunTracking)
    : PlanStage("ts_bucket_to_cellblock"_sd, nodeId, participateInTrialRunTracking),
      _bucketSlotId(bucketSlot),
      _topLevelPaths(std::move(topLevelPaths)),
      _blocksOutSlotId(std::move(blocksOut)),
      _metaOutSlotId(metaOut),
      _hasMetaField(hasMetaField),
      _timeField(timeField) {
    tassert(7796402,
            "Meta slot is requested but no 'meta' field in timeseries collection options",
            !_metaOutSlotId || _hasMetaField);
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> TsBucketToCellBlockStage::clone() const {
    return std::make_unique<TsBucketToCellBlockStage>(_children[0]->clone(),
                                                      _bucketSlotId,
                                                      _topLevelPaths,
                                                      _blocksOutSlotId,
                                                      _metaOutSlotId,
                                                      _hasMetaField,
                                                      _timeField,
                                                      _commonStats.nodeId,
                                                      _participateInTrialRunTracking);
}

void TsBucketToCellBlockStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    // Gets the incoming accessor for buckets.
    _bucketAccessor = _children[0]->getAccessor(ctx, _bucketSlotId);

    _tsCellBlocks.resize(_topLevelPaths.size());
    _blocksOutAccessor.resize(_topLevelPaths.size());
}

value::SlotAccessor* TsBucketToCellBlockStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_metaOutSlotId && slot == *_metaOutSlotId) {
        return &_metaOutAccessor;
    }

    for (size_t i = 0; i < _topLevelPaths.size(); ++i) {
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
}

PlanState TsBucketToCellBlockStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    for (auto& acc : _blocksOutAccessor) {
        acc.reset();
    }

    auto state = _children[0]->getNext();
    if (state == PlanState::IS_EOF) {
        return trackPlanState(state);
    }

    auto [bucketTag, bucketVal] = _bucketAccessor->getViewOfValue();
    invariant(bucketTag == value::TypeTags::bsonObject);

    BSONObj bucketObj(value::getRawPointerView(bucketVal));

    if (_metaOutSlotId && _hasMetaField) {
        auto metaElt = bucketObj[timeseries::kBucketMetaFieldName];
        auto [metaTag, metaVal] = bson::convertFrom<true>(metaElt);
        _metaOutAccessor.reset(false, metaTag, metaVal);
    }

    BSONElement bucketControl = bucketObj[timeseries::kBucketControlFieldName];
    invariant(!bucketControl.eoo());
    BSONElement data = bucketObj[timeseries::kBucketDataFieldName];
    invariant(!data.eoo());
    invariant(data.type() == BSONType::Object);

    for (auto elt : data.embeddedObject()) {
        for (size_t i = 0; i < _topLevelPaths.size(); ++i) {
            if (elt.fieldName() == _topLevelPaths[i]) {
                {
                    auto [blockTag, blockVal] = bson::convertFrom<true>(elt);
                    tassert(7796400,
                            "Unsupported type for timeseries bucket data",
                            blockTag == value::TypeTags::bsonObject ||
                                blockTag == value::TypeTags::bsonBinData);

                    // Up to this point, the storage engine owns the underlying storage buffer for
                    // the block and so the CellBlock must not own it.
                    _tsCellBlocks[i].emplace(/*owned*/ false, blockTag, blockVal);
                }

                _blocksOutAccessor[i].reset(
                    false,
                    value::TypeTags::cellBlock,
                    value::bitcastFrom<value::CellBlock*>(&_tsCellBlocks[i].get()));
            }
        }
    }

    // Any block slots that are still Nothing get filled with an EmptyBlock. This way later stages
    // do not have to deal with block slots being Nothing.
    for (size_t i = 0; i < _blocksOutAccessor.size(); ++i) {
        if (_blocksOutAccessor[i].getViewOfValue().first == value::TypeTags::Nothing) {
            auto emptyBlock = std::make_unique<value::EmptyCellBlock>();
            _blocksOutAccessor[i].reset(
                true,
                value::TypeTags::cellBlock,
                value::bitcastFrom<value::CellBlock*>(emptyBlock.release()));
        }
    }

    return trackPlanState(state);
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

    ret.emplace_back(DebugPrinter::Block("paths[`"));
    for (size_t idx = 0; idx < _topLevelPaths.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _blocksOutSlotId[idx]);
        ret.emplace_back(" = ");

        ret.emplace_back(_topLevelPaths[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    if (_metaOutSlotId) {
        ret.emplace_back("meta = ");
        DebugPrinter::addIdentifier(ret, *_metaOutSlotId);
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
}  // namespace mongo::sbe
