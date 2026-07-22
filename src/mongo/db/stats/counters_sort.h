// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {

class SortCounters {
public:
    void incrementSortCountersPerQuery(int64_t bytesSorted, int64_t keysSorted) {
        sortTotalBytesCounter.incrementRelaxed(bytesSorted);
        sortTotalKeysCounter.incrementRelaxed(keysSorted);
    }

    void incrementSortCountersPerSpilling(int64_t sortSpills, int64_t sortSpillBytes) {
        sortSpillsCounter.incrementRelaxed(sortSpills);
        sortSpillBytesCounter.incrementRelaxed(sortSpillBytes);
    }

    // Counters tracking sort stats across all engines
    // The total number of spills from sort stages
    Counter64& sortSpillsCounter = *MetricBuilder<Counter64>{"query.sort.spillToDisk"};
    // The total bytes spilled. This is the storage size after compression.
    Counter64& sortSpillBytesCounter = *MetricBuilder<Counter64>{"query.sort.spillToDiskBytes"};
    // The number of keys that we've sorted.
    Counter64& sortTotalKeysCounter = *MetricBuilder<Counter64>{"query.sort.totalKeysSorted"};
    // The amount of data we've sorted in bytes
    Counter64& sortTotalBytesCounter = *MetricBuilder<Counter64>{"query.sort.totalBytesSorted"};
};
extern SortCounters sortCounters;

/**
 * Gauge of the current on-disk size of query spilling that lands in temporary files on the local
 * dbpath. Maintained from sorter::File, which underlies all file-based spilling.
 */
class FileSpillingMetrics {
public:
    // Current on-disk size of temporary spill files on the local dbpath. Incremented as sorter
    // files are written and decremented when they are destroyed.
    Counter64& fileSpilledStorageSize =
        *MetricBuilder<Counter64>{"query.spilling.fileSpilledStorageSize"};
};
extern FileSpillingMetrics fileSpillingMetrics;

}  // namespace mongo
