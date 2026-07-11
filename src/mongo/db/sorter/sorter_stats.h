// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

[[MONGO_MOD_PUBLIC]];
namespace mongo {

/**
 * For collecting cumulative stats of all sorters.
 */
struct SorterTracker {
    Atomic<long long> spilledRanges{0};
    Atomic<long long> mergedSpills{0};
    Atomic<long long> spilledKeyValuePairs{0};
    Atomic<long long> bytesSpilled{0};
    Atomic<long long> bytesSpilledUncompressed{0};
    Atomic<long long> numSorted{0};
    Atomic<long long> bytesSorted{0};
    Atomic<long long> memUsage{0};
};

/**
 * For collecting container usage metrics.
 */
class SorterContainerStats {
public:
    explicit SorterContainerStats(SorterTracker* sorterTracker);

    void addSpilledDataSize(long long size);
    void addSpilledDataSizeUncompressed(long long size);

    void incrementNumSpilledEntries();

    long long bytesSpilled() const {
        return _bytesSpilled.load();
    }

    long long bytesSpilledUncompressed() const {
        return _bytesSpilledUncompressed.load();
    }

    long long numSpilledEntries() const {
        return _numSpilledEntries.load();
    }

private:
    SorterTracker* _sorterTracker;

    Atomic<long long> _bytesSpilled;
    Atomic<long long> _bytesSpilledUncompressed;
    Atomic<long long> _numSpilledEntries;
};


/**
 * For collecting file usage metrics.
 */
class SorterFileStats {
public:
    explicit SorterFileStats(SorterTracker* sorterTracker);

    void addSpilledDataSize(long long size);
    void addSpilledDataSizeUncompressed(long long size);

    Atomic<long long> opened;
    Atomic<long long> closed;

    long long bytesSpilled() const {
        return _bytesSpilled.load();
    }

    long long bytesSpilledUncompressed() const {
        return _bytesSpilledUncompressed.load();
    }

private:
    SorterTracker* _sorterTracker;

    Atomic<long long> _bytesSpilled;
    Atomic<long long> _bytesSpilledUncompressed;
};

/**
 * For collecting individual sorter stats.
 */
class SorterStats {
public:
    SorterStats(SorterTracker* sorterTracker);

    void incrementSpilledRanges();
    /**
     * Sets the number of spilled ranges to the specified amount.
     */
    void setSpilledRanges(uint64_t spills);
    uint64_t spilledRanges() const;

    void incrementMergedSpills();
    uint64_t mergedSpills() const;

    void incrementSpilledKeyValuePairs(uint64_t keyValuePairs);
    uint64_t spilledKeyValuePairs() const;

    void incrementNumSorted(uint64_t sortedKeys = 1);
    uint64_t numSorted() const;

    void incrementBytesSorted(uint64_t bytes);
    uint64_t bytesSorted() const;

    void incrementMemUsage(uint64_t memUsage);
    void decrementMemUsage(uint64_t memUsage);
    void resetMemUsage();
    void setMemUsage(uint64_t memUsage);
    uint64_t memUsage() const;

private:
    uint64_t _spilledRanges = 0;         // Number of spills.
    uint64_t _mergedSpills = 0;          // Number of times spilled ranges were merged.
    uint64_t _spilledKeyValuePairs = 0;  // Number of spilled pairs.
    uint64_t _numSorted = 0;             // Number of keys sorted.
    uint64_t _bytesSorted = 0;           // Total bytes of data sorted.
    uint64_t _memUsage = 0;              // Current memory being used.

    // All SorterStats update the SorterTracker to report sorter statistics for the
    // server.
    SorterTracker* _sorterTracker;
};
}  // namespace mongo
