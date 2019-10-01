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
 */
class SortExecutor {
public:
    /**
     * If the passed in limit is 0, this is treated as no limit.
     */
    SortExecutor(SortPattern sortPattern,
                 uint64_t limit,
                 uint64_t maxMemoryUsageBytes,
                 std::string tempDir,
                 bool allowDiskUse);

    boost::optional<Document> getNextDoc();

    boost::optional<WorkingSetMember> getNextWsm();

    const SortPattern& sortPattern() const {
        return _sortPattern;
    }

    /**
     * Absorbs 'limit', enabling a top-k sort. It is safe to call this multiple times, it will keep
     * the smallest limit.
     */
    void setLimit(uint64_t limit) {
        if (!_limit || limit < _limit)
            _limit = limit;
    }

    uint64_t getLimit() const {
        return _limit;
    }

    bool hasLimit() const {
        return _limit > 0;
    }

    bool wasDiskUsed() const {
        return _wasDiskUsed;
    }

    /**
     * Signals to the sort executor that there will be no more input documents.
     */
    void loadingDone();

    /**
     * Add a Document with sort key specified by Value to the DocumentSorter.
     */
    void add(Value, Document);

    /**
     * Add a WorkingSetMember with sort key specified by Value to the DocumentSorter.
     */
    void add(Value, WorkingSetMember);

    /**
     * Returns true if the loading phase has been explicitly completed, and then the stream of
     * documents has subsequently been exhausted by "get next" calls.
     */
    bool isEOF() const {
        return _isEOF;
    }

    std::unique_ptr<SortStats> stats() const;

private:
    using DocumentSorter = Sorter<Value, WorkingSetMember>;
    class Comparator {
    public:
        Comparator(const SortPattern& sortPattern) : _sortKeyComparator(sortPattern) {}
        int operator()(const DocumentSorter::Data& lhs, const DocumentSorter::Data& rhs) const {
            return _sortKeyComparator(lhs.first, rhs.first);
        }

    private:
        SortKeyComparator _sortKeyComparator;
    };

    SortOptions makeSortOptions() const;

    SortPattern _sortPattern;
    //  A limit of zero is defined as no limit.
    uint64_t _limit;
    uint64_t _maxMemoryUsageBytes;
    std::string _tempDir;
    bool _diskUseAllowed = false;

    std::unique_ptr<DocumentSorter> _sorter;
    std::unique_ptr<DocumentSorter::Iterator> _output;

    bool _isEOF = false;
    bool _wasDiskUsed = false;
    uint64_t _totalDataSizeBytes = 0u;
};
}  // namespace mongo
