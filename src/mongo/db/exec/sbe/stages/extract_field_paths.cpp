/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/stages/extract_field_paths.h"

#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {
ExtractFieldPathsStage::ExtractFieldPathsStage(std::unique_ptr<PlanStage> input,
                                               value::SlotId inputSlotId,
                                               std::vector<value::CellBlock::Path> pathReqs,
                                               value::SlotVector outputSlotIds,
                                               PlanNodeId planNodeId,
                                               bool participateInTrialRunTracking)
    : PlanStage("extract_field_paths"_sd,
                nullptr /* yieldPolicy */,
                planNodeId,
                participateInTrialRunTracking),
      _inputSlotId(inputSlotId),
      _pathReqs(std::move(pathReqs)),
      _outputSlotIds(std::move(outputSlotIds)) {
    tassert(10984201,
            "expect pathReqs and outputSlotIds to be equal length",
            _pathReqs.size() == _outputSlotIds.size());

    for (size_t i = 0; i < _outputSlotIds.size(); i++) {
        _outputAccessorsIdxForSlotId[_outputSlotIds[i]] = i;
    }
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> ExtractFieldPathsStage::clone() const {
    return std::make_unique<ExtractFieldPathsStage>(_children[0]->clone(),
                                                    _inputSlotId,
                                                    _pathReqs,
                                                    _outputSlotIds,
                                                    _commonStats.nodeId,
                                                    participateInTrialRunTracking());
}

void ExtractFieldPathsStage::constructRoot() {
    _root = std::make_unique<value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder>>();

    _recorders.reserve(_pathReqs.size());
    for (size_t i = 0; i < _pathReqs.size(); ++i) {
        _recorders.emplace_back();
        _root->add(
            _pathReqs[i], nullptr /* filterRecorder */, &_recorders.back() /* outProjRecorder */);
    }
}

void ExtractFieldPathsStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    constructRoot();

    _outputAccessors.resize(_pathReqs.size());
    _inputAccessor = _children[0]->getAccessor(ctx, _inputSlotId);
}

value::SlotAccessor* ExtractFieldPathsStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outputAccessorsIdxForSlotId.find(slot);
        it != _outputAccessorsIdxForSlotId.end()) {
        tassert(
            10984200, "invalid slot output accessor index", it->second < _outputAccessors.size());
        return &_outputAccessors[it->second];
    }
    return _children[0]->getAccessor(ctx, slot);
}

void ExtractFieldPathsStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    _children[0]->open(reOpen);

    // Until we have valid data, we disable access to slots.
    disableSlotAccess();
}

PlanState ExtractFieldPathsStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // Disable slot access for this stage before resetting slot accessors. After the accessors get
    // updated with their new values, the 'trackPlanState(PlanState::ADVANCED)' call will re-enable
    // access for us.
    disableSlotAccess();

    for (auto& acc : _outputAccessors) {
        acc.reset();
    }

    auto state = _children[0]->getNext();
    if (state == PlanState::IS_EOF) {
        return trackPlanState(state);
    }

    auto [inputTag, inputVal] = _inputAccessor->getViewOfValue();
    // TODO SERVER-110354 Remove this restriction
    tassert(10984202,
            "ExtractFieldPathsStage currently only supports TypeTags::bsonObject as input",
            inputTag == value::TypeTags::bsonObject);
    value::walkObj<value::ScalarProjectionPositionInfoRecorder>(
        _root.get(),
        value::bitcastTo<const char*>(inputVal),
        [](value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder>* node,
           value::TypeTags eltTag,
           value::Value eltVal,
           const char* bsonPtr) {
            if (auto rec = node->projRecorder) {
                rec->recordValue(eltTag, eltVal);
            }
        });

    // Consume all outputs
    for (size_t i = 0; i < _recorders.size(); ++i) {
        auto movableGuard = _recorders[i].extractValue();
        // TODO SERVER-109926 Try to populate output slots with unowned views when possible
        _outputAccessors[i].reset(true /* owned */, movableGuard.tag(), movableGuard.value());
        // Ownership was transferred to the output accessor.
        movableGuard.disown();
    }

    return trackPlanState(PlanState::ADVANCED);
}

void ExtractFieldPathsStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _children[0]->close();
}

std::unique_ptr<PlanStageStats> ExtractFieldPathsStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* ExtractFieldPathsStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> ExtractFieldPathsStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    DebugPrinter::addIdentifier(ret, _inputSlotId);
    ret.emplace_back(DebugPrinter::Block("pathReqs[`"));
    for (size_t idx = 0; idx < _pathReqs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _outputSlotIds[idx]);
        ret.emplace_back("=");

        ret.emplace_back(value::pathToString(_pathReqs[idx]));
    }
    ret.emplace_back("`]");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());
    return ret;
}

size_t ExtractFieldPathsStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    return size;
}

void ExtractFieldPathsStage::doSaveState() {
    for (auto& accessor : _outputAccessors) {
        prepareForYielding(accessor, slotsAccessible());
    }
}
}  // namespace mongo::sbe
