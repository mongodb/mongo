// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/sorter_stats.h"

#include "mongo/util/assert_util.h"

#include <algorithm>

namespace mongo {

SorterContainerStats::SorterContainerStats(SorterTracker* sorterTracker)
    : _sorterTracker(sorterTracker) {};

void SorterContainerStats::addSpilledDataSize(long long size) {
    _bytesSpilled.fetchAndAdd(size);
    if (_sorterTracker) {
        _sorterTracker->bytesSpilled.fetchAndAdd(size);
    }
}

void SorterContainerStats::addSpilledDataSizeUncompressed(long long size) {
    _bytesSpilledUncompressed.fetchAndAdd(size);
    if (_sorterTracker) {
        _sorterTracker->bytesSpilledUncompressed.fetchAndAdd(size);
    }
}

void SorterContainerStats::incrementNumSpilledEntries() {
    _numSpilledEntries.fetchAndAdd(1);
}

SorterFileStats::SorterFileStats(SorterTracker* sorterTracker) : _sorterTracker(sorterTracker) {};

void SorterFileStats::addSpilledDataSize(long long size) {
    _bytesSpilled.fetchAndAdd(size);
    if (_sorterTracker) {
        _sorterTracker->bytesSpilled.fetchAndAdd(size);
    }
}

void SorterFileStats::addSpilledDataSizeUncompressed(long long size) {
    _bytesSpilledUncompressed.fetchAndAdd(size);
    if (_sorterTracker) {
        _sorterTracker->bytesSpilledUncompressed.fetchAndAdd(size);
    }
}

SorterStats::SorterStats(SorterTracker* sorterTracker) : _sorterTracker(sorterTracker) {};

void SorterStats::incrementSpilledRanges() {
    _spilledRanges++;
    if (_sorterTracker) {
        _sorterTracker->spilledRanges.fetchAndAdd(1);
    }
}

void SorterStats::setSpilledRanges(uint64_t spills) {
    if (spills == _spilledRanges) {
        return;
    }

    if (_sorterTracker) {
        if (spills > _spilledRanges) {
            _sorterTracker->spilledRanges.fetchAndAdd(spills - _spilledRanges);
        } else {
            _sorterTracker->spilledRanges.fetchAndSubtract(_spilledRanges - spills);
        }
    }

    _spilledRanges = spills;
}

uint64_t SorterStats::spilledRanges() const {
    return _spilledRanges;
}

void SorterStats::incrementMergedSpills() {
    _mergedSpills++;
    if (_sorterTracker) {
        _sorterTracker->mergedSpills.fetchAndAdd(1);
    }
}

uint64_t SorterStats::mergedSpills() const {
    return _mergedSpills;
}

void SorterStats::incrementSpilledKeyValuePairs(uint64_t records) {
    _spilledKeyValuePairs += records;
    if (_sorterTracker) {
        _sorterTracker->spilledKeyValuePairs.fetchAndAdd(records);
    }
}

uint64_t SorterStats::spilledKeyValuePairs() const {
    return _spilledKeyValuePairs;
}

void SorterStats::incrementNumSorted(uint64_t sortedKeys) {
    _numSorted += sortedKeys;
    if (_sorterTracker) {
        _sorterTracker->numSorted.fetchAndAdd(sortedKeys);
    }
}

uint64_t SorterStats::numSorted() const {
    return _numSorted;
}

void SorterStats::incrementBytesSorted(uint64_t bytes) {
    _bytesSorted += bytes;
    if (_sorterTracker) {
        _sorterTracker->bytesSorted.fetchAndAdd(bytes);
    }
}

uint64_t SorterStats::bytesSorted() const {
    return _bytesSorted;
}

void SorterStats::incrementMemUsage(uint64_t memUsage) {
    _memUsage += memUsage;
    if (_sorterTracker) {
        _sorterTracker->memUsage.fetchAndAdd(memUsage);
    }
}

void SorterStats::decrementMemUsage(uint64_t memUsage) {
    // SERVER-61281 added memoisation for the memory usage of a document. The memory usage is stored
    // in '_snapshottedSize' and the value is not always up-to-date. Thus, it is possible for the
    // same document the 'memUsage' to have increased between the calls of 'incrementMemUsage' and
    // 'decrementMemUsage'. In this case, when 'decrementMemUsage' is called, 'memUsage' might be
    // greater than '_memUsage'.
    memUsage = std::min(memUsage, _memUsage);
    _memUsage -= memUsage;
    if (_sorterTracker) {
        _sorterTracker->memUsage.fetchAndSubtract(memUsage);
    }
}

void SorterStats::resetMemUsage() {
    setMemUsage(0);
}

void SorterStats::setMemUsage(uint64_t memUsage) {
    if (memUsage == _memUsage) {
        return;
    }

    if (_sorterTracker) {
        if (memUsage > _memUsage) {
            _sorterTracker->memUsage.fetchAndAdd(memUsage - _memUsage);
        } else {
            _sorterTracker->memUsage.fetchAndSubtract(_memUsage - memUsage);
        }
    }
    _memUsage = memUsage;
}

uint64_t SorterStats::memUsage() const {
    return _memUsage;
}
}  // namespace mongo
