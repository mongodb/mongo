// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort_key_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>
#include <utility>

#include <boost/filesystem.hpp>

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
 *
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on this class.
 */
template <typename T>
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SortExecutor {
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
                 MemoryUsageLimit maxMemoryUsageBytes,
                 boost::filesystem::path tempDir,
                 bool allowDiskUse,
                 bool moveSortedDataIntoIterator = false)
        : _sortPattern(std::move(sortPattern)),
          _tempDir(std::move(tempDir)),
          _diskUseAllowed(allowDiskUse),
          _moveSortedDataIntoIterator(moveSortedDataIntoIterator),
          _maxAllowedMemoryUsageBytes(maxMemoryUsageBytes) {
        _stats.sortPattern =
            _sortPattern.serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
        _stats.limit = limit;
        _stats.maxMemoryUsageBytes = static_cast<uint64_t>(maxMemoryUsageBytes.get());
        if (allowDiskUse) {
            _sorterFileStats = std::make_unique<SorterFileStats>(nullptr);
        }
    }

    SortExecutor(SortPattern sortPattern,
                 uint64_t limit,
                 uint64_t maxMemoryUsageBytes,
                 boost::filesystem::path tempDir,
                 bool allowDiskUse,
                 bool moveSortedDataIntoIterator = false)
        : SortExecutor(std::move(sortPattern),
                       limit,
                       MemoryUsageLimit{static_cast<int64_t>(maxMemoryUsageBytes)},
                       std::move(tempDir),
                       allowDiskUse,
                       moveSortedDataIntoIterator) {}

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

    MemoryUsageLimit getMaxMemoryBytes() const {
        return _maxAllowedMemoryUsageBytes;
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
        return opts;
    }

    std::unique_ptr<DocumentSorter> makeSorter();

    void ensureSorter() {
        // This conditional should only pass if no documents were added to the sorter.
        if (!_sorter) {
            _sorter = makeSorter();
        }
    }

    const SortPattern _sortPattern;
    const boost::filesystem::path _tempDir;
    const bool _diskUseAllowed;
    const bool _moveSortedDataIntoIterator;

    MemoryUsageLimit _maxAllowedMemoryUsageBytes;

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
