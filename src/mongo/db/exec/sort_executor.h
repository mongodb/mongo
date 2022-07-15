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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sort_key_comparator.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/db/sorter/sorter.h"

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
        int operator()(const typename DocumentSorter::Data& lhs,
                       const typename DocumentSorter::Data& rhs) const {
            return _sortKeyComparator(lhs.first, rhs.first);
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
                 bool allowDiskUse)
        : _sortPattern(std::move(sortPattern)),
          _tempDir(std::move(tempDir)),
          _diskUseAllowed(allowDiskUse) {
        _stats.sortPattern =
            _sortPattern.serialize(SortPattern::SortKeySerialization::kForExplain).toBson();
        _stats.limit = limit;
        _stats.maxMemoryUsageBytes = maxMemoryUsageBytes;
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
        return _stats.spills > 0;
    }

    /**
     * Returns true if the loading phase has been explicitly completed, and then the stream of
     * documents has subsequently been exhausted by "get next" calls.
     */
    bool isEOF() const {
        return _isEOF;
    }

    const SortStats& stats() const {
        return _stats;
    }

    /**
     * Add data item to be sorted of type T with sort key specified by Value to the sort executor.
     * Should only be called before 'loadingDone()' is called.
     */
    void add(const Value& sortKey, const T& data) {
        if (!_sorter) {
            _sorter.reset(DocumentSorter::make(makeSortOptions(), Comparator(_sortPattern)));
        }
        _sorter->add(sortKey, data);
    }

    /**
     * Signals to the sort executor that there will be no more input documents.
     */
    void loadingDone() {
        // This conditional should only pass if no documents were added to the sorter.
        if (!_sorter) {
            _sorter.reset(DocumentSorter::make(makeSortOptions(), Comparator(_sortPattern)));
        }
        _output.reset(_sorter->done());
        _stats.keysSorted += _sorter->numSorted();
        _stats.spills += _sorter->stats().spilledRanges();
        _stats.totalDataSizeBytes += _sorter->totalDataSizeSorted();
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
            _output.reset();
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

private:
    SortOptions makeSortOptions() const {
        SortOptions opts;
        if (_stats.limit) {
            opts.limit = _stats.limit;
        }

        opts.maxMemoryUsageBytes = _stats.maxMemoryUsageBytes;
        if (_diskUseAllowed) {
            opts.extSortAllowed = true;
            opts.tempDir = _tempDir;
        }

        return opts;
    }

    const SortPattern _sortPattern;
    const std::string _tempDir;
    const bool _diskUseAllowed;

    std::unique_ptr<DocumentSorter> _sorter;
    std::unique_ptr<typename DocumentSorter::Iterator> _output;

    SortStats _stats;

    bool _isEOF = false;
};
}  // namespace mongo
