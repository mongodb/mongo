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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/stages/sort.h"

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
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
                     size_t limit,
                     size_t memoryLimit,
                     bool allowDiskUse,
                     PlanNodeId planNodeId,
                     bool participateInTrialRunTracking)
    : PlanStage("sort"_sd, planNodeId, participateInTrialRunTracking),
      _obs(std::move(obs)),
      _dirs(std::move(dirs)),
      _vals(std::move(vals)),
      _allowDiskUse(allowDiskUse),
      _mergeData({0, 0}) {
    _children.emplace_back(std::move(input));

    invariant(_obs.size() == _dirs.size());

    _specificStats.limit = limit;
    _specificStats.maxMemoryUsageBytes = memoryLimit;
}

SortStage::~SortStage() {}

std::unique_ptr<PlanStage> SortStage::clone() const {
    return std::make_unique<SortStage>(_children[0]->clone(),
                                       _obs,
                                       _dirs,
                                       _vals,
                                       _specificStats.limit,
                                       _specificStats.maxMemoryUsageBytes,
                                       _allowDiskUse,
                                       _commonStats.nodeId,
                                       _participateInTrialRunTracking);
}

void SortStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    size_t counter = 0;
    // Process order by fields.
    for (auto& slot : _obs) {
        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        auto [it, inserted] =
            _outAccessors.emplace(slot,
                                  std::make_unique<value::MaterializedRowKeyAccessor<SorterData*>>(
                                      _mergeDataIt, counter));
        ++counter;
        uassert(4822812, str::stream() << "duplicate field: " << slot, inserted);
    }

    counter = 0;
    // Process value fields.
    for (auto& slot : _vals) {
        _inValueAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        auto [it, inserted] = _outAccessors.emplace(
            slot,
            std::make_unique<value::MaterializedRowValueAccessor<SorterData*>>(_mergeDataIt,
                                                                               counter));
        ++counter;
        uassert(4822813, str::stream() << "duplicate field: " << slot, inserted);
    }
}

value::SlotAccessor* SortStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
        return it->second.get();
    }

    return ctx.getAccessor(slot);
}

void SortStage::makeSorter() {
    SortOptions opts;
    opts.tempDir = storageGlobalParams.dbpath + "/_tmp";
    opts.maxMemoryUsageBytes = _specificStats.maxMemoryUsageBytes;
    opts.extSortAllowed = _allowDiskUse;
    opts.limit =
        _specificStats.limit != std::numeric_limits<size_t>::max() ? _specificStats.limit : 0;
    opts.moveSortedDataIntoIterator = true;

    auto comp = [&](const SorterData& lhs, const SorterData& rhs) {
        auto size = lhs.first.size();
        auto& left = lhs.first;
        auto& right = rhs.first;
        for (size_t idx = 0; idx < size; ++idx) {
            auto [lhsTag, lhsVal] = left.getViewOfValue(idx);
            auto [rhsTag, rhsVal] = right.getViewOfValue(idx);
            auto [tag, val] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);

            auto result = value::bitcastTo<int32_t>(val);
            if (result) {
                return _dirs[idx] == value::SortDirection::Descending ? -result : result;
            }
        }

        return 0;
    };

    _sorter.reset(Sorter<value::MaterializedRow, value::MaterializedRow>::make(opts, comp, {}));
    _mergeIt.reset();
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

void SortStage::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    invariant(_opCtx);
    _commonStats.opens++;
    _children[0]->open(reOpen);

    makeSorter();

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow keys{_inKeyAccessors.size()};
        value::MaterializedRow vals{_inValueAccessors.size()};

        size_t idx = 0;
        for (auto accessor : _inKeyAccessors) {
            auto [tag, val] = accessor->getViewOfValue();
            auto [cTag, cVal] = copyValue(tag, val);
            keys.reset(idx++, true, cTag, cVal);
        }

        idx = 0;
        for (auto accessor : _inValueAccessors) {
            auto [tag, val] = accessor->getViewOfValue();
            auto [cTag, cVal] = copyValue(tag, val);
            vals.reset(idx++, true, cTag, cVal);
        }

        _sorter->emplace(std::move(keys), std::move(vals));

        if (_tracker && _tracker->trackProgress<TrialRunTracker::kNumResults>(1)) {
            // If we either hit the maximum number of document to return during the trial run, or
            // if we've performed enough physical reads, stop populating the sort heap and bail out
            // from the trial run by raising a special exception to signal a runtime planner that
            // this candidate plan has completed its trial run early. Note that the sort stage is a
            // blocking operation and until all documents are loaded from the child stage and
            // sorted, the control is not returned to the runtime planner, so an raising this
            // special is mechanism to stop the trial run without affecting the plan stats of the
            // higher level stages.
            _tracker = nullptr;
            _children[0]->close();
            uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit in sort");
        }
    }

    _specificStats.totalDataSizeBytes += _sorter->totalDataSizeSorted();
    _mergeIt.reset(_sorter->done());
    _specificStats.spills += _sorter->stats().spilledRanges();
    _specificStats.keysSorted += _sorter->numSorted();
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(_opCtx);
    metricsCollector.incrementKeysSorted(_sorter->numSorted());
    metricsCollector.incrementSorterSpills(_sorter->stats().spilledRanges());

    _children[0]->close();
}

PlanState SortStage::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    // When the sort spilled data to disk then read back the sorted runs.
    if (_mergeIt && _mergeIt->more()) {
        _mergeData = _mergeIt->next();

        return trackPlanState(PlanState::ADVANCED);
    } else {
        return trackPlanState(PlanState::IS_EOF);
    }
}

void SortStage::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();
    _mergeIt.reset();
    _sorter.reset();
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

    if (_specificStats.limit != std::numeric_limits<size_t>::max()) {
        ret.emplace_back(std::to_string(_specificStats.limit));
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
    return size;
}
}  // namespace sbe
}  // namespace mongo
