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
                     TrialRunProgressTracker* tracker,
                     PlanNodeId planNodeId)
    : PlanStage("sort"_sd, planNodeId),
      _obs(std::move(obs)),
      _dirs(std::move(dirs)),
      _vals(std::move(vals)),
      _allowDiskUse(allowDiskUse),
      _mergeData({0, 0}),
      _tracker(tracker) {
    _children.emplace_back(std::move(input));

    invariant(_obs.size() == _dirs.size());

    _specificStats.limit = limit;
    _specificStats.maxMemoryUsageBytes = memoryLimit;
}

SortStage ::~SortStage() {}

std::unique_ptr<PlanStage> SortStage::clone() const {
    return std::make_unique<SortStage>(_children[0]->clone(),
                                       _obs,
                                       _dirs,
                                       _vals,
                                       _specificStats.limit,
                                       _specificStats.maxMemoryUsageBytes,
                                       _allowDiskUse,
                                       _tracker,
                                       _commonStats.nodeId);
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

void SortStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);

    makeSorter();

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        value::MaterializedRow keys{_inKeyAccessors.size()};
        value::MaterializedRow vals{_inValueAccessors.size()};

        size_t idx = 0;
        for (auto accesor : _inKeyAccessors) {
            auto [tag, val] = accesor->copyOrMoveValue();
            keys.reset(idx++, true, tag, val);
        }

        idx = 0;
        for (auto accesor : _inValueAccessors) {
            auto [tag, val] = accesor->copyOrMoveValue();
            vals.reset(idx++, true, tag, val);
        }

        // TODO SERVER-51815: count total mem usage for specificStats.
        _sorter->emplace(std::move(keys), std::move(vals));

        if (_tracker && _tracker->trackProgress<TrialRunProgressTracker::kNumResults>(1)) {
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
            uasserted(ErrorCodes::QueryTrialRunCompleted, "Trial run early exit");
        }
    }

    _mergeIt.reset(_sorter->done());
    _specificStats.wasDiskUsed = _specificStats.wasDiskUsed || _sorter->usedDisk();

    _children[0]->close();
}

PlanState SortStage::getNext() {
    // When the sort spilled data to disk then read back the sorted runs.
    if (_mergeIt && _mergeIt->more()) {
        _mergeData = _mergeIt->next();

        return trackPlanState(PlanState::ADVANCED);
    } else {
        return trackPlanState(PlanState::IS_EOF);
    }
}

void SortStage::close() {
    _commonStats.closes++;
    _mergeIt.reset();
    _sorter.reset();
}

std::unique_ptr<PlanStageStats> SortStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->specific = std::make_unique<SortStats>(_specificStats);
    ret->children.emplace_back(_children[0]->getStats());
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
}  // namespace sbe
}  // namespace mongo
