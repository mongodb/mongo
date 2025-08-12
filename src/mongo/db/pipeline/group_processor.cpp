/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/group_processor.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/sorter/sorter_file_name.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/stats/counters.h"

namespace mongo {

GroupProcessor::GroupProcessor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               int64_t maxMemoryUsageBytes)

    : GroupProcessorBase(expCtx, maxMemoryUsageBytes) {}

boost::optional<Document> GroupProcessor::getNext() {
    if (_spilled) {
        return getNextSpilled();
    } else {
        return getNextStandard();
    }
}

boost::optional<Document> GroupProcessor::getNextSpilled() {
    // We aren't streaming, and we have spilled to disk.
    if (!_sorterIterator)
        return boost::none;

    Value currentId = _firstPartOfNextGroup.first;
    const size_t numAccumulators = _accumulatedFields.size();

    // Call startNewGroup on every accumulator.
    Value expandedId = expandId(currentId);
    Document idDoc =
        expandedId.getType() == BSONType::object ? expandedId.getDocument() : Document();
    for (size_t i = 0; i < numAccumulators; ++i) {
        Value initializerValue =
            _accumulatedFields[i].expr.initializer->evaluate(idDoc, &_expCtx->variables);
        _currentAccumulators[i]->reset();
        _currentAccumulators[i]->startNewGroup(initializerValue);
    }

    while (_expCtx->getValueComparator().evaluate(currentId == _firstPartOfNextGroup.first)) {
        // Inside of this loop, _firstPartOfNextGroup is the current data being processed.
        // At loop exit, it is the first value to be processed in the next group.
        switch (numAccumulators) {  // mirrors switch in spill()
            case 1:                 // Single accumulators serialize as a single Value.
                _currentAccumulators[0]->process(_firstPartOfNextGroup.second, true);
                [[fallthrough]];
            case 0:  // No accumulators so no Values.
                break;
            default: {  // Multiple accumulators serialize as an array of Values.
                const std::vector<Value>& accumulatorStates =
                    _firstPartOfNextGroup.second.getArray();
                for (size_t i = 0; i < numAccumulators; i++) {
                    _currentAccumulators[i]->process(accumulatorStates[i], true);
                }
            }
        }

        if (!_sorterIterator->more()) {
            _sorterIterator.reset();
            break;
        }

        _firstPartOfNextGroup = _sorterIterator->next();
    }

    return makeDocument(currentId, _currentAccumulators);
}

boost::optional<Document> GroupProcessor::getNextStandard() {
    if (_groupsIterator == _groups.end()) {
        return boost::none;
    }
    auto& it = _groupsIterator;
    Document out = makeDocument(it->first, it->second);
    ++it;
    return out;
}

namespace {

using GroupsMap = GroupProcessorBase::GroupsMap;

class SorterComparator {
public:
    SorterComparator(ValueComparator valueComparator) : _valueComparator(valueComparator) {}

    int operator()(const Value& lhs, const Value& rhs) const {
        return _valueComparator.compare(lhs, rhs);
    }

private:
    ValueComparator _valueComparator;
};

class SpillSTLComparator {
public:
    SpillSTLComparator(ValueComparator valueComparator) : _valueComparator(valueComparator) {}

    bool operator()(const GroupsMap::value_type* lhs, const GroupsMap::value_type* rhs) const {
        return _valueComparator.evaluate(lhs->first < rhs->first);
    }

private:
    ValueComparator _valueComparator;
};

}  // namespace

void GroupProcessor::add(const Value& groupKey, const Document& root) {
    auto [groupIter, inserted] = findOrCreateGroup(groupKey);

    const size_t numAccumulators = _accumulatedFields.size();
    auto& group = groupIter->second;
    for (size_t i = 0; i < numAccumulators; i++) {
        // Only process the input and update the memory footprint if the current accumulator
        // needs more input.
        if (group[i]->needsInput()) {
            accumulate(groupIter, i, computeAccumulatorArg(root, i));
        }
    }

    if (shouldSpillWithAttemptToSaveMemory() || shouldSpillOnEveryDuplicateId(inserted)) {
        spill();
    }
}

void GroupProcessor::readyGroups() {
    _spilled = !_sortedFiles.empty();
    if (_spilled) {
        if (!_groups.empty()) {
            spill();
        }

        _sorterIterator = Sorter<Value, Value>::Iterator::merge(
            _sortedFiles, SortOptions(), SorterComparator(_expCtx->getValueComparator()));

        // prepare current to accumulate data
        _currentAccumulators.reserve(_accumulatedFields.size());
        for (const auto& accumulatedField : _accumulatedFields) {
            _currentAccumulators.push_back(accumulatedField.makeAccumulator());
        }

        MONGO_verify(_sorterIterator->more());  // we put data in, we should get something out.
        _firstPartOfNextGroup = _sorterIterator->next();
    } else {
        _groupsIterator = _groups.begin();
    }

    // Update GroupStats here when reading groups in case the query finishes early without resetting
    // the GroupProcessor. This guarantees we have $group-level statistics for explain.
    _stats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();

    _groupsReady = true;
}

void GroupProcessor::reset() {
    // Free our resources.
    GroupProcessorBase::reset();

    _groupsReady = false;
    _groupsIterator = _groups.end();

    _sorterIterator.reset();
    _sortedFiles.clear();
}

bool GroupProcessor::shouldSpillWithAttemptToSaveMemory() {
    if (!_memoryTracker.allowDiskUse() && !_memoryTracker.withinMemoryLimit()) {
        freeMemory();
    }
    return !_memoryTracker.withinMemoryLimit();
}

bool GroupProcessor::shouldSpillOnEveryDuplicateId(bool isNewGroup) {
    // Spill every time we have a duplicate id to stress merge logic.
    return (internalQueryEnableAggressiveSpillsInGroup.loadRelaxed() &&
            !_expCtx->getOperationContext()->readOnly() && !isNewGroup &&  // is not a new group
            !_expCtx->getInRouter() &&        // can't spill to disk in router
            _memoryTracker.allowDiskUse() &&  // never spill when disk use is explicitly prohibited
            _sortedFiles.size() < 20);
}

void GroupProcessor::spill() {
    if (_groups.empty()) {
        return;
    }

    // If _groupsReady is true, we may have already returned some results, so we should skip them.
    auto groupsIt = _groupsReady ? _groupsIterator : _groups.begin();
    if (groupsIt == _groups.end()) {
        // There is nothing to spill.
        return;
    }

    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for $group, but didn't allow external sort."
            " Pass allowDiskUse:true to opt in.",
            _memoryTracker.allowDiskUse());

    // Ensure there is sufficient disk space for spilling
    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        _expCtx->getTempDir(), internalQuerySpillingMinAvailableDiskSpaceBytes.load()));

    std::vector<const GroupProcessorBase::GroupsMap::value_type*>
        ptrs;  // using pointers to speed sorting
    ptrs.reserve(_groups.size());

    int64_t spilledRecords = 0;
    for (auto end = _groups.end(); groupsIt != end; ++groupsIt) {
        ptrs.push_back(&*groupsIt);
        ++spilledRecords;
    }

    std::sort(ptrs.begin(), ptrs.end(), SpillSTLComparator(_expCtx->getValueComparator()));

    // Initialize '_file' in a lazy manner only when it is needed.
    if (!_file) {
        _spillStats = std::make_unique<SorterFileStats>(nullptr /* sorterTracker */);
        _file = std::make_shared<Sorter<Value, Value>::File>(
            sorter::nextFileName(_expCtx->getTempDir()), _spillStats.get());
    }
    SortedFileWriter<Value, Value> writer(SortOptions().TempDir(_expCtx->getTempDir()), _file);
    switch (_accumulatedFields.size()) {  // same as ptrs[i]->second.size() for all i.
        case 0:                           // no values, essentially a distinct
            for (size_t i = 0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first, Value());
            }
            break;

        case 1:  // just one value, use optimized serialization as single Value
            for (size_t i = 0; i < ptrs.size(); i++) {
                writer.addAlreadySorted(ptrs[i]->first,
                                        ptrs[i]->second[0]->getValue(/*toBeMerged=*/true));
            }
            break;

        default:  // multiple values, serialize as array-typed Value
            for (size_t i = 0; i < ptrs.size(); i++) {
                std::vector<Value> accums;
                for (size_t j = 0; j < ptrs[i]->second.size(); j++) {
                    accums.push_back(ptrs[i]->second[j]->getValue(/*toBeMerged=*/true));
                }
                writer.addAlreadySorted(ptrs[i]->first, Value(std::move(accums)));
            }
            break;
    }
    _sortedFiles.emplace_back(writer.done());

    auto spilledDataStorageIncrease = _stats.spillingStats.updateSpillingStats(
        1, _memoryTracker.inUseTrackedMemoryBytes(), spilledRecords, _spillStats->bytesSpilled());
    groupCounters.incrementPerSpilling(1 /* spills */,
                                       _memoryTracker.inUseTrackedMemoryBytes(),
                                       spilledRecords,
                                       spilledDataStorageIncrease);

    // Zero out the current per-accumulation statement memory consumption, as the memory has been
    // freed by spilling.
    GroupProcessorBase::reset();
    _groupsIterator = _groups.end();

    if (_groupsReady) {
        tassert(9917000,
                "Expect everything to be spilled if groups are ready and it is not the first spill",
                !_spilled);
        // Ready groups again to read from disk instead of from memory.
        readyGroups();
    }
}

}  // namespace mongo
