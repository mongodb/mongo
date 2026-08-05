// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/streaming_agg.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/value.h"

#include <string_view>
#include <utility>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

StreamingAggStage::StreamingAggStage(std::unique_ptr<PlanStage> input,
                                     value::SlotVector keys,
                                     boost::optional<value::SlotId> collatorSlot,
                                     std::vector<std::unique_ptr<HashAggAccumulator>> accumulators,
                                     PlanNodeId planNodeId,
                                     bool participateInTrialRunTracking)
    : PlanStage("streaming_group"sv,
                nullptr /* yieldPolicy */,
                planNodeId,
                participateInTrialRunTracking),
      _keySlots(std::move(keys)),
      _collatorSlot(collatorSlot),
      _accumulatorList(std::move(accumulators)) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> StreamingAggStage::clone() const {
    std::vector<std::unique_ptr<HashAggAccumulator>> accumulators;
    accumulators.reserve(_accumulatorList.size());
    for (const auto& accumulator : _accumulatorList) {
        accumulators.emplace_back(accumulator->clone());
    }

    return std::make_unique<StreamingAggStage>(_children[0]->clone(),
                                               _keySlots,
                                               _collatorSlot,
                                               std::move(accumulators),
                                               _commonStats.nodeId,
                                               participateInTrialRunTracking());
}

void StreamingAggStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(11200200,
                "collator accessor should exist if collator slot provided to StreamingAggStage",
                _collatorAccessor != nullptr);
    }

    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](value::SlotId slot) {
        auto [_, inserted] = dupCheck.emplace(slot);
        tassert(11200201, "duplicate slot id", inserted);
    };

    _outKeyAccessors.reserve(_keySlots.size());
    for (size_t i = 0; i < _keySlots.size(); ++i) {
        auto keySlot = _keySlots[i];
        throwIfDupSlot(keySlot);
        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, keySlot));
        _outKeyAccessors.emplace_back(_inOutKey, i);
        _outAccessors[keySlot] = &_outKeyAccessors.back();
    }

    _curAggAccessors.reserve(_accumulatorList.size());
    _outAggAccessors.reserve(_accumulatorList.size());
    for (auto& accumulator : _accumulatorList) {
        throwIfDupSlot(accumulator->getOutSlot());

        _curAggAccessors.emplace_back();
        _outAggAccessors.emplace_back();
        _outAccessors[accumulator->getOutSlot()] = &_outAggAccessors.back();

        ctx.root = this;
        accumulator->prepare(ctx, &_curAggAccessors.back());
    }

    _compiled = true;
}

value::SlotAccessor* StreamingAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second;
        }
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }
    return ctx.getAccessor(slot);
}

void StreamingAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    ++_commonStats.opens;

    _curKey.resize(_keySlots.size());
    _inOutKey.resize(_keySlots.size());

    if (_collatorAccessor) {
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        uassert(
            11200202, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        _keyEq = value::MaterializedRowEq(value::getCollatorView(collatorVal));
    } else {
        _keyEq = value::MaterializedRowEq();
    }

    _children[0]->open(reOpen);

    if (_children[0]->getNext() == PlanState::ADVANCED) {
        readInKey();
        swap(_curKey, _inOutKey);
        startGroup();
        _isEOF = false;
    } else {
        _isEOF = true;
    }
}

void StreamingAggStage::readInKey() {
    size_t idx = 0;
    for (auto& accessor : _inKeyAccessors) {
        _inOutKey.reset(idx++, accessor->getViewOfValue());
    }
}

void StreamingAggStage::startGroup() {
    _curKey.makeOwned();

    for (size_t idx = 0; idx < _accumulatorList.size(); ++idx) {
        _curAggAccessors[idx].reset();
        _accumulatorList[idx]->initialize(_bytecode, _curAggAccessors[idx]);
    }
    accumulate();
}

void StreamingAggStage::accumulate() {
    for (size_t idx = 0; idx < _accumulatorList.size(); ++idx) {
        _accumulatorList[idx]->accumulate(_bytecode, _curAggAccessors[idx]);
    }
}

void StreamingAggStage::endGroup() {
    swap(_curKey, _inOutKey);

    for (size_t idx = 0; idx < _accumulatorList.size(); ++idx) {
        _outAggAccessors[idx].reset(_curAggAccessors[idx].copyOrMoveValue());
    }
}

PlanState StreamingAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    if (MONGO_unlikely(_isEOF)) {
        return trackPlanState(PlanState::IS_EOF);
    }

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        readInKey();
        if (!_keyEq(_curKey, _inOutKey)) {
            endGroup();
            startGroup();
            return trackPlanState(PlanState::ADVANCED);
        }

        accumulate();
    }

    endGroup();
    _isEOF = true;
    return trackPlanState(PlanState::ADVANCED);
}

void StreamingAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _isEOF = true;
    _curKey = value::MaterializedRow{};
    _inOutKey = value::MaterializedRow{};

    _children[0]->close();
}

std::unique_ptr<PlanStageStats> StreamingAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("keySlots", _keySlots.begin(), _keySlots.end());
        if (_collatorSlot) {
            bob.appendNumber("collatorSlot", static_cast<long long>(*_collatorSlot));
        }
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* StreamingAggStage::getSpecificStats() const {
    return nullptr;
}

void StreamingAggStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                     DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _keySlots.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _keySlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _accumulatorList.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        const auto& accumulator = _accumulatorList[idx];
        DebugPrinter::addIdentifier(ret, accumulator->getOutSlot());
        ret.emplace_back(DebugPrinter::Block("="));
        DebugPrinter::addBlocks(ret, accumulator->debugPrintAccumulate());

        if (auto initializer = accumulator->debugPrintInitialize(); initializer) {
            ret.emplace_back(DebugPrinter::Block("init{`"));
            DebugPrinter::addBlocks(ret, std::move(*initializer));
            ret.emplace_back(DebugPrinter::Block("`}"));
        }
    }
    ret.emplace_back("`]");

    if (_collatorSlot) {
        ret.emplace_back(DebugPrinter::Block("collator"));
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t StreamingAggStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_keySlots);
    size += size_estimator::estimate(_accumulatorList);
    return size;
}
}  // namespace mongo::sbe
