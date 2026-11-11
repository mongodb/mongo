// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/compact_hash_agg.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/hashagg_base.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;

namespace {
CompactStateLayout buildLayout(
    const std::vector<std::unique_ptr<CompactHashAggAccumulator>>& accumulators) {
    CompactStateLayout layout;
    for (const auto& accumulator : accumulators) {
        accumulator->addState(layout);
    }
    return layout;
}
}  // namespace

CompactHashAggStage::CompactHashAggStage(
    std::unique_ptr<PlanStage> input,
    value::SlotVector gbs,
    value::SlotVector gbsOut,
    std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators,
    boost::optional<value::SlotId> collatorSlot,
    PlanYieldPolicySBE* yieldPolicy,
    PlanNodeId planNodeId,
    bool participateInTrialRunTracking)
    : HashAggBaseStage("compact_group"sv,
                       yieldPolicy,
                       planNodeId,
                       /*_collatorAccessor=*/nullptr,
                       participateInTrialRunTracking,
                       false /* allowDiskUse; the compact stage does not spill */,
                       /*forceIncreasedSpilling=*/false),
      _gbs(std::move(gbs)),
      _gbsOut(std::move(gbsOut)),
      _accumulators(std::move(accumulators)),
      _layout(buildLayout(_accumulators)),
      _collatorSlot(collatorSlot) {
    _children.emplace_back(std::move(input));
    tassert(13913000,
            "CompactHashAggStage needs one output slot for each group-by slot",
            _gbsOut.size() == _gbs.size());
}

std::unique_ptr<PlanStage> CompactHashAggStage::clone() const {
    std::vector<std::unique_ptr<CompactHashAggAccumulator>> accumulators;
    for (const auto& accumulator : _accumulators) {
        accumulators.push_back(accumulator->clone());
    }
    return std::make_unique<CompactHashAggStage>(_children[0]->clone(),
                                                 _gbs,
                                                 _gbsOut,
                                                 std::move(accumulators),
                                                 _collatorSlot,
                                                 _yieldPolicy,
                                                 _commonStats.nodeId,
                                                 participateInTrialRunTracking());
}

void CompactHashAggStage::prepare(CompileCtx& ctx) {
    _bytecode = ctx.getByteCode();
    _children[0]->prepare(ctx);

    if (_collatorSlot) {
        _collatorAccessor = getAccessor(ctx, *_collatorSlot);
        tassert(13605501,
                "collator accessor should exist if collator slot provided to CompactHashAggStage",
                _collatorAccessor != nullptr);
    }

    value::SlotSet dupCheck;
    auto throwIfDupSlot = [&dupCheck](value::SlotId slot) {
        auto [_, inserted] = dupCheck.emplace(slot);
        tassert(13605503, "duplicate slot id", inserted);
    };

    size_t keyIdx = 0;
    for (size_t i = 0; i < _gbs.size(); ++i) {
        auto slot = _gbs[i];
        throwIfDupSlot(slot);
        throwIfDupSlot(_gbsOut[i]);

        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));

        // Construct an accessor for obtaining the key value from the hash table '_ht'.
        _outKeyAccessors.emplace_back(std::make_unique<CompactKeyAccessor>(_htIt, keyIdx));

        _outAccessors[_gbsOut[i]] = _outKeyAccessors.back().get();

        ++keyIdx;
    }

    for (const auto& accumulator : _accumulators) {
        throwIfDupSlot(accumulator->getOutSlot());
        _outAggAccessors.emplace_back(std::make_unique<value::OwnedValueAccessor>());
        _outAccessors[accumulator->getOutSlot()] = _outAggAccessors.back().get();
        ctx.root = this;
        accumulator->prepare(ctx);
    }

    _compiled = true;

    _memoryTracker = OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
        _opCtx, loadMemoryLimit(StageMemoryLimit::QuerySBEAggApproxMemoryUseInBytesBeforeSpill));
}

value::SlotAccessor* CompactHashAggStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_compiled) {
        if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
            return it->second;
        }
    } else {
        return _children[0]->getAccessor(ctx, slot);
    }

    return ctx.getAccessor(slot);
}

void CompactHashAggStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;

    tassert(13605505, "Expecting _opCtx to be populated", _opCtx);
    _children[0]->open(_childOpened);
    _childOpened = true;
    if (_collatorAccessor) {
        auto [tag, collatorVal] = _collatorAccessor->getViewOfValue();
        uassert(
            13605502, "collatorSlot must be of collator type", tag == value::TypeTags::collator);
        auto collatorView = value::getCollatorView(collatorVal);
        const value::MaterializedRowHasher hasher(collatorView);
        _keyEq = value::MaterializedRowEq(collatorView);
        _ht.emplace(_layout, hasher, _keyEq);
    } else {
        _ht.emplace(_layout);
    }

    MemoryCheckData memoryCheckData;

    value::MaterializedRow key{_inKeyAccessors.size()};

    // If the group-by key is empty, we aggregate into a single row. In this case, avoid hash
    // table lookups for each child document.
    bool firstDoc = true;
    const bool groupByListHasSlots = !_inKeyAccessors.empty();
    while (_children[0]->getNext() == PlanState::ADVANCED) {
        if (groupByListHasSlots || firstDoc) {
            // Copy keys in order to do the lookup.
            size_t idx = 0;
            for (auto& p : _inKeyAccessors) {
                key.reset(idx++, p->getViewOfValue());
            }

            _htIt = _ht->findOrInsert(key);
            firstDoc = false;
        }

        for (const auto& accumulator : _accumulators) {
            accumulator->accumulate(*_bytecode, _htIt->second);
        }

        // If the group-by key is empty we will only ever aggregate into a single row so no
        // sense in checking memory usage.
        if (groupByListHasSlots) {
            // Estimates how much memory is being used. If we estimate that the hash table
            // exceeds the allotted memory budget, the base class fails the query with
            // 'QueryExceededMemoryLimitNoDiskUseAllowed' since the compact stage does not
            // spill.
            checkMemoryUsageAndSpillIfNecessary(memoryCheckData);
        }
    }  // while child's getNext advanced

    _children[0]->close();
    _childOpened = false;

    _htIt = _ht->end();
}

PlanState CompactHashAggStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));
    checkForInterruptAndYield(_opCtx);

    setIteratorToNextRecord();

    if (_htIt == _ht->end()) {
        // The hash table has been drained, so we're done.
        return trackPlanState(PlanState::IS_EOF);
    }

    for (size_t idx = 0; idx < _accumulators.size(); ++idx) {
        _accumulators[idx]->finalize(_htIt->second, *_outAggAccessors[idx]);
    }
    return trackPlanState(PlanState::ADVANCED);
}

std::unique_ptr<PlanStageStats> CompactHashAggStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<HashAggStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.append("groupBySlots", _gbs.begin(), _gbs.end());
        bob.append("groupByOutSlots", _gbsOut.begin(), _gbsOut.end());
        if (!_accumulators.empty()) {
            DebugPrinter printer;
            BSONObjBuilder exprBuilder(bob.subobjStart("expressions"));
            for (const auto& accumulator : _accumulators) {
                exprBuilder.append(str::stream() << accumulator->getOutSlot(),
                                   printer.print(accumulator->debugPrint()));
            }
        }

        // Spilling stats.
        bob.appendBool("usedDisk", _specificStats.usedDisk);
        bob.appendNumber("spills",
                         static_cast<long long>(_specificStats.spillingStats.getSpills()));
        bob.appendNumber("spilledBytes",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledBytes()));
        bob.appendNumber("spilledRecords",
                         static_cast<long long>(_specificStats.spillingStats.getSpilledRecords()));
        bob.appendNumber(
            "spilledDataStorageSize",
            static_cast<long long>(_specificStats.spillingStats.getSpilledDataStorageSize()));

        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(_specificStats.peakTrackedMemBytes));
        }

        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* CompactHashAggStage::getSpecificStats() const {
    return &_specificStats;
}

HashAggStats* CompactHashAggStage::getHashAggStats() {
    return &_specificStats;
}

void CompactHashAggStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _ht = boost::none;
    resetOutputs();

    _children[0]->close();
    _childOpened = false;

    _memoryTracker.value().set(0);
    _specificStats.peakTrackedMemBytes = _memoryTracker.value().peakTrackedMemoryBytes();
}

void CompactHashAggStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                       DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _gbs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _gbs[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _gbs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _gbsOut[idx]);
        ret.emplace_back("=");
        DebugPrinter::addIdentifier(ret, _gbs[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _accumulators.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _accumulators[idx]->getOutSlot());
        ret.emplace_back(DebugPrinter::Block("="));
        DebugPrinter::addBlocks(ret, _accumulators[idx]->debugPrint());
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    if (_collatorSlot) {
        DebugPrinter::addIdentifier(ret, *_collatorSlot);
    }

    DebugPrinter::addNewLine(ret);

    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t CompactHashAggStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_gbs);
    size += size_estimator::estimate(_gbsOut);
    size += size_estimator::estimate(_accumulators);
    size += _layout.heapSize();
    return size;
}

void CompactHashAggStage::resetOutputs() {
    for (auto& accessor : _outAggAccessors) {
        accessor->reset();
    }
}
}  // namespace sbe
}  // namespace mongo
