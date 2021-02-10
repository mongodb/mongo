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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/window_function/partition_iterator.h"

namespace mongo {

boost::optional<Document> PartitionIterator::operator[](int index) {
    auto desired = _currentIndex + index;

    if (_state == IteratorState::kAdvancedToEOF)
        return boost::none;

    // Case 0: Outside of lower bound of partition.
    if (desired < 0)
        return boost::none;

    // Case 1: Document is in the cache already.
    if (desired >= 0 && desired < (int)_cache.size())
        return _cache[desired];

    // Case 2: Attempting to access index greater than what the cache currently holds. If we've
    // already exhausted the partition, then early return. Otherwise continue to pull in
    // documents from the prior stage until we get to the desired index or reach the next partition.
    if (_state == IteratorState::kAwaitingAdvanceToNext ||
        _state == IteratorState::kAwaitingAdvanceToEOF)
        return boost::none;
    for (int i = _cache.size(); i <= desired; i++) {
        // Pull in document from prior stage.
        getNextDocument();
        // Check for EOF or the next partition.
        if (_state == IteratorState::kAwaitingAdvanceToNext ||
            _state == IteratorState::kAwaitingAdvanceToEOF)
            return boost::none;
    }

    return _cache[desired];
}

PartitionIterator::AdvanceResult PartitionIterator::advance() {
    // Check if the next document is in the cache.
    if ((_currentIndex + 1) < (int)_cache.size()) {
        // Same partition, update the current index.
        _currentIndex++;
        return AdvanceResult::kAdvanced;
    }

    // At this point, the requested document is not in the cache and we need to consider
    // whether to pull from the prior stage.
    switch (_state) {
        case IteratorState::kNotInitialized:
        case IteratorState::kIntraPartition:
            // Pull in the next document and advance the pointer.
            getNextDocument();
            if (_state == IteratorState::kAwaitingAdvanceToEOF) {
                _cache.clear();
                _currentIndex = 0;
                _state = IteratorState::kAdvancedToEOF;
                return AdvanceResult::kEOF;
            } else if (_state == IteratorState::kAwaitingAdvanceToNext) {
                advanceToNextPartition();
                return AdvanceResult::kNewPartition;
            } else {
                // Same partition, update the current index.
                _currentIndex++;
                return AdvanceResult::kAdvanced;
            }
        case IteratorState::kAwaitingAdvanceToNext:
            // The doc in the next partition has already been read.
            advanceToNextPartition();
            return AdvanceResult::kNewPartition;
        case IteratorState::kAwaitingAdvanceToEOF:
        case IteratorState::kAdvancedToEOF:
            // In either of these states, there's no point in reading from the prior document source
            // because we've already hit EOF.
            _cache.clear();
            _currentIndex = 0;
            return AdvanceResult::kEOF;
        default:
            MONGO_UNREACHABLE_TASSERT(5340102);
    }
}

void PartitionIterator::getNextDocument() {
    tassert(5340103,
            "Invalid call to PartitionIterator::getNextDocument",
            _state != IteratorState::kAdvancedToEOF);

    auto getNextRes = _source->getNext();
    if (getNextRes.isEOF()) {
        _state = IteratorState::kAwaitingAdvanceToEOF;
        return;
    }

    if (!getNextRes.isAdvanced())
        return;

    auto doc = getNextRes.releaseDocument();
    if (_partitionExpr) {
        auto curKey = (*_partitionExpr)->evaluate(doc, &_expCtx->variables);
        uassert(ErrorCodes::TypeMismatch,
                "Cannot 'partitionBy' an expression of type array",
                !curKey.isArray());
        if (_state == IteratorState::kNotInitialized) {
            _nextPartition = NextPartitionState{std::move(doc), std::move(curKey)};
            advanceToNextPartition();
        } else if (_expCtx->getValueComparator().compare(curKey, _partitionKey) != 0) {
            _nextPartition = NextPartitionState{std::move(doc), std::move(curKey)};
            _state = IteratorState::kAwaitingAdvanceToNext;
        } else
            _cache.emplace_back(std::move(doc));
    } else {
        _cache.emplace_back(std::move(doc));
        _state = IteratorState::kIntraPartition;
    }
}

}  // namespace mongo
