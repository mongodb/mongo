// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/space_saving.h"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace mongo::stats {

std::string MCVEntry::toString() const {
    std::ostringstream os;
    os << "value: " << value.get() << ", estFreq: " << estFreq << ", err: " << err;
    return os.str();
}

SpaceSavingSketch::SpaceSavingSketch(size_t k)
    : _k(std::max(k, kMinNumCounters)), _table(0, sbe::value::ValueHash(), sbe::value::ValueEq()) {
    _counters.reserve(_k);
    _table.reserve(_k);
    // At most k non-empty buckets can exist at once, one per counter.
    _bucketPool.resize(_k);
    for (auto& bucket : _bucketPool) {
        bucket._next = _freeBuckets;
        _freeBuckets = &bucket;
    }
}

void SpaceSavingSketch::_insertIntoBucketBefore(Element* element, Bucket* before, size_t count) {
    Bucket* bucket;
    if (before && before->_count == count) {
        bucket = before;
    } else {
        bucket = _freeBuckets;
        _freeBuckets = bucket->_next;
        bucket->_count = count;
        bucket->_head = bucket->_tail = nullptr;
        bucket->_next = before;
        bucket->_prev = before ? before->_prev : _maxBucket;
        (bucket->_prev ? bucket->_prev->_next : _minBucket) = bucket;
        (before ? before->_prev : _maxBucket) = bucket;
    }
    element->_bucket = bucket;
    element->_prev = nullptr;
    element->_next = bucket->_head;
    (bucket->_head ? bucket->_head->_prev : bucket->_tail) = element;
    bucket->_head = element;
}

void SpaceSavingSketch::_removeFromBucket(Element* element) {
    Bucket* bucket = element->_bucket;
    (element->_prev ? element->_prev->_next : bucket->_head) = element->_next;
    (element->_next ? element->_next->_prev : bucket->_tail) = element->_prev;
    element->_bucket = nullptr;
    if (!bucket->_head) {
        (bucket->_prev ? bucket->_prev->_next : _minBucket) = bucket->_next;
        (bucket->_next ? bucket->_next->_prev : _maxBucket) = bucket->_prev;
        bucket->_next = _freeBuckets;
        _freeBuckets = bucket;
    }
}

void SpaceSavingSketch::increment(const SBEValue& value) {
    const auto it = _table.find(value.get());
    if (it != _table.end()) {
        // Monitored value: move its counter from bucket with estimate 'c' to bucket with 'c + 1'.
        // Reuse the adjacent next bucket if it already holds count 'c + 1'.
        Element* element = it->second;
        const size_t nextCount = element->_bucket->_count + 1;
        // Still valid if the element's bucket is released below.
        Bucket* const next = element->_bucket->_next;
        _removeFromBucket(element);
        _insertIntoBucketBefore(element, next, nextCount);
        return;
    }

    if (_counters.size() < _k) {
        // There are unused counters left: create a new counter with count 1 and no error.
        _counters.push_back(Element{SBEValue{value}});
        Element* element = &_counters.back();
        _insertIntoBucketBefore(element, _minBucket, 1);
        _table.insert(element->_value.get(), element);
        return;
    }

    // All k counters are used: evict the tail of the minimum-count bucket and reuse its counter for
    // the newcomer. The estimate is set to the evicted count plus 1 and the error term is set to
    // the evicted count. The newcomer's true count is thereby bounded between 1 and the evicted
    // count + 1.
    Element* reusedElement = _minBucket->_tail;
    const size_t evictedCount = _minBucket->_count;
    Bucket* const next = _minBucket->_next;
    _table.erase(_table.find(reusedElement->_value.get()));
    _removeFromBucket(reusedElement);
    reusedElement->_value = SBEValue{value};
    reusedElement->_err = evictedCount;
    _insertIntoBucketBefore(reusedElement, next, evictedCount + 1);
    _table.insert(reusedElement->_value.get(), reusedElement);
}

std::vector<MCVEntry> SpaceSavingSketch::getTopK() const {
    std::vector<MCVEntry> entries;
    entries.reserve(_counters.size());
    for (const auto& counter : _counters) {
        entries.emplace_back(SBEValue{counter._value}, counter._bucket->_count, counter._err);
    }
    std::sort(entries.begin(), entries.end(), [](const MCVEntry& a, const MCVEntry& b) {
        if (a.estFreq != b.estFreq) {
            return a.estFreq > b.estFreq;
        }
        return compareValues(
                   a.value.getTag(), a.value.getValue(), b.value.getTag(), b.value.getValue()) < 0;
    });
    return entries;
}

MostCommonValues MostCommonValues::make(const std::vector<SBEValue>& values, size_t k) {
    SpaceSavingSketch sketch(k);
    for (const auto& value : values) {
        sketch.increment(value);
    }
    MostCommonValues result{sketch.getTopK()};
    result._inputSize = values.size();
    return result;
}

size_t MostCommonValues::getCumulativeLowBoundFrequency() const {
    size_t sum = 0;
    for (const auto& entry : _entries) {
        sum += entry.estFreq - entry.err;
    }
    return sum;
}

std::string MostCommonValues::toString() const {
    std::ostringstream os;
    if (_inputSize > 0) {
        const double toPercent = 100.0 / _inputSize;
        const size_t guaranteedCumulativeFrequency = getCumulativeLowBoundFrequency();
        os << std::fixed << std::setprecision(1) << "inputSize: " << _inputSize
           << ", cumulativeGuaranteedFrequency: " << guaranteedCumulativeFrequency << " ("
           << guaranteedCumulativeFrequency * toPercent << "%)"
           << ", guaranteedTopK: " << getGuaranteedTopK() << "\n";
    }
    for (const auto& entry : _entries) {
        os << entry.toString() << "\n";
    }
    return os.str();
}

size_t MostCommonValues::getGuaranteedTopK() const {
    // All error terms zero means no counter was ever evicted: every distinct value is
    // monitored (a distinct count above k would have forced an eviction), so all estimates
    // are exact and the whole prefix is guaranteed.
    if (std::all_of(_entries.begin(), _entries.end(), [](const MCVEntry& entry) {
            return entry.err == 0;
        })) {
        return _entries.size();
    }
    size_t numGuaranteed = 0;
    size_t minLowerBound = std::numeric_limits<size_t>::max();
    for (size_t j = 0; j < _entries.size(); ++j) {
        if (j > 0 && minLowerBound > _entries[j].estFreq) {
            numGuaranteed = j;
        }
        minLowerBound = std::min(minLowerBound, _entries[j].estFreq - _entries[j].err);
    }
    return numGuaranteed;
}

}  // namespace mongo::stats
