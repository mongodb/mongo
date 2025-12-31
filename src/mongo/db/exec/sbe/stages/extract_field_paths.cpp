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
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {
ExtractFieldPathsStage::ExtractFieldPathsStage(std::unique_ptr<PlanStage> input,
                                               std::vector<PathSlot> inputs,
                                               std::vector<PathSlot> outputs,
                                               PlanNodeId planNodeId,
                                               bool participateInTrialRunTracking)
    : PlanStage("extract_field_paths"_sd,
                nullptr /* yieldPolicy */,
                planNodeId,
                participateInTrialRunTracking),
      _inputs(std::move(inputs)),
      _outputs(std::move(outputs)) {

    for (size_t i = 0; i < _outputs.size(); i++) {
        _outputAccessorsIdxForSlotId[_outputs[i].second] = i;
    }
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> ExtractFieldPathsStage::clone() const {
    return std::make_unique<ExtractFieldPathsStage>(_children[0]->clone(),
                                                    _inputs,
                                                    _outputs,
                                                    _commonStats.nodeId,
                                                    participateInTrialRunTracking());
}

void ExtractFieldPathsStage::constructRoot() {
    _root = std::make_unique<value::ObjectWalkNode<value::ScalarProjectionPositionInfoRecorder>>();

    _recorders.reserve(_outputs.size());
    for (size_t i = 0; i < _outputs.size(); ++i) {
        _recorders.emplace_back();
        _root->add(_outputs[i].first,
                   nullptr /* filterRecorder */,
                   &_recorders.back() /* outProjRecorder */);
    }
}

void ExtractFieldPathsStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    constructRoot();

    _outputAccessors.resize(_outputs.size());
    for (const auto& [path, slotId] : _inputs) {
        auto inputAccessor = _children[0]->getAccessor(ctx, slotId);
        _root->addAccessorAtPath(inputAccessor, path);
    }
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

    auto walk = [](value::ObjectWalkNode<value::ScalarProjectionPositionInfoRecorder>* node,
                   value::TypeTags eltTag,
                   value::Value eltVal,
                   const char* bsonPtr) {
        if (auto rec = node->projRecorder) {
            rec->recordValue(eltTag, eltVal);
        }
    };

    if (_root->inputAccessor) {
        // Should only be used for unit tests.
        auto [inputTag, inputVal] = _root->inputAccessor->getViewOfValue();

        if (value::TypeTags::bsonObject == inputTag) {
            value::walkBsonObj<value::ScalarProjectionPositionInfoRecorder>(
                _root.get(), inputVal, value::bitcastTo<const char*>(inputVal), walk);
        } else if (value::TypeTags::Object == inputTag) {
            value::walkObject<value::ScalarProjectionPositionInfoRecorder>(
                _root.get(), inputVal, walk);
        }

    } else {
        // Important this is only for toplevel fields. For nested fields, we would need knowledge of
        // arrayness. We would also need to check for input accessors during the tree traversal.
        for (const auto& child : _root->getChildren) {
            const auto& childWalkNode = child.second;
            if (childWalkNode->inputAccessor) {
                auto [childTag, childVal] = childWalkNode->inputAccessor->getViewOfValue();
                value::walkField<value::ScalarProjectionPositionInfoRecorder>(
                    childWalkNode.get(),
                    childTag,
                    childVal,
                    value::bitcastTo<const char*>(childVal),
                    walk);
            }
        }
    }

    // Consume all outputs
    for (size_t i = 0; i < _recorders.size(); ++i) {
        _outputAccessors[i].reset(_recorders[i].extractValue());
        // Ownership was transferred to the output accessor (if the value was owned).
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

void ExtractFieldPathsStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                          DebugPrintInfo& debugPrintInfo) const {
    auto addPathSlotsInAscOrderBySlotId = [&](const std::vector<PathSlot>& pathSlots,
                                              const std::string& prefix) {
        ret.emplace_back(DebugPrinter::Block(prefix));
        std::vector<std::pair<value::SlotId, size_t>> slotIdxs;
        slotIdxs.reserve(pathSlots.size());
        for (size_t idx = 0; idx < pathSlots.size(); ++idx) {
            const auto& [path, slotId] = pathSlots[idx];
            slotIdxs.push_back({slotId, idx});
        }

        std::sort(slotIdxs.begin(), slotIdxs.end());

        bool first = true;
        for (auto& [slotId, idx] : slotIdxs) {
            if (!first) {
                ret.emplace_back(DebugPrinter::Block("`,"));
            }
            first = false;
            const auto& [path, _] = pathSlots[idx];
            tassert(9719400, "expected slot ids to match", slotId == _);
            DebugPrinter::addIdentifier(ret, slotId);
            ret.emplace_back("=");
            ret.emplace_back(value::pathToString(path));
        }
        ret.emplace_back("`]");
    };

    addPathSlotsInAscOrderBySlotId(_inputs, "inputs[`");
    addPathSlotsInAscOrderBySlotId(_outputs, "outputs[`");

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
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
