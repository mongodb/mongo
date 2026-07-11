// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <algorithm>
#include <cstdint>

namespace mongo {

/**
 * For collecting spilling stats.
 */
class SpillingStats {
public:
    void incrementSpills(uint64_t spills = 1);
    uint64_t getSpills() const {
        return _spills;
    }
    void setSpills(uint64_t spills) {
        _spills = spills;
    }

    void incrementSpilledBytes(uint64_t spilledBytes);
    uint64_t getSpilledBytes() const {
        return _spilledBytes;
    }
    void setSpilledBytes(uint64_t spilledBytes) {
        _spilledBytes = spilledBytes;
    }

    void incrementSpilledDataStorageSize(uint64_t spilledDataStorageSize);

    // Updates the spilled data storage size and returns the incremental change.
    uint64_t updateSpilledDataStorageSize(uint64_t totalSpilledDataStorageSize) {
        uint64_t currentSpilledDataStorageSize = _spilledDataStorageSize;
        _spilledDataStorageSize = std::max(_spilledDataStorageSize, totalSpilledDataStorageSize);
        return _spilledDataStorageSize - currentSpilledDataStorageSize;
    }

    uint64_t getSpilledDataStorageSize() const {
        return _spilledDataStorageSize;
    }

    void incrementSpilledRecords(uint64_t spilledRecords);
    uint64_t getSpilledRecords() const {
        return _spilledRecords;
    }
    void setSpilledRecords(uint64_t spilledRecords) {
        _spilledRecords = spilledRecords;
    }

    // Updates the spilling stats and returns the incremental change in spilled data storage size.
    uint64_t updateSpillingStats(uint64_t additionalSpills,
                                 uint64_t additionalSpilledBytes,
                                 uint64_t additionalSpilledRecords,
                                 uint64_t currentSpilledDataStorageSize) {
        incrementSpills(additionalSpills);
        incrementSpilledBytes(additionalSpilledBytes);
        incrementSpilledRecords(additionalSpilledRecords);
        return updateSpilledDataStorageSize(currentSpilledDataStorageSize);
    }

    void accumulate(const SpillingStats& rhs) {
        incrementSpills(rhs.getSpills());
        incrementSpilledBytes(rhs.getSpilledBytes());
        incrementSpilledDataStorageSize(rhs.getSpilledDataStorageSize());
        incrementSpilledRecords(rhs.getSpilledRecords());
    }

private:
    // The number of times the tracked entity spilled.
    uint64_t _spills = 0;
    // The size, in bytes, of the memory released with spilling.
    uint64_t _spilledBytes = 0;
    // The size, in bytes, of disk space used for spilling.
    uint64_t _spilledDataStorageSize = 0;
    // The number of records, written to record store.
    uint64_t _spilledRecords = 0;
};

}  // namespace mongo
