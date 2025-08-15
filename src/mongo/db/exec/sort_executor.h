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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort_key_comparator.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_stats.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace mongo {
/**
 * The SortExecutor class is the internal implementation of sorting for query execution. The
 * caller should provide input documents by repeated calls to the add() function, and then
 * complete the loading process with a single call to loadingDone(). Finally, getNext() should be
 * called to return the documents one by one in sorted order.
 *
 * The template parameter is the type of data being sorted. In DocumentSource execution, we sort
 * Document objects directly, but in the PlanStage layer we may sort WorkingSetMembers. The type of
 * the sort key, on the other hand, is always Value.
 */
template <typename T>
class SortExecutor {
public:
    using DocumentSorter = Sorter<Value, T>;
    class Comparator {
    public:
        Comparator(const SortPattern& sortPattern) : _sortKeyComparator(sortPattern) {}
        int operator()(const Value& lhs, const Value& rhs) const {
            return _sortKeyComparator(lhs, rhs);
        }

    private:
        SortKeyComparator _sortKeyComparator;
    };

    /**
     * If the passed in limit is 0, this is treated as no limit.
     */
    SortExecutor(SortPattern sortPattern,
                 uint64_t limit,
                 uint64_t maxMemoryUsageBytes,
                 std::string tempDir,
                 bool allowDiskUse,
                 bool moveSortedDataIntoIterator = false)
        : _sortPattern(std::move(sortPattern)),
          _tempDir(std::move(tempDir)),
          _diskUseAllowed(allowDiskUse),
          _moveSortedDataIntoIterator(moveSortedDataIntoIterator) {
        _stats.sortPattern =
            _sortPattern.serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
        _stats.limit = limit;
        _stats.maxMemoryUsageBytes = maxMemoryUsageBytes;
        if (allowDiskUse) {
            _sorterFileStats = std::make_unique<SorterFileStats>(nullptr);
        }
    }

    const SortPattern& sortPattern() const {
        return _sortPattern;
    }

    /**
     * Absorbs 'limit', enabling a top-k sort. It is safe to call this multiple times, it will keep
     * the smallest limit.
     */
    void setLimit(uint64_t limit) {
        if (!_stats.limit || limit < _stats.limit)
            _stats.limit = limit;
    }

    uint64_t getLimit() const {
        return _stats.limit;
    }

    bool hasLimit() const {
        return _stats.limit > 0;
    }

    bool wasDiskUsed() const {
        return _stats.spillingStats.getSpills() > 0;
    }

    /**
     * Returns true if the loading phase has been explicitly completed, and then the stream of
     * documents has subsequently been exhausted by "get next" calls.
     */
    bool isEOF() const {
        return _isEOF;
    }

    const SortStats& stats() const {
        if (_sorter) {
            _stats.memoryUsageBytes = _sorter->stats().memUsage();
            _stats.peakTrackedMemBytes =
                std::max(_stats.peakTrackedMemBytes, _stats.memoryUsageBytes);
        }
        return _stats;
    }

    SorterFileStats* getSorterFileStats() const {
        return _sorterFileStats.get();
    }

    long long spilledBytes() const {
        if (!_sorterFileStats) {
            return 0;
        }
        return _sorterFileStats->bytesSpilledUncompressed();
    }

    long long spilledDataStorageSize() const {
        if (!_sorterFileStats) {
            return 0;
        }
        return _sorterFileStats->bytesSpilled();
    }

    /**
     * Add data item to be sorted of type T with sort key specified by Value to the sort executor.
     * Should only be called before 'loadingDone()' is called.
     */
    void add(const Value& sortKey, const T& data) {
        ensureSorter();
        _sorter->add(sortKey, data);
    }

    /**
     * Signals to the sort executor that there will be no more input documents.
     */
    void loadingDone() {
        ensureSorter();
        _output = _sorter->done();
        _stats.keysSorted += _sorter->stats().numSorted();
        _stats.spillingStats.incrementSpills(_sorter->stats().spilledRanges());
        _stats.spillingStats.incrementSpilledBytes(spilledBytes());
        _stats.spillingStats.incrementSpilledDataStorageSize(spilledDataStorageSize());
        _stats.spillingStats.incrementSpilledRecords(_sorter->stats().spilledKeyValuePairs());
        _stats.totalDataSizeBytes += _sorter->stats().bytesSorted();
        _stats.memoryUsageBytes = 0;
        _sorter.reset();
    }

    /**
     * Returns true if there are more results which can be returned via 'getNext()', or false to
     * indicate end-of-stream. Should only be called after 'loadingDone()' is called.
     */
    bool hasNext() {
        if (_isEOF) {
            return false;
        }

        if (!_output->more()) {
            clearSortTable();
            _isEOF = true;
            return false;
        }

        return true;
    }

    /**
     * Returns the next data item in the sorted stream, which is a pair consisting of the sort key
     * and the corresponding item being sorted. Illegal to call if there is no next item;
     * end-of-stream must be detected with 'hasNext()'.
     */
    std::pair<Value, T> getNext() {
        return _output->next();
    }

    uint64_t getMaxMemoryBytes() const {
        return _stats.maxMemoryUsageBytes;
    }

    /**
     * Pauses Loading and creates an iterator which can be used to get the current state in
     * read-only mode. The stream code needs this to pause and get the current internal state which
     * can be used to store it to a persistent storage which will constitute a checkpoint for
     * streaming processing.
     */
    void pauseLoading() {
        invariant(!_paused);
        _paused = true;
        ensureSorter();
        _output = _sorter->pause();
    }

    /**
     * Resumes Loading. This will remove the iterator created in pauseLoading().
     */
    void resumeLoading() {
        invariant(_paused);
        _paused = false;
        ensureSorter();
        clearSortTable();
        _sorter->resume();
        _isEOF = false;
    }

    void forceSpill() {
        if (_sorter) {
            _sorter->spill();
        } else if (_output) {
            if (_output->spillable()) {
                SorterTracker tracker;
                auto opts = makeSortOptions();
                opts.Tracker(&tracker);

                _output = _output->spill(opts, typename DocumentSorter::Settings());

                _stats.spillingStats.incrementSpills(tracker.spilledRanges.loadRelaxed());
                _stats.spillingStats.incrementSpilledRecords(
                    tracker.spilledKeyValuePairs.loadRelaxed());
                _stats.spillingStats.setSpilledBytes(spilledBytes());
                _stats.spillingStats.updateSpilledDataStorageSize(spilledDataStorageSize());
            }
        }
    }

private:
    /*
     * '_output' is a DocumentSorter::Iterator that can have the following iterator values:
     * (1) InMemIterator, (2) InMemReadOnlyIterator, (3) FileIterator, or (4) MergeIterator
     * If '_output' is an InMemIterator or an InMemReadOnlyIterator, the sort table will be cleared
     * in memory. If '_output' is an MergeIterator, the  spilled sorted data will be cleared.
     * However, the sort table needs to be cleared through a call to reset(). Otherwise, '_output'
     * is a FileIterator and the sort table needs to be cleared through a call to reset().
     */
    void clearSortTable() {
        _output.reset();
    }

    SortOptions makeSortOptions() const {
        SortOptions opts;
        opts.MoveSortedDataIntoIterator(_moveSortedDataIntoIterator);
        if (_stats.limit) {
            opts.Limit(_stats.limit);
        }

        opts.MaxMemoryUsageBytes(_stats.maxMemoryUsageBytes);
        if (_diskUseAllowed) {
            opts.TempDir(_tempDir);
            opts.FileStats(_sorterFileStats.get());
        }

        return opts;
    }

    void ensureSorter() {
        // This conditional should only pass if no documents were added to the sorter.
        if (!_sorter) {
            _sorter = DocumentSorter::make(makeSortOptions(), Comparator(_sortPattern));
        }
    }

    const SortPattern _sortPattern;
    const std::string _tempDir;
    const bool _diskUseAllowed;
    const bool _moveSortedDataIntoIterator;

    std::unique_ptr<SorterFileStats> _sorterFileStats;
    std::unique_ptr<DocumentSorter> _sorter;
    std::unique_ptr<typename DocumentSorter::Iterator> _output;

    mutable SortStats _stats;

    bool _isEOF = false;
    bool _paused = false;
};

extern template class SortExecutor<Document>;
extern template class SortExecutor<SortableWorkingSetMember>;
extern template class SortExecutor<BSONObj>;
}  // namespace mongo
