// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/unique.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/stats/counters.h"

#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

namespace mongo {
namespace sbe {
using namespace std::literals::string_view_literals;
namespace {

template <typename T, typename H, typename E>
size_t estimateSetSizeBytes(const absl::flat_hash_set<T, H, E>& hashSet) {
    // from https://abseil.io/docs/cpp/guides/container#abslflat_hash_map-and-abslflat_hash_set
    return (sizeof(T) + 1) * hashSet.bucket_count();
}

uint64_t estimateRowSizeBytes(const value::MaterializedRow& row) {
    // MaterializedRow is a dynamic array of values, tags, and "owned" bools.
    size_t valueAndTags =
        row.size() * (sizeof(value::Value) + sizeof(value::TypeTags) + sizeof(bool));
    // This call to estimate() computes any additional memory needed for non-shallow types.
    return valueAndTags + size_estimator::estimate(row);
}

}  // namespace
UniqueStage::UniqueStage(std::unique_ptr<PlanStage> input,
                         value::SlotVector keys,
                         PlanNodeId planNodeId,
                         bool participateInTrialRunTracking)
    : PlanStage("unique"sv, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _keySlots(keys),
      _dedupReporter(OperationMemoryUsageTracker::createDeduplicatorReporter(
          [](int64_t deduplicatedBytes, int64_t deduplicatedRecords) {
              uniqueCounters.incrementPerDeduplication(deduplicatedBytes, deduplicatedRecords);
          },
          internalQueryMaxWriteToServerStatusMemoryUsageBytes.loadRelaxed())) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> UniqueStage::clone() const {
    return std::make_unique<UniqueStage>(
        _children[0]->clone(), _keySlots, _commonStats.nodeId, participateInTrialRunTracking());
}

void UniqueStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    for (auto&& keySlot : _keySlots) {
        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, keySlot));
    }

    _memoryTracker = OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForSBE(
        _opCtx, loadMemoryLimit(StageMemoryLimit::SBEUniqueStageMaxMemoryBytes));
}

value::SlotAccessor* UniqueStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _children[0]->getAccessor(ctx, slot);
}

void UniqueStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    ++_commonStats.opens;

    if (reOpen) {
        _seen.clear();
        _prevSeenSizeBytes = 0;
        _memoryTracker->set(0);
    }
    _children[0]->open(reOpen);
}

PlanState UniqueStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow key{_inKeyAccessors.size()};
        size_t idx = 0;
        for (auto& accessor : _inKeyAccessors) {
            auto [tag, val] = accessor->getViewOfValue();
            key.reset(idx++, false, tag, val);
        }

        ++_specificStats.dupsTested;
        auto [it, inserted] = _seen.emplace(std::move(key));
        if (inserted) {
            const_cast<value::MaterializedRow&>(*it).makeOwned();
            size_t newSeenSizeBytes = estimateSetSizeBytes(_seen);
            _memoryTracker->add((newSeenSizeBytes - _prevSeenSizeBytes) +
                                estimateRowSizeBytes(*it));
            _dedupReporter.add((newSeenSizeBytes - _prevSeenSizeBytes) + estimateRowSizeBytes(*it));
            _prevSeenSizeBytes = newSeenSizeBytes;
            uassert(11130301,
                    "Exceeded memory limit in record id deduplicator for unique stage",
                    _memoryTracker->withinMemoryLimit());
            return trackPlanState(PlanState::ADVANCED);
        } else {
            // This row has been seen already, so we skip it.
            ++_specificStats.dupsDropped;
        }
    }

    return trackPlanState(PlanState::IS_EOF);
}

void UniqueStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _seen.clear();
    _memoryTracker->set(0);
    _prevSeenSizeBytes = 0;
    _specificStats.peakTrackedMemBytes = _memoryTracker->peakTrackedMemoryBytes();

    _children[0]->close();
}

std::unique_ptr<PlanStageStats> UniqueStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<UniqueStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("dupsTested", static_cast<long long>(_specificStats.dupsTested));
        bob.appendNumber("dupsDropped", static_cast<long long>(_specificStats.dupsDropped));
        bob.append("keySlots", _keySlots.begin(), _keySlots.end());
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(_specificStats.peakTrackedMemBytes));
        }
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* UniqueStage::getSpecificStats() const {
    return &_specificStats;
}

void UniqueStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                               DebugPrintInfo& debugPrintInfo) const {
    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _keySlots.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _keySlots[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t UniqueStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size_estimator::estimate(_keySlots);
    size += size_estimator::estimate(_specificStats);
    return size;
}

UniqueRoaringStage::UniqueRoaringStage(std::unique_ptr<PlanStage> input,
                                       value::SlotId key,
                                       PlanNodeId planNodeId,
                                       bool participateInTrialRunTracking)
    : PlanStage(
          "unique_roaring"sv, nullptr /* yieldPolicy */, planNodeId, participateInTrialRunTracking),
      _keySlot(key),
      _seen(static_cast<size_t>(internalRoaringBitmapsThreshold.load()),
            static_cast<size_t>(internalRoaringBitmapsBatchSize.load()),
            static_cast<uint64_t>(internalRoaringBitmapsThreshold.load() /
                                  internalRoaringBitmapsMinimalDensity.load())),
      _dedupReporter(OperationMemoryUsageTracker::createDeduplicatorReporter(
          [](int64_t deduplicatedBytes, int64_t deduplicatedRecords) {
              uniqueRoaringCounters.incrementPerDeduplication(deduplicatedBytes,
                                                              deduplicatedRecords);
          },
          internalQueryMaxWriteToServerStatusMemoryUsageBytes.loadRelaxed())) {
    _children.emplace_back(std::move(input));
}

std::unique_ptr<PlanStage> UniqueRoaringStage::clone() const {
    return std::make_unique<UniqueRoaringStage>(
        _children[0]->clone(), _keySlot, _commonStats.nodeId, participateInTrialRunTracking());
}

void UniqueRoaringStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);
    _inKeyAccessor = _children[0]->getAccessor(ctx, _keySlot);
    _memoryTracker = OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForSBE(
        _opCtx, loadMemoryLimit(StageMemoryLimit::SBEUniqueStageMaxMemoryBytes));
}

value::SlotAccessor* UniqueRoaringStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _children[0]->getAccessor(ctx, slot);
}

void UniqueRoaringStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));
    ++_commonStats.opens;

    if (reOpen) {
        _seen.clear();
        _memoryTracker->set(0);
        _prevSeenSizeBytes = 0;
    }
    _children[0]->open(reOpen);
}

PlanState UniqueRoaringStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        auto [tag, val] = _inKeyAccessor->getViewOfValue();

        int64_t roaringVal;
        switch (tag) {
            case value::TypeTags::NumberInt32: {
                roaringVal = value::bitcastTo<int32_t>(val);
                break;
            }
            case value::TypeTags::NumberInt64: {
                roaringVal = value::bitcastTo<int64_t>(val);
                break;
            }
            case value::TypeTags::RecordId: {
                auto recordId = value::getRecordIdView(val);
                tassert(
                    9762501,
                    "unique_roaring stage encountered a record id that is not formatted as long",
                    recordId->isLong());
                roaringVal = recordId->getLong();
                break;
            }
            default: {
                auto tag_ = tag;
                tasserted(9762500,
                          str::stream()
                              << "unique_roaring stage encountered unexpected SBE value type: "
                              << tag_);
            }
        }

        ++_specificStats.dupsTested;
        auto inserted = _seen.addChecked(roaringVal);
        if (inserted) {
            size_t newSeenSizeBytes = _seen.getApproximateSize();
            _memoryTracker->add(newSeenSizeBytes - _prevSeenSizeBytes);
            _dedupReporter.add(newSeenSizeBytes - _prevSeenSizeBytes);
            _prevSeenSizeBytes = newSeenSizeBytes;
            uassert(11130300,
                    "Exceeded memory limit in record id deduplicator for unique_roaring stage",
                    _memoryTracker->withinMemoryLimit());
            return trackPlanState(PlanState::ADVANCED);
        } else {
            // This row has been seen already, so we skip it.
            ++_specificStats.dupsDropped;
        }
    }

    return trackPlanState(PlanState::IS_EOF);
}

void UniqueRoaringStage::close() {
    auto optTimer(getOptTimer(_opCtx));
    trackClose();

    _seen.clear();
    _memoryTracker->set(0);
    _prevSeenSizeBytes = 0;
    _specificStats.peakTrackedMemBytes = _memoryTracker->peakTrackedMemoryBytes();

    _children[0]->close();
}

std::unique_ptr<PlanStageStats> UniqueRoaringStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<UniqueStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("dupsTested", static_cast<long long>(_specificStats.dupsTested));
        bob.appendNumber("dupsDropped", static_cast<long long>(_specificStats.dupsDropped));
        bob.append("keySlot", _keySlot);
        if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
            bob.appendNumber("peakTrackedMemBytes",
                             static_cast<long long>(_specificStats.peakTrackedMemBytes));
        }
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* UniqueRoaringStage::getSpecificStats() const {
    return &_specificStats;
}

void UniqueRoaringStage::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                      DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addIdentifier(ret, _keySlot);
    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
}

size_t UniqueRoaringStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_specificStats);
    return size;
}
}  // namespace sbe
}  // namespace mongo
