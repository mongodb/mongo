/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/container/inlined_vector.h>
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <string>
#include <system_error>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace {
std::string nextFileName() {
    static mongo::AtomicWord<unsigned> sortExecutorFileCounter;
    return "extsort-sort-sbe." + std::to_string(sortExecutorFileCounter.fetchAndAdd(1));
}
}  // namespace

#include "mongo/db/sorter/sorter.cpp"

namespace mongo {
namespace sbe {
SortStage::SortStage(std::unique_ptr<PlanStage> input,
                     value::SlotVector obs,
                     std::vector<value::SortDirection> dirs,
                     value::SlotVector vals,
                     std::unique_ptr<EExpression> limit,
                     size_t memoryLimit,
                     bool allowDiskUse,
                     PlanYieldPolicy* yieldPolicy,
                     PlanNodeId planNodeId,
                     bool participateInTrialRunTracking)
    : PlanStage("sort"_sd, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _obs(std::move(obs)),
      _dirs(std::move(dirs)),
      _vals(std::move(vals)),
      _allowDiskUse(allowDiskUse),
      _limitExpr(std::move(limit)) {
    _children.emplace_back(std::move(input));

    invariant(_obs.size() == _dirs.size());

    _specificStats.maxMemoryUsageBytes = memoryLimit;
}

SortStage::~SortStage() {}

void SortStage::prepare(CompileCtx& ctx) {
    _stageImpl = makeStageImpl();
    _stageImpl->prepare(ctx);
}

value::SlotAccessor* SortStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    return _stageImpl->getAccessor(ctx, slot);
}

void SortStage::open(bool reOpen) {
    _stageImpl->open(reOpen);
}

PlanState SortStage::getNext() {
    return _stageImpl->getNext();
}

void SortStage::close() {
    return _stageImpl->close();
}

std::unique_ptr<PlanStage> SortStage::clone() const {
    return std::make_unique<SortStage>(_children[0]->clone(),
                                       _obs,
                                       _dirs,
                                       _vals,
                                       _limitExpr ? _limitExpr->clone() : nullptr,
                                       _specificStats.maxMemoryUsageBytes,
                                       _allowDiskUse,
                                       _yieldPolicy,
                                       _commonStats.nodeId,
                                       _participateInTrialRunTracking);
}

template <typename KeyType, typename ValueType>
std::unique_ptr<SortStage::SortIface> SortStage::makeStageImplInternal() {
    return std::make_unique<SortImpl<KeyType, ValueType>>(*this);
}

template <typename KeyType>
std::unique_ptr<SortStage::SortIface> SortStage::makeStageImplInternal(size_t valueSize) {
    switch (valueSize) {
        case 1:
            return makeStageImplInternal<KeyType, value::FixedSizeRow<1>>();
        default:
            return makeStageImplInternal<KeyType, value::MaterializedRow>();
    }
}

std::unique_ptr<SortStage::SortIface> SortStage::makeStageImplInternal(size_t keySize,
                                                                       size_t valueSize) {
    switch (keySize) {
        case 1:
            return makeStageImplInternal<value::FixedSizeRow<1>>(valueSize);
        case 2:
            return makeStageImplInternal<value::FixedSizeRow<2>>(valueSize);
        case 3:
            return makeStageImplInternal<value::FixedSizeRow<3>>(valueSize);
        default:
            return makeStageImplInternal<value::MaterializedRow>(valueSize);
    }
}

std::unique_ptr<SortStage::SortIface> SortStage::makeStageImpl() {
    return makeStageImplInternal(_obs.size(), _vals.size());
}

void SortStage::doDetachFromTrialRunTracker() {
    _tracker = nullptr;
}

PlanStage::TrialRunTrackerAttachResultMask SortStage::doAttachToTrialRunTracker(
    TrialRunTracker* tracker, TrialRunTrackerAttachResultMask childrenAttachResult) {
    // The SortStage only tracks the "numResults" metric when it is the most deeply nested blocking
    // stage.
    if (!(childrenAttachResult & TrialRunTrackerAttachResultFlags::AttachedToBlockingStage)) {
        _tracker = tracker;
    }

    // Return true to indicate that the tracker is attached to a blocking stage: either this stage
    // or one of its descendent stages.
    return childrenAttachResult | TrialRunTrackerAttachResultFlags::AttachedToBlockingStage;
}

std::unique_ptr<PlanStageStats> SortStage::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<SortStats>(_specificStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("memLimit", static_cast<long long>(_specificStats.maxMemoryUsageBytes));
        bob.appendNumber("totalDataSizeSorted",
                         static_cast<long long>(_specificStats.totalDataSizeBytes));
        bob.appendBool("usedDisk", _specificStats.spills > 0);
        bob.appendNumber("spills", static_cast<long long>(_specificStats.spills));
        bob.appendNumber("spilledDataStorageSize",
                         static_cast<long long>(_specificStats.spilledDataStorageSize));

        BSONObjBuilder childrenBob(bob.subobjStart("orderBySlots"));
        for (size_t idx = 0; idx < _obs.size(); ++idx) {
            childrenBob.append(str::stream() << _obs[idx],
                               _dirs[idx] == sbe::value::SortDirection::Ascending ? "asc" : "desc");
        }
        childrenBob.doneFast();
        bob.append("outputSlots", _vals.begin(), _vals.end());
        ret->debugInfo = bob.obj();
    }

    ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    return ret;
}

const SpecificStats* SortStage::getSpecificStats() const {
    return &_specificStats;
}

std::vector<DebugPrinter::Block> SortStage::debugPrint() const {
    auto ret = PlanStage::debugPrint();

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _obs.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _obs[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _dirs.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret,
                                    _dirs[idx] == value::SortDirection::Ascending ? "asc" : "desc");
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _vals.size(); ++idx) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }

        DebugPrinter::addIdentifier(ret, _vals[idx]);
    }
    ret.emplace_back("`]");

    if (_limitExpr) {
        DebugPrinter::addBlocks(ret, _limitExpr->debugPrint());
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}

size_t SortStage::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_obs);
    size += size_estimator::estimate(_dirs);
    size += size_estimator::estimate(_vals);
    size += size_estimator::estimate(_specificStats);
    size += _limitExpr ? _limitExpr->estimateSize() : 0;
    return size;
}

template <typename KeyRow, typename ValueRow>
SortStage::SortImpl<KeyRow, ValueRow>::SortImpl(SortStage& stage) : _stage(stage), _mergeData() {}

template <typename KeyRow, typename ValueRow>
SortStage::SortImpl<KeyRow, ValueRow>::~SortImpl() {}

template <typename KeyRow, typename ValueRow>
void SortStage::SortImpl<KeyRow, ValueRow>::prepare(CompileCtx& ctx) {
    _stage._children[0]->prepare(ctx);

    size_t counter = 0;
    // Process order by fields.
    for (auto& slot : _stage._obs) {
        _inKeyAccessors.emplace_back(_stage._children[0]->getAccessor(ctx, slot));
        auto [it, inserted] =
            _outAccessors.emplace(slot,
                                  std::make_unique<value::MaterializedRowKeyAccessor<SorterData*>>(
                                      _mergeDataIt, counter));
        ++counter;
        uassert(4822812, str::stream() << "duplicate field: " << slot, inserted);
    }

    counter = 0;
    // Process value fields.
    for (auto& slot : _stage._vals) {
        _inValueAccessors.emplace_back(_stage._children[0]->getAccessor(ctx, slot));
        auto [it, inserted] = _outAccessors.emplace(
            slot,
            std::make_unique<value::MaterializedRowValueAccessor<SorterData*>>(_mergeDataIt,
                                                                               counter));
        ++counter;
        uassert(4822813, str::stream() << "duplicate field: " << slot, inserted);
    }

    if (_stage._limitExpr) {
        _limitCode = _stage._limitExpr->compile(ctx);
    }
}

template <typename KeyRow, typename ValueRow>
value::SlotAccessor* SortStage::SortImpl<KeyRow, ValueRow>::getAccessor(CompileCtx& ctx,
                                                                        value::SlotId slot) {
    if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
        return it->second.get();
    }

    return ctx.getAccessor(slot);
}

template <typename KeyRow, typename ValueRow>
int64_t SortStage::SortImpl<KeyRow, ValueRow>::runLimitCode() {
    auto [owned, tag, val] = vm::ByteCode{}.run(_limitCode.get());
    value::ValueGuard guard{owned, tag, val};
    tassert(8349205, "Limit code returned unexpected value", tag == value::TypeTags::NumberInt64);
    return value::bitcastTo<size_t>(val);
}

template <typename KeyRow, typename ValueRow>
void SortStage::SortImpl<KeyRow, ValueRow>::makeSorter() {
    SortOptions opts;
    opts.tempDir = storageGlobalParams.dbpath + "/_tmp";
    opts.maxMemoryUsageBytes = _stage._specificStats.maxMemoryUsageBytes;
    opts.extSortAllowed = _stage._allowDiskUse;
    opts.limit = _stage._specificStats.limit != std::numeric_limits<size_t>::max()
        ? _stage._specificStats.limit
        : 0;
    opts.moveSortedDataIntoIterator = true;
    if (_stage._allowDiskUse) {
        _stage._sorterFileStats = std::make_unique<SorterFileStats>(nullptr);
        opts.sorterFileStats = _stage._sorterFileStats.get();
    }

    auto comp = [&](const KeyRow& lhs, const KeyRow& rhs) {
        auto size = lhs.size();
        for (size_t idx = 0; idx < size; ++idx) {
            auto [lhsTag, lhsVal] = lhs.getViewOfValue(idx);
            auto [rhsTag, rhsVal] = rhs.getViewOfValue(idx);
            auto [tag, val] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
            uassert(7086700, "Invalid comparison result", tag == value::TypeTags::NumberInt32);
            auto result = value::bitcastTo<int32_t>(val);
            if (result) {
                return _stage._dirs[idx] == value::SortDirection::Descending ? -result : result;
            }
        }

        return 0;
    };

    _sorter.reset(Sorter<KeyRow, ValueRow>::make(opts, comp, {}));
    _mergeIt.reset();
}

template <typename KeyRow, typename ValueRow>
void SortStage::SortImpl<KeyRow, ValueRow>::open(bool reOpen) {
    auto optTimer(_stage.getOptTimer(_stage._opCtx));

    invariant(_stage._opCtx);
    _stage._commonStats.opens++;
    _stage._children[0]->open(reOpen);

    if (_limitCode) {
        _stage._specificStats.limit = runLimitCode();
    } else {
        _stage._specificStats.limit = std::numeric_limits<size_t>::max();
    }

    makeSorter();

    while (_stage._children[0]->getNext() == PlanState::ADVANCED) {
        KeyRow keys{_inKeyAccessors.size()};

        size_t idx = 0;
        for (auto accessor : _inKeyAccessors) {
            auto [tag, val] = accessor->getViewOfValue();
            keys.reset(idx++, false, tag, val);
        }

        // Do not allocate the values here, instead let the sorter decide, since the sorter may
        // decide not to store the values in the case of sort-limit.
        _sorter->emplace(std::move(keys), [&]() {
            ValueRow vals{_inValueAccessors.size()};
            size_t idx = 0;
            for (auto accessor : _inValueAccessors) {
                auto [tag, val] = accessor->getViewOfValue();
                vals.reset(idx++, false, tag, val);
            }
            return vals;
        });

        if (_stage._tracker && _stage._tracker->trackProgress<TrialRunTracker::kNumResults>(1)) {
            // If we either hit the maximum number of document to return during the trial run, or
            // if we've performed enough physical reads, stop populating the sort heap and bail out
            // from the trial run by raising a special exception to signal a runtime planner that
            // this candidate plan has completed its trial run early. Note that the sort stage is a
            // blocking operation and until all documents are loaded from the child stage and
            // sorted, the control is not returned to the runtime planner, so an raising this
            // special is mechanism to stop the trial run without affecting the plan stats of the
            // higher level stages.
            _stage._tracker = nullptr;
            _stage._children[0]->close();
            uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in sort");
        }
    }

    _stage._specificStats.totalDataSizeBytes += _sorter->stats().bytesSorted();
    _mergeIt.reset(_sorter->done());
    _stage._specificStats.spills += _sorter->stats().spilledRanges();
    _stage._specificStats.keysSorted += _sorter->stats().numSorted();
    if (_stage._sorterFileStats) {
        _stage._specificStats.spilledDataStorageSize += _stage._sorterFileStats->bytesSpilled();
    }
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_stage._opCtx);
    metricsCollector.incrementKeysSorted(_sorter->stats().numSorted());
    metricsCollector.incrementSorterSpills(_sorter->stats().spilledRanges());

    _stage._children[0]->close();
}

template <typename KeyRow, typename ValueRow>
PlanState SortStage::SortImpl<KeyRow, ValueRow>::getNext() {
    auto optTimer(_stage.getOptTimer(_stage._opCtx));
    _stage.checkForInterruptAndYield(_stage._opCtx);

    // When the sort spilled data to disk then read back the sorted runs.
    if (_mergeIt && _mergeIt->more()) {
        _mergeData = _mergeIt->next();

        return _stage.trackPlanState(PlanState::ADVANCED);
    } else {
        return _stage.trackPlanState(PlanState::IS_EOF);
    }
}

template <typename KeyRow, typename ValueRow>
void SortStage::SortImpl<KeyRow, ValueRow>::close() {
    auto optTimer(_stage.getOptTimer(_stage._opCtx));

    _stage.trackClose();
    _mergeIt.reset();
    _sorter.reset();
}

}  // namespace sbe
}  // namespace mongo
