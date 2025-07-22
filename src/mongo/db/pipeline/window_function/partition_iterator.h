/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/partition_key_comparator.h"
#include "mongo/db/pipeline/spilling/spillable_deque.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * This class provides an abstraction for accessing documents in a partition via an interator-type
 * interface. There is always a "current" document; operator[] provides random access relative to
 * the current document, so that iter[+2] is refers to the 2 positions ahead of the current one.
 *
 * The 'partionExpr' is used to determine partition boundaries, provide the illusion that only the
 * current partition exists.
 *
 * The 'sortPattern' is used for resolving range-based and time-based bounds, in 'getEndpoints()'.
 *
 */
class PartitionIterator {
public:
    PartitionIterator(ExpressionContext* expCtx,
                      exec::agg::Stage* source,
                      MemoryUsageTracker* tracker,
                      boost::optional<boost::intrusive_ptr<Expression>> partitionExpr,
                      const boost::optional<SortPattern>& sortPattern);

    ~PartitionIterator();

    using SlotId = unsigned int;
    SlotId newSlot() {
        tassert(5371200,
                "Unexpected usage of partition iterator, expected all consumers to create slots "
                "before retrieving documents",
                _state == IteratorState::kNotInitialized);
        auto retId = static_cast<SlotId>(_slots.size());
        _slots.emplace_back(-1);
        return retId;
    }

    /**
     * Returns the current document pointed to by the iterator.
     */
    boost::optional<Document> current() {
        return (*this)[0];
    }

    /**
     * Returns true if iterator execution is paused.
     */
    bool isPaused() {
        return _state == IteratorState::kPauseExecution;
    }

    enum class AdvanceResult {
        kAdvanced,
        kNewPartition,
        kEOF,
    };

    /**
     * Advances the iterator to the next document in the partition. Returns a status indicating
     * whether the new iterator state is in the same partition, the next partition, or EOF.
     */
    AdvanceResult advance();

    /**
     * Returns the index of the iterator for the current partition. Note that the returned index
     * refers to the global index in the partition and should not be used as an argument to the
     * operator[] overload.
     */
    auto getCurrentPartitionIndex() const {
        return _indexOfCurrentInPartition;
    }

    /**
     * Sets the input Stage for this iterator to 'source'.
     */
    void setSource(exec::agg::Stage* source) {
        _source = source;
    }

    /**
     * Returns the value in bytes of the data being stored by this partition iterator. Does not
     * include the size of the constant size objects being held or the overhead of the data
     * structures.
     */
    auto getApproximateSize() const {
        return _cache.getApproximateSize() + getNextPartitionStateSize();
    }

    /**
     * Spill whatever's in the cache to disk. Caller is responsible for checking whether this is
     * allowed.
     */
    void spillToDisk() {
        _cache.spillToDisk();
    }

    bool usedDisk() const {
        return _cache.usedDisk();
    }

    const SpillingStats& getSpillingStats() const {
        return _cache.getSpillingStats();
    }

    /**
     * Clean up all memory associated with the partition iterator. All calls requesting documents
     * are invalid after calling this.
     */
    void finalize() {
        _cache.finalize();
    }

    /**
     * Optimizes the partition expression if one exists. If the partition expression references a
     * let variable it would not have been optimized when initialized.
     */
    void optimizePartition() {
        if (_partitionExpr) {
            _partitionExpr = _partitionExpr->get()->optimize();
        }
    }

private:
    friend class PartitionAccessor;

    /**
     * Request the document in the current partition that is 'index' positions from the current. For
     * instance, index 0 refers to the current document pointed to by the iterator.
     *
     * Returns boost::none if the document is not in the partition or we've hit EOF.
     */
    boost::optional<Document> operator[](int index);

    /**
     * Resolves any type of WindowBounds to a concrete pair of indices, '[lower, upper]'.
     *
     * Both 'lower' and 'upper' are valid offsets, such that '(*this)[lower]' and '(*this)[upper]'
     * returns a document. If the window contains one document, then 'lower == upper'. If the
     * window contains zero documents, then there are no valid offsets, so we return boost::none.
     * (The window can be empty when it is shifted completely past one end of the partition, as in
     *  [+999, +999] or [-999, -999].)
     *
     * The offsets can be different after every 'advance()'. Even for simple document-based
     * windows, the returned offsets can be different than the user-specified bounds when we
     * are close to a partition boundary.  For example, at the beginning of a partition,
     * 'getEndpoints(DocumentBased{-10, +7})' would be '[0, +7]'.
     *
     * This method is non-const because it may pull documents into memory up to the end of the
     * window.
     *
     * 'hint', if specified, should be the last result of getEndpoints() for the same 'bounds'.
     */
    boost::optional<std::pair<int, int>> getEndpoints(
        const WindowBounds& bounds, const boost::optional<std::pair<int, int>>& hint);

    /**
     * Returns the smallest offset 'i' such that (*this)[i] is in '_cache'.
     *
     * This value is negative or zero, because the current document is always in '_cache'.
     */
    auto getMinCachedOffset() const {
        return -_indexOfCurrentInPartition + _cache.getLowestIndex();
    }

    /**
     * Returns the largest offset 'i' such that (*this)[i] is in '_cache'.
     *
     * Note that offsets greater than 'i' might still be in the partition, even though they
     * haven't been loaded into '_cache' yet. If you want to know where the partition ends,
     * call 'cacheWholePartition' first.
     *
     * This value is positive or zero, because the current document is always in '_cache'.
     */
    auto getMaxCachedOffset() const {
        return _cache.getHighestIndex() - _indexOfCurrentInPartition;
    }

    /**
     * Performs the actual work of the 'advance()' call. This is wrapped by 'advance()' so that
     * regardless of how it returns 'advance()' will also free documents that can be expired as a
     * result.
     */
    AdvanceResult advanceInternal();
    /**
     * Loads documents into '_cache' until we reach a partition boundary.
     */
    void cacheWholePartition() {
        // Start from one past the end of _cache.
        int i = getMaxCachedOffset() + 1;
        // If we have already loaded everything into '_cache' then this condition will be false
        // immediately.
        while ((*this)[i]) {
            ++i;
        }
    }

    /**
     * Marks the given index as expired for the slot 'id'. This does not necessarily mean that the
     * document will be released, just that this slot no longer requires it.
     */
    void expireUpTo(SlotId id, int index) {
        // 'index' is relevant to the current document, adjust it to figure out what index it refers
        // to in the cache.
        _slots[id] = std::max(_slots[id], _indexOfCurrentInPartition + index);
    }

    /**
     * Frees any documents from the cache which have been marked as "expired" by all slots.
     */
    void releaseExpired();

    /**
     * Retrieves the next document from the prior stage and updates the state accordingly.
     */
    void getNextDocument();

    void resetCache() {
        _cache.clear();
        _indexOfCurrentInPartition = 0;
        for (int slot = 0; slot < (int)_slots.size(); slot++) {
            _slots[slot] = -1;
        }
    }

    /**
     * Resets the state of the iterator with the first document of the new partition.
     */
    void advanceToNextPartition();

    // Internal helpers for 'getEndpoints()'.
    boost::optional<std::pair<int, int>> getEndpointsRangeBased(
        const WindowBounds::RangeBased& bounds, const boost::optional<std::pair<int, int>>& hint);
    boost::optional<std::pair<int, int>> getEndpointsDocumentBased(
        const WindowBounds::DocumentBased& bounds,
        const boost::optional<std::pair<int, int>>& hint);

    ExpressionContext* _expCtx;
    exec::agg::Stage* _source;
    boost::optional<boost::intrusive_ptr<Expression>> _partitionExpr;

    // '_sortExpr' tells us which field is the "time" field. When the user writes
    // 'sortBy: {ts: 1}', any time-based or range-based window bounds are defined using
    // the value of the "$ts" field. This _sortExpr is used in getEndpoints().
    boost::optional<boost::intrusive_ptr<ExpressionFieldPath>> _sortExpr;

    std::unique_ptr<PartitionKeyComparator> _partitionComparator = nullptr;
    std::vector<int> _slots;

    // When encountering the first document of the next partition, we stash it away until the
    // iterator has advanced to it. This document is not accessible until then.
    boost::optional<Document> _nextPartitionDoc;
    size_t getNextPartitionStateSize() const {
        size_t size = 0;
        if (_nextPartitionDoc) {
            size += _nextPartitionDoc->getApproximateSize();
        }
        if (_partitionComparator) {
            size += _partitionComparator->getApproximateSize();
        }
        return size;
    }
    void updateNextPartitionStateSize();

    enum class IteratorState {
        // Default state, no documents have been pulled into the cache.
        kNotInitialized,
        // Input sources do not have a result to be processed yet, but there may be more results in
        // the future.
        kPauseExecution,
        // Iterating the current partition. We don't know where the current partition ends, or
        // whether it's the last partition.
        kIntraPartition,
        // The first document of the next partition has been retrieved, but the iterator has not
        // advanced to it yet.
        kAwaitingAdvanceToNext,
        // Similar to the next partition case, except for EOF: we know the current partition is the
        // final one, because the underlying iterator has returned EOF.
        kAwaitingAdvanceToEOF,
        // The iterator has exhausted the input documents. Any access should be disallowed.
        kAdvancedToEOF,
    } _state;

    // '_indexOfCurrentInPartition' is the current document, which '(*this)[0]' returns.
    int _indexOfCurrentInPartition = 0;

    // The actual cache of the PartitionIterator. Holds documents and spills documents that exceed
    // the memory limit given to PartitionIterator to disk.
    SpillableDeque _cache;

    // Memory token, used to track memory consumption of PartitionIterator. Needed to avoid problems
    // when getNextPartitionStateSize() changes value between invocations.
    MemoryUsageToken _memoryToken;
};

/**
 * This class provides access to an underlying PartitionIterator and manages when documents in the
 * partition are no longer needed.
 */
class PartitionAccessor {
public:
    enum class Policy {
        // This policy assumes that when the caller accesses a certain index 'i', that it will no
        // longer require all documents up to and including the document at index 'i'.
        kDefaultSequential,
        // This policy should be used if the caller requires the endpoints of a window. Documents
        // to the left of the left endpoint may disappear on the next call to releaseExpired().
        kEndpoints,
        // This policy means the caller only looks at how the right endpoint changes.
        // The caller may look at documents between the most recent two right endpoints.
        kRightEndpoint,
        // This policy allows the window function executor to manually release expired documents
        // after evaluating values in the accessed documents.
        kManual,
    };
    PartitionAccessor(PartitionIterator* iter, Policy policy)
        : _iter(iter), _slot(iter->newSlot()), _policy(policy) {}

    boost::optional<Document> operator[](int index) {
        auto ret = (*_iter)[index];
        switch (_policy) {
            case Policy::kDefaultSequential:
                _iter->expireUpTo(_slot, index);
                break;
            case Policy::kEndpoints:
            case Policy::kRightEndpoint:
                break;
            case Policy::kManual:
                break;
        }
        return ret;
    }

    auto getCurrentPartitionIndex() const {
        return _iter->getCurrentPartitionIndex();
    }

    boost::optional<std::pair<int, int>> getEndpoints(
        const WindowBounds& bounds,
        const boost::optional<std::pair<int, int>>& hint = boost::none) {
        auto endpoints = _iter->getEndpoints(bounds, hint);
        switch (_policy) {
            case Policy::kDefaultSequential:
                tasserted(5371201, "Invalid usage of partition accessor");
                break;
            case Policy::kEndpoints:
                // With this policy, all documents before the lower bound can be marked as expired.
                // They will only be released on the next call to releaseExpired(), so when
                // getEndpoints() returns, the caller may also look at documents from the previous
                // result of getEndpoints(), until it returns control to the Stage.
                if (endpoints) {
                    _iter->expireUpTo(_slot, endpoints->first - 1);
                }
                break;
            case Policy::kRightEndpoint:
                // With this policy, all documents before the upper bound can be marked as expired.
                // They will only be released on the next call to releaseExpired(), so when
                // getEndpoints() returns, the caller may also look at documents from the previous
                // result of getEndpoints(), until it returns control to the Stage.
                if (endpoints) {
                    _iter->expireUpTo(_slot, endpoints->second - 1);
                }
                break;
            case Policy::kManual:
                break;
        }
        return endpoints;
    }
    void manualExpireUpTo(int offset) {
        tassert(
            6050002,
            "Documents can only be manually expired by a PartitionIterator with a manual policy ",
            _policy == Policy::kManual);
        _iter->expireUpTo(_slot, offset);
    }


private:
    PartitionIterator* _iter;
    const PartitionIterator::SlotId _slot;
    const Policy _policy;
};

}  // namespace mongo
