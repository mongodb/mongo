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
        // Iterating the current partition.
        kIntraPartition,
        // The first document of the next partition has been retrieved, but the iterator has not
        // advanced to it yet.
        kAwaitingAdvanceToNext,
        // Similar to the next partition case, except for EOF.
        kAwaitingAdvanceToEOF,
        // The iterator has exhausted the input documents. Any access should be disallowed.
        kAdvancedToEOF,
    } _state;
};

}  // namespace mongo
