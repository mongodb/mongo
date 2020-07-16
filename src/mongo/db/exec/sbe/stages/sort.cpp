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
#include "mongo/db/sorter/sorter.h"
#include "mongo/util/str.h"

namespace {
std::string nextFileName() {
    static mongo::AtomicWord<unsigned> sortExecutorFileCounter;
    return "extsort-sort-sbe." + std::to_string(sortExecutorFileCounter.fetchAndAdd(1));
}
}  // namespace

namespace mongo {
namespace sbe {
SortStage::SortStage(std::unique_ptr<PlanStage> input,
                     value::SlotVector obs,
                     std::vector<value::SortDirection> dirs,
                     value::SlotVector vals,
                     size_t limit,
                     size_t memoryLimit,
                     bool allowDiskUse,
                     TrialRunProgressTracker* tracker)
    : PlanStage("sort"_sd),
      _obs(std::move(obs)),
      _dirs(std::move(dirs)),
      _vals(std::move(vals)),
      _limit(limit),
      _memoryLimit(memoryLimit),
      _allowDiskUse(allowDiskUse),
      _st(value::MaterializedRowComparator{_dirs}),
      _tracker(tracker) {
    _children.emplace_back(std::move(input));

    invariant(_obs.size() == _dirs.size());
}

SortStage ::~SortStage() {}

std::unique_ptr<PlanStage> SortStage::clone() const {
    return std::make_unique<SortStage>(
        _children[0]->clone(), _obs, _dirs, _vals, _limit, _memoryLimit, _allowDiskUse, _tracker);
}

void SortStage::prepare(CompileCtx& ctx) {
    _children[0]->prepare(ctx);

    value::SlotSet dupCheck;

    size_t counter = 0;
    // Process order by fields.
    for (auto& slot : _obs) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822812, str::stream() << "duplicate field: " << slot, inserted);

        _inKeyAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        std::vector<std::unique_ptr<value::SlotAccessor>> accessors;
        accessors.emplace_back(std::make_unique<SortKeyAccessor>(_stIt, counter));
        accessors.emplace_back(std::make_unique<value::MaterializedRowKeyAccessor<SorterData*>>(
            _mergeDataIt, counter));
        _outAccessors.emplace(slot, value::SwitchAccessor{std::move(accessors)});
        ++counter;
    }

    counter = 0;
    // Process value fields.
    for (auto& slot : _vals) {
        auto [it, inserted] = dupCheck.insert(slot);
        uassert(4822813, str::stream() << "duplicate field: " << slot, inserted);

        _inValueAccessors.emplace_back(_children[0]->getAccessor(ctx, slot));
        std::vector<std::unique_ptr<value::SlotAccessor>> accessors;
        accessors.emplace_back(std::make_unique<SortValueAccessor>(_stIt, counter));
        accessors.emplace_back(std::make_unique<value::MaterializedRowValueAccessor<SorterData*>>(
            _mergeDataIt, counter));
        _outAccessors.emplace(slot, value::SwitchAccessor{std::move(accessors)});
        ++counter;
    }
}

value::SlotAccessor* SortStage::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (auto it = _outAccessors.find(slot); it != _outAccessors.end()) {
        return &it->second;
    }

    return ctx.getAccessor(slot);
}

void SortStage::open(bool reOpen) {
    _commonStats.opens++;
    _children[0]->open(reOpen);

    value::MaterializedRow keys;
    value::MaterializedRow vals;

    SortOptions opts;
    opts.tempDir = storageGlobalParams.dbpath + "/_tmp";
    std::string spillFileName = opts.tempDir + "/" + nextFileName();
    std::streampos nextSortedFileWriterOffset = 0;
    size_t memorySize = 0;

    _mergeIt.reset();
    _iters.clear();

    auto spill = [&]() {
        SortedFileWriter<value::MaterializedRow, value::MaterializedRow> writer{
            opts, spillFileName, nextSortedFileWriterOffset};

        for (auto& [k, v] : _st) {
            writer.addAlreadySorted(k, v);
        }
        _st.clear();
        memorySize = 0;

        auto iteratorPtr = writer.done();
        nextSortedFileWriterOffset = writer.getFileEndOffset();

        _iters.push_back(std::shared_ptr<SorterIterator>(iteratorPtr));
    };

    while (_children[0]->getNext() == PlanState::ADVANCED) {
        keys._fields.reserve(_inKeyAccessors.size());
        vals._fields.reserve(_inValueAccessors.size());

        for (auto accesor : _inKeyAccessors) {
            keys._fields.push_back(value::OwnedValueAccessor{});
            auto [tag, val] = accesor->copyOrMoveValue();
            keys._fields.back().reset(true, tag, val);
        }
        for (auto accesor : _inValueAccessors) {
            vals._fields.push_back(value::OwnedValueAccessor{});
            auto [tag, val] = accesor->copyOrMoveValue();
            vals._fields.back().reset(true, tag, val);
        }

        memorySize += keys.memUsageForSorter();
        memorySize += vals.memUsageForSorter();

        _st.emplace(std::move(keys), std::move(vals));
        if (_st.size() - 1 == _limit) {
            _st.erase(--_st.end());
        }

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

        // Test if we have to spill
        // TODO SERVER-49829 - topk spilling
        if (_limit == std::numeric_limits<std::size_t>::max() && memorySize > _memoryLimit) {
            uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                    str::stream()
                        << "Sort exceeded memory limit of " << _memoryLimit
                        << " bytes, but did not opt in to external sorting. Aborting operation."
                        << " Pass allowDiskUse:true to opt in.",
                    _allowDiskUse);

            spill();
        }
    }

    if (!_iters.empty()) {
        // Spill the last part that still sits in memory.
        if (!_st.empty()) {
            spill();
        }

        _mergeIt.reset(SorterIterator::merge(
            _iters, spillFileName, opts, [&](const SorterData& lhs, const SorterData& rhs) {
                for (size_t idx = 0; idx < lhs.first._fields.size(); ++idx) {
                    auto [lhsTag, lhsVal] = lhs.first._fields[idx].getViewOfValue();
                    auto [rhsTag, rhsVal] = rhs.first._fields[idx].getViewOfValue();
                    auto [tag, val] = value::compareValue(lhsTag, lhsVal, rhsTag, rhsVal);
                    invariant(tag == value::TypeTags::NumberInt32);
                    auto result = value::bitcastTo<int32_t>(val);
                    if (val) {
                        return _dirs[idx] == value::SortDirection::Descending ? -result : result;
                    }
                }

                return 0;
            }));

        // Switch all output accessors to point to the spilled data.
        for (auto&& [_, acc] : _outAccessors) {
            acc.setIndex(1);
        }
    }
    _children[0]->close();

    _stIt = _st.end();
}

PlanState SortStage::getNext() {
    // When the sort spilled data to disk then read back the sorted runs.
    if (_mergeIt) {
        if (_mergeIt->more()) {
            _mergeData = _mergeIt->next();

            return trackPlanState(PlanState::ADVANCED);
        } else {
            return trackPlanState(PlanState::IS_EOF);
        }
    }

    if (_stIt == _st.end()) {
        _stIt = _st.begin();
    } else {
        ++_stIt;
    }

    if (_stIt == _st.end()) {
        return trackPlanState(PlanState::IS_EOF);
    }

    return trackPlanState(PlanState::ADVANCED);
}

void SortStage::close() {
    _commonStats.closes++;
    _st.clear();
}

std::unique_ptr<PlanStageStats> SortStage::getStats() const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);
    ret->children.emplace_back(_children[0]->getStats());
    return ret;
}

const SpecificStats* SortStage::getSpecificStats() const {
    return nullptr;
}

std::vector<DebugPrinter::Block> SortStage::debugPrint() const {
    std::vector<DebugPrinter::Block> ret;
    DebugPrinter::addKeyword(ret, "sort");

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

    if (_limit != std::numeric_limits<size_t>::max()) {
        ret.emplace_back(std::to_string(_limit));
    }

    DebugPrinter::addNewLine(ret);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint());

    return ret;
}
}  // namespace sbe
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
