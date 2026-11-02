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
                                     value::SlotVector keySlots,
                                     value::SlotVector clusterKeySlots,
                                     boost::optional<value::SlotId> collatorSlot,
                                     std::vector<std::unique_ptr<HashAggAccumulator>> accumulators,
                                     PlanNodeId planNodeId,
                                     bool participateInTrialRunTracking)
    : PlanStage("streaming_group"sv,
                /*yieldPolicy=*/nullptr,
                planNodeId,
                participateInTrialRunTracking),
      _keySlots(std::move(keySlots)),
      _clusterKeySlots(std::move(clusterKeySlots)),
      _collatorSlot(collatorSlot),
      _accumulatorList(std::move(accumulators)) {
    tassert(13625700,
            "clusterKeySlots must hold one slot per key slot",
            _clusterKeySlots.size() == _keySlots.size());
    size_t clusteredGroupKeys = 0;
    for (size_t i = 0; i < _keySlots.size(); ++i) {
        if (_keySlots[i] != _clusterKeySlots[i]) {
            ++clusteredGroupKeys;
        }
    }
    tassert(
        13625701, "Too many clustered group keys", clusteredGroupKeys <= kMaxClusteredGroupKeys);
    _noClustering = clusteredGroupKeys == 0;

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
                                               _clusterKeySlots,
                                               _collatorSlot,
                                               std::move(accumulators),
                                               _commonStats.nodeId,
                                               participateInTrialRunTracking());
}

void StreamingAggStage::prepare(CompileCtx& ctx) {
    _bytecode = ctx.getByteCode();
    _children[0]->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(11200200,
                "collator accessor should exist if collator slot provided to StreamingAggStage",
                _collatorAccessor != nullptr);
    }

    // Group-by slots and accumulator out slots must be unique, but cluster key slots may repeat a
    // group-by slot id, so they only need to be unique among themselves.
    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](value::SlotId slot) {
        auto [_, inserted] = dupCheck.emplace(slot);
        tassert(11200201, "Duplicate slot id", inserted);
    };
    value::SlotSet clusteredDupCheck;
    auto throwIfDupClusteredSlot = [&clusteredDupCheck](value::SlotId slot) {
        auto [_, inserted] = clusteredDupCheck.emplace(slot);
        tassert(13625703, "Duplicate cluster key slot id", inserted);
    };

    _outKeyAccessors.reserve(_keySlots.size());
    for (size_t i = 0; i < _keySlots.size(); ++i) {
        auto keySlot = _keySlots[i];
        throwIfDupSlot(keySlot);
        throwIfDupClusteredSlot(_clusterKeySlots[i]);
        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, keySlot));
        _clusteredInKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, _clusterKeySlots[i]));
        _outKeyAccessors.emplace_back(_outKey, i);
        _outAccessors[keySlot] = &_outKeyAccessors.back();
    }

    _curAggAccessors.reserve(_accumulatorList.size());
    _outAggAccessors.reserve(_accumulatorList.size());
    for (size_t i = 0; i < _accumulatorList.size(); ++i) {
        auto& accumulator = _accumulatorList[i];
        throwIfDupSlot(accumulator->getOutSlot());

        _curAggAccessors.emplace_back(_groupAggs, _curGroupIdx, i);
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

    _inGroupKey.resize(_keySlots.size());
    _inClusterKey.resize(_keySlots.size());
    _clusterKey.resize(_keySlots.size());
    _outKey.resize(_keySlots.size());
    _groupKeys.clear();
    _groupAggs.clear();
    _numLiveGroups = 0;
    _emitIdx = kNoGroup;
    _curGroupIdx = kNoGroup;
    _nextKey.resize(_keySlots.size());
    _nextAgg.resize(_accumulatorList.size());

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
        readInGroupKey();
        startGroup();
        if (!_noClustering) {
            readInClusterKey();
            storeClusterKey();
        }
        accumulate();
        _isEOF = false;
    } else {
        _isEOF = true;
    }
}

void StreamingAggStage::readInGroupKey() {
    for (size_t i = 0; i < _inKeyAccessors.size(); ++i) {
        _inGroupKey.reset(i, _inKeyAccessors[i]->getViewOfValue());
    }
}

void StreamingAggStage::readInClusterKey() {
    for (size_t i = 0; i < _clusteredInKeyAccessors.size(); ++i) {
        _inClusterKey.reset(i, _clusteredInKeyAccessors[i]->getViewOfValue());
    }
}

void StreamingAggStage::storeClusterKey() {
    swap(_clusterKey, _inClusterKey);
    _clusterKey.makeOwned();
}

void StreamingAggStage::endCluster() {
    tassert(13625709,
            "All rows from previous cluster must be emitted before emitting next cluster",
            _emitIdx == kNoGroup);

    if (!_isEOF && _curGroupIdx != kNoGroup) {
        // stash the first row of the next cluster
        tassert(13625710,
                "At a cluster boundary, the next group must be at the end of the vector",
                _curGroupIdx == _numLiveGroups - 1);
        swap(_groupKeys[_curGroupIdx], _nextKey);
        swap(_groupAggs[_curGroupIdx], _nextAgg);
        _curGroupIdx = kNoGroup;
        --_numLiveGroups;
    }

    _emitIdx = 0;

    bool emitted = emitNext();
    tassert(13625702, "A finished cluster must produce at least one group.", emitted);
}

bool StreamingAggStage::startGroup() {
    // Change to a new group key, which may have already been seen in the current cluster key.
    for (size_t i = 0; i < _numLiveGroups; ++i) {
        if (_keyEq(_groupKeys[i], _inGroupKey)) {
            _curGroupIdx = i;
            return false;
        }
    }

    startNewGroupAt(_numLiveGroups);
    ++_numLiveGroups;
    return true;
}

void StreamingAggStage::startNewGroupAt(size_t idx) {
    // Initialize a row buffer for the current group at 'idx' in _groupKeys/_groupAggs, reusing the
    // element at that position if present or extending the vectors.
    _curGroupIdx = idx;
    tassert(13625704, "Too many groups seen in a single cluster", idx < kMaxGroupsPerClusterKey);

    value::MaterializedRow keyCopy(_inGroupKey);
    keyCopy.makeOwned();
    if (idx < _groupKeys.size()) {
        _groupKeys[idx] = std::move(keyCopy);
        tassert(13625705,
                "Group row width must match the number of accumulators",
                _groupAggs[idx].size() == _accumulatorList.size());
    } else {
        _groupKeys.emplace_back(std::move(keyCopy));
        _groupAggs.emplace_back(_accumulatorList.size());
    }

    for (size_t i = 0; i < _accumulatorList.size(); ++i) {
        _accumulatorList[i]->initialize(*_bytecode, _curAggAccessors[i]);
    }
}

bool StreamingAggStage::prepareGroup() {
    tassert(13625706,
            "Group key and accumulator row vectors must stay in sync",
            _groupKeys.size() == _groupAggs.size());
    tassert(13625707, "Group index must reference a valid group", _curGroupIdx < _numLiveGroups);

    readInGroupKey();
    if (_keyEq(_groupKeys[_curGroupIdx], _inGroupKey)) {
        // Same group as the previous row
        return false;
    }

    // Start the new group, check if the cluster key changed
    bool newGroup = startGroup();
    if (!newGroup) {
        // Previously seen group
        return false;
    }

    if (_noClustering) {
        // Every new group is a new cluster
        return true;
    }

    readInClusterKey();
    if (!_keyEq(_clusterKey, _inClusterKey)) {
        storeClusterKey();
        return true;
    }
    return false;
}

void StreamingAggStage::accumulate() {
    tassert(13625708,
            "Current group index must reference an allocated row",
            _curGroupIdx < _groupAggs.size());
    for (size_t i = 0; i < _accumulatorList.size(); ++i) {
        _accumulatorList[i]->accumulate(*_bytecode, _curAggAccessors[i]);
    }
}

bool StreamingAggStage::emitNext() {
    // Emit rows as long as there are values in [_emitIdx, _numLiveGroups)
    if (_emitIdx >= _numLiveGroups) {
        return false;
    }

    // After swapping the group into _outKey and _outAggAccessors, reset the buffers to be reused by
    // future groups. Clearing the agg row is required for correctness, as accumulator initializers
    // aren't guaranteed to overwrite leftover state, while clearing the key only releases unused
    // memory, and the key will be overwritten when initialized.

    auto& outKey = _groupKeys[_emitIdx];
    auto& outAgg = _groupAggs[_emitIdx];
    for (size_t i = 0; i < _accumulatorList.size(); ++i) {
        value::MaterializedSingleRowAccessor acc(outAgg, i);
        _accumulatorList[i]->finalize(*_bytecode, acc);
        _outAggAccessors[i].reset(outAgg.copyOrMoveValue(i));
        outAgg.reset(i, value::TagValueView::nothing());
    }
    swap(_outKey, outKey);
    for (size_t i = 0; i < _keySlots.size(); ++i) {
        outKey.reset(i, value::TagValueView::nothing());
    }
    if (++_emitIdx == _numLiveGroups) {
        _emitIdx = kNoGroup;
        if (!_isEOF) {
            // Finished emitting rows, move the stashed values for the next cluster into the vector.
            swap(_nextKey, _groupKeys[0]);
            swap(_nextAgg, _groupAggs[0]);
            _curGroupIdx = 0;
            _numLiveGroups = 1;
        } else {
            _numLiveGroups = 0;
        }
    }
    return true;
}

PlanState StreamingAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    // Emit all available rows before accumulating new values.
    if (emitNext()) {
        return trackPlanState(PlanState::ADVANCED);
    }

    if (MONGO_unlikely(_isEOF)) {
        return trackPlanState(PlanState::IS_EOF);
    }

    // Continue reading the current cluster until we either cross a cluster boundary or hit EOF.
    while (_children[0]->getNext() == PlanState::ADVANCED) {
        bool newCluster = prepareGroup();
        accumulate();
        if (newCluster) {
            endCluster();
            return trackPlanState(PlanState::ADVANCED);
        }
    }

    _isEOF = true;
    endCluster();
    return trackPlanState(PlanState::ADVANCED);
}

void StreamingAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _isEOF = true;
    _clusterKey = value::MaterializedRow{};
    _inClusterKey = value::MaterializedRow{};
    _inGroupKey = value::MaterializedRow{};
    _outKey = value::MaterializedRow{};
    _groupKeys.clear();
    _groupAggs.clear();
    _numLiveGroups = 0;
    _emitIdx = kNoGroup;
    _curGroupIdx = kNoGroup;
    _nextKey = value::MaterializedRow{};
    _nextAgg = value::MaterializedRow{};

    _children[0]->close();
}

std::unique_ptr<PlanStageStats> StreamingAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("keySlots", _keySlots.begin(), _keySlots.end());
        bob.append("clusterKeySlots", _clusterKeySlots.begin(), _clusterKeySlots.end());
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

    ret.emplace_back(DebugPrinter::Block("clustered [`"));
    for (size_t idx = 0; idx < _clusterKeySlots.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _clusterKeySlots[idx]);
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
    size += size_estimator::estimate(_clusterKeySlots);
    size += size_estimator::estimate(_accumulatorList);
    return size;
}
}  // namespace mongo::sbe
