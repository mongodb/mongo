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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/window_function/window_bounds.h"

namespace mongo {

/**
 * This class provides an abstraction for accessing documents in a partition via an interator-type
 * interface. There is always a "current" document with which indexed access is relative to.
 */
class PartitionIterator {
public:
    PartitionIterator(ExpressionContext* expCtx,
                      DocumentSource* source,
                      boost::optional<boost::intrusive_ptr<Expression>> partitionExpr)
        : _expCtx(expCtx),
          _source(source),
          _partitionExpr(partitionExpr),
          _state(IteratorState::kNotInitialized) {}

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
        return _currentPartitionIndex;
    }

    /**
     * Sets the input DocumentSource for this iterator to 'source'.
     */
    void setSource(DocumentSource* source) {
        _source = source;
    }

    /**
     * Returns the value in bytes of the data being stored by this partition iterator. Does not
     * include the size of the constant size objects being held or the overhead of the data
     * structures.
     */
    auto getApproximateSize() const {
        return _memUsageBytes;
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
     * Resolve any type of WindowBounds to a concrete pair of indices, '[lower, upper]'.
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
     */
    boost::optional<std::pair<int, int>> getEndpoints(const WindowBounds& bounds);

    /**
     * Marks the given index as expired for the slot 'id'. This does not necessarily mean that the
     * document will be released, just that this slot no longer requires it.
     */
    void expireUpTo(SlotId id, int index) {
        // 'index' is relevant to the current document, adjust it to figure out what index it refers
        // to in the cache.
        _slots[id] = std::max(_slots[id], _currentCacheIndex + index);
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
        // Everything should be empty at this point.
        _memUsageBytes = 0;
        _currentCacheIndex = 0;
        _currentPartitionIndex = 0;
        for (size_t slot = 0; slot < _slots.size(); slot++) {
            _slots[slot] = -1;
        }
    }

    /**
     * Resets the state of the iterator with the first document of the new partition.
     */
    void advanceToNextPartition() {
        tassert(5340101,
                "Invalid call to PartitionIterator::advanceToNextPartition",
                _nextPartition != boost::none);
        resetCache();
        // Cache is cleared, and we are moving the _nextPartition value to different positions.
        _memUsageBytes = getNextPartitionStateSize();
        _cache.emplace_back(std::move(_nextPartition->_doc));
        _partitionKey = std::move(_nextPartition->_partitionKey);
        _nextPartition.reset();
        _state = IteratorState::kIntraPartition;
    }

    ExpressionContext* _expCtx;
    DocumentSource* _source;
    boost::optional<boost::intrusive_ptr<Expression>> _partitionExpr;
    std::deque<Document> _cache;
    // '_cache[_currentCacheIndex]' is the current document, which '(*this)[0]' returns.
    int _currentCacheIndex = 0;
    int _currentPartitionIndex = 0;
    Value _partitionKey;
    std::vector<int> _slots;

    // When encountering the first document of the next partition, we stash it away until the
    // iterator has advanced to it. This document is not accessible until then.
    struct NextPartitionState {
        Document _doc;
        Value _partitionKey;
    };
    boost::optional<NextPartitionState> _nextPartition;
    size_t getNextPartitionStateSize() {
        if (_nextPartition) {
            return _nextPartition->_doc.getApproximateSize() +
                _nextPartition->_partitionKey.getApproximateSize();
        }
        return 0;
    }

    // The value in bytes of the data being held. Does not include the size of the constant size
    // data members or overhead of data structures.
    size_t _memUsageBytes = 0;

    enum class IteratorState {
        // Default state, no documents have been pulled into the cache.
        kNotInitialized,
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
        // This policy should be used if the caller requires the endpoints of a window, potentially
        // accessing the same bound twice.
        kEndpointsOnly
    };
    PartitionAccessor(PartitionIterator* iter, Policy policy)
        : _iter(iter), _slot(iter->newSlot()), _policy(policy) {}

    boost::optional<Document> operator[](int index) {
        auto ret = (*_iter)[index];
        switch (_policy) {
            case Policy::kDefaultSequential:
                _iter->expireUpTo(_slot, index);
                break;
            case Policy::kEndpointsOnly:
                break;
        }
        return ret;
    }

    auto getCurrentPartitionIndex() const {
        return _iter->getCurrentPartitionIndex();
    }

    boost::optional<std::pair<int, int>> getEndpoints(const WindowBounds& bounds) {
        tassert(5371201, "Invalid usage of partition accessor", _policy == Policy::kEndpointsOnly);
        auto endpoints = _iter->getEndpoints(bounds);
        // With this policy, all documents before the lower bound can be marked as expired.
        if (endpoints) {
            _iter->expireUpTo(_slot, endpoints->first - 1);
        }
        return endpoints;
    }

private:
    PartitionIterator* _iter;
    const PartitionIterator::SlotId _slot;
    const Policy _policy;
};

}  // namespace mongo
