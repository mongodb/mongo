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

    /**
     * Request the document in the current partition that is 'index' positions from the current. For
     * instance, index 0 refers to the current document pointed to by the iterator.
     *
     * Returns boost::none if the document is not in the partition or we've hit EOF.
     */
    boost::optional<Document> operator[](int index);

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
     * Returns the offset of the iterator for the current partition.
     */
    auto getCurrentOffset() const {
        return _currentIndex;
    }

    /**
     * Sets the input DocumentSource for this iterator to 'source'.
     */
    void setSource(DocumentSource* source) {
        _source = source;
    }

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

private:
    /**
     * Retrieves the next document from the prior stage and updates the state accordingly.
     */
    void getNextDocument();

    /**
     * Resets the state of the iterator with the first document of the new partition.
     */
    void advanceToNextPartition() {
        tassert(5340101,
                "Invalid call to PartitionIterator::advanceToNextPartition",
                _nextPartition != boost::none);
        _cache.clear();
        _cache.emplace_back(std::move(_nextPartition->_doc));
        _partitionKey = std::move(_nextPartition->_partitionKey);
        _nextPartition.reset();
        _currentIndex = 0;
        _state = IteratorState::kIntraPartition;
    }

    ExpressionContext* _expCtx;
    DocumentSource* _source;
    boost::optional<boost::intrusive_ptr<Expression>> _partitionExpr;
    std::vector<Document> _cache;
    // '_cache[_currentIndex]' is the current document, which '(*this)[0]' returns.
    int _currentIndex = 0;
    Value _partitionKey;

    // When encountering the first document of the next partition, we stash it away until the
    // iterator has advanced to it. This document is not accessible until then.
    struct NextPartitionState {
        Document _doc;
        Value _partitionKey;
    };
    boost::optional<NextPartitionState> _nextPartition;

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

}  // namespace mongo
