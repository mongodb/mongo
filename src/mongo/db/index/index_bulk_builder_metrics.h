/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter_stats.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUBLIC;
namespace mongo {

// Represents a point-in-time snapshot of index bulk builder metrics, including those used in
// serverStatus and OTel metrics.
struct IndexBulkBuilderMetricsSnapshot {
    long long count, resumed, filesOpenedForExternalSort, filesClosedForExternalSort, spilledRanges,
        mergedSpills, bytesSpilledUncompressed, bytesSpilled, numSorted, bytesSorted, memUsage;
};

struct IndexBulkBuilderMetrics {
    // Number of instances of the bulk builder created.
    AtomicWord<long long> count;

    // Number of times the bulk builder was created for a resumable index build.
    // This value should not exceed 'count'.
    AtomicWord<long long> resumed;

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
