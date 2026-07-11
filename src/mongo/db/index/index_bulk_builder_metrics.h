// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo {

// Represents a point-in-time snapshot of index bulk builder metrics, including those used in
// serverStatus and OTel metrics.
struct IndexBulkBuilderMetricsSnapshot {
    long long count, resumed, filesOpenedForExternalSort, filesClosedForExternalSort, spilledRanges,
        mergedSpills, bytesSpilledUncompressed, bytesSpilled, numSorted, bytesSorted, memUsage;
};

struct IndexBulkBuilderMetrics {
    // Number of instances of the bulk builder created.
    Atomic<long long> count;

    // Number of times the bulk builder was created for a resumable index build.
    // This value should not exceed 'count'.
    Atomic<long long> resumed;

    // Sorter statistics that are aggregate of all sorters.
    SorterTracker sorterTracker;

    // Number of times a file-based external sorter opens/closes a file handle to spill data to
    // disk. This pair of counters in aggregate indicate the number of open file handles used by the
    // external sorter and may be useful in diagnosing situations where the process is close to
    // exhausting this finite resource.
    SorterFileStats sorterFileStats{&sorterTracker};

    // Tracks the number of bytes of uncompressed data spilled from a external sorter with a
    // container as the underlying storage. This is the only metric tracked as we only open one
    // container per sorter instance and we don't handle compression in the sorter for a
    // container-based sorter.
    SorterContainerStats sorterContainerStats{&sorterTracker};

    // Get a snapshot (as a copy) of index bulk builder metrics.
    IndexBulkBuilderMetricsSnapshot snapshot() const;
};

// Get the global index bulk builder metrics tracker, which is used by all index build sorters and
// which back the "indexBulkBuilder" serverStatus section as well as related OTel metrics.
IndexBulkBuilderMetrics& indexBulkBuilderMetrics();

// Updates the "indexBulkBuilder" OTel metrics, based on the deltas between the previous and the
// current metrics snapshot.
void updateIndexBulkBuilderOtelMetrics(const IndexBulkBuilderMetricsSnapshot& prev,
                                       const IndexBulkBuilderMetricsSnapshot& curr);

}  // namespace mongo
