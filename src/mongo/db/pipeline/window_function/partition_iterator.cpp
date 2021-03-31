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
#include "mongo/util/visit_helper.h"

namespace mongo {

boost::optional<Document> PartitionIterator::operator[](int index) {
    auto desired = _currentCacheIndex + index;

    if (_state == IteratorState::kAdvancedToEOF)
        return boost::none;

    // Check that the caller is not attempting to access a document which has been released
    // already.
    tassert(5371202,
            str::stream() << "Invalid access of expired document in partition at index " << desired,
            desired >= 0 || ((_currentPartitionIndex + index) < 0));

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

void PartitionIterator::releaseExpired() {
    if (_slots.size() == 0)
        return;

    // The mapping of SlotId -> cacheIndex represents the highest index document in the cache which
    // the executor no longer requires. To be able to safely free the document at index N from the
    // cache, the following conditions must be met:
    // * All executors have expired at least index N
    // * The current index has advanced past N. We need to keep around the "current" document since
    //   the aggregation stage hasn't projected the output fields yet.
    auto minIndex = _slots[0];
    for (auto&& cacheIndex : _slots) {
        minIndex = std::min(minIndex, cacheIndex);
    }

    auto newCurrent = _currentCacheIndex;
    for (auto i = 0; i <= minIndex && i < _currentCacheIndex; i++) {
        _cache.pop_front();
        newCurrent--;
    }

    // Adjust the expired indexes for each slot since some documents may have been freed
    // from the front of the cache.
    if (newCurrent == _currentCacheIndex)
        return;
    for (size_t slot = 0; slot < _slots.size(); slot++) {
        _slots[slot] -= (_currentCacheIndex - newCurrent);
    }
    _currentCacheIndex = newCurrent;
}

PartitionIterator::AdvanceResult PartitionIterator::advance() {
    // After advancing the iterator, check whether there are any documents that can be released from
    // the cache.
    ON_BLOCK_EXIT([&] { releaseExpired(); });

    // Check if the next document is in the cache.
    if ((_currentCacheIndex + 1) < (int)_cache.size()) {
        // Same partition, update the current index.
        _currentCacheIndex++;
        _currentPartitionIndex++;
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
                resetCache();
                _state = IteratorState::kAdvancedToEOF;
                return AdvanceResult::kEOF;
            } else if (_state == IteratorState::kAwaitingAdvanceToNext) {
                advanceToNextPartition();
                return AdvanceResult::kNewPartition;
            } else {
                // Same partition, update the current index.
                _currentCacheIndex++;
                _currentPartitionIndex++;
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
            resetCache();
            return AdvanceResult::kEOF;
        default:
            MONGO_UNREACHABLE_TASSERT(5340102);
    }
}

namespace {
boost::optional<int> numericBound(WindowBounds::Bound<int> bound) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](WindowBounds::Unbounded) -> boost::optional<int> { return boost::none; },
            [](WindowBounds::Current) -> boost::optional<int> { return 0; },
            [](int i) -> boost::optional<int> { return i; },
        },
        bound);
}
}  // namespace

boost::optional<std::pair<int, int>> PartitionIterator::getEndpoints(const WindowBounds& bounds) {
    // For range-based bounds, we will need to:
    // 1. extract the sortBy for (*this)[0]
    // 2. step backwards until we cross bounds.lower
    // 3. step forwards until we cross bounds.upper
    // This means we'll need to pass in sortBy somewhere.
    tassert(5423300,
            "TODO SERVER-54294: range-based and time-based bounds",
            stdx::holds_alternative<WindowBounds::DocumentBased>(bounds.bounds));
    tassert(5423301, "getEndpoints assumes there is a current document", (*this)[0] != boost::none);
    auto docBounds = stdx::get<WindowBounds::DocumentBased>(bounds.bounds);
    boost::optional<int> lowerBound = numericBound(docBounds.lower);
    boost::optional<int> upperBound = numericBound(docBounds.upper);
    tassert(5423302,
            "Bounds should never be inverted",
            !lowerBound || !upperBound || lowerBound <= upperBound);

    // Pull documents into the cache until it contains the whole window.
    if (upperBound) {
        // For a right-bounded window we only need to pull in documents up to the bound.
        (*this)[*upperBound];
    } else {
        // For a right-unbounded window we need to pull in the whole partition. operator[] reports
        // end of partition by returning boost::none instead of a document.
        for (int i = 0; (*this)[i]; ++i) {
        }
    }

    // Valid offsets into the cache are any 'i' such that '_cache[_currentCacheIndex + i]' is valid.
    // We know the cache is nonempty because it contains the current document.
    int cacheOffsetMin = -_currentCacheIndex;
    int cacheOffsetMax = cacheOffsetMin + _cache.size() - 1;

    // The window can only be empty if the bounds are shifted completely out of the partition.
    if (lowerBound && lowerBound > cacheOffsetMax)
        return boost::none;
    if (upperBound && upperBound < cacheOffsetMin)
        return boost::none;

    // Now we know that the window is nonempty, and the cache contains it.
    // All we have to do is clamp the bounds to fall within the cache.
    auto clamp = [&](int offset) {
        // Return the closest offset from the interval '[cacheOffsetMin, cacheOffsetMax]'.
        return std::max(cacheOffsetMin, std::min(offset, cacheOffsetMax));
    };
    int lowerOffset = lowerBound ? clamp(*lowerBound) : cacheOffsetMin;
    int upperOffset = upperBound ? clamp(*upperBound) : cacheOffsetMax;

    return {{lowerOffset, upperOffset}};
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
        // Because partitioning is achieved by sorting in $setWindowFields, and missing fields and
        // nulls are considered equivalent in sorting, documents with missing fields and nulls may
        // interleave with each other, resulting in these documents processed into many separate
        // partitions (null, missing, null, missing). However, it is still guranteed that all nulls
        // and missing values will be grouped together after sorting. To address this issue, we
        // coerce documents with the missing fields to null partition, which is also consistent with
        // the approach in $group.
        auto retValue = (*_partitionExpr)->evaluate(doc, &_expCtx->variables);
        auto curKey = retValue.missing() ? Value(BSONNULL) : std::move(retValue);
        uassert(ErrorCodes::TypeMismatch,
                "Cannot 'partitionBy' an expression of type array",
                !curKey.isArray());
        if (_state == IteratorState::kNotInitialized) {
            _nextPartition = NextPartitionState{std::move(doc), std::move(curKey)};
            advanceToNextPartition();
        } else if (_expCtx->getValueComparator().compare(curKey, _partitionKey) != 0) {
            _nextPartition = NextPartitionState{std::move(doc), std::move(curKey)};
            _memUsageBytes += getNextPartitionStateSize();
            _state = IteratorState::kAwaitingAdvanceToNext;
        } else {
            _memUsageBytes += doc.getApproximateSize();
            _cache.emplace_back(std::move(doc));
        }
    } else {
        _memUsageBytes += doc.getApproximateSize();
        _cache.emplace_back(std::move(doc));
        _state = IteratorState::kIntraPartition;
    }
}

}  // namespace mongo
