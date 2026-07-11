// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/collection_index_usage_tracker_decoration.h"

#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {
const auto getCollectionIndexUsageTrackerDecoration =
    Collection::declareDecoration<CollectionIndexUsageTrackerDecoration>();

auto& collectionScansCounter = *MetricBuilder<Counter64>("queryExecutor.collectionScans.total");
auto& collectionScansNonTailableCounter =
    *MetricBuilder<Counter64>("queryExecutor.collectionScans.nonTailable");

auto& profilerScansCounter =
    *MetricBuilder<Counter64>("queryExecutor.profiler.collectionScans.total");
auto& profilerScansTailableCounter =
    *MetricBuilder<Counter64>("queryExecutor.profiler.collectionScans.tailable");
auto& profilerScansNonTailableCounter =
    *MetricBuilder<Counter64>("queryExecutor.profiler.collectionScans.nonTailable");

}  // namespace

CollectionIndexUsageTracker& CollectionIndexUsageTrackerDecoration::write(Collection* collection) {
    auto& decoration = getCollectionIndexUsageTrackerDecoration(collection);

    // Make copy of existing CollectionIndexUsageTracker and store it in our writable Collection
    // instance.
    decoration._collectionIndexUsageTracker =
        new CollectionIndexUsageTracker(*decoration._collectionIndexUsageTracker);

    return *decoration._collectionIndexUsageTracker;
}

CollectionIndexUsageTrackerDecoration::CollectionIndexUsageTrackerDecoration() {
    // This can get instantiated in unittests that doesn't set a global service context.
    if (!hasGlobalServiceContext())
        return;

    _collectionIndexUsageTracker =
        new CollectionIndexUsageTracker(AggregatedIndexUsageTracker::get(getGlobalServiceContext()),
                                        getGlobalServiceContext()->getPreciseClockSource());
}

void CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
    const Collection* coll,
    long long collectionScans,
    long long collectionScansNonTailable,
    const std::set<std::string>& indexesUsed) {
    getCollectionIndexUsageTrackerDecoration(coll)
        ._collectionIndexUsageTracker->recordCollectionIndexUsage(
            collectionScans, collectionScansNonTailable, indexesUsed);

    // All five global `Counter64` increments below boil down to `fetchAndAdd(0)` for index-only
    // reads (YCSB 100read, etc.), which still bounces the shared cache lines across every core.
    // A single compound guard skips the whole block when there is nothing to record.
    if (collectionScans > 0 || collectionScansNonTailable > 0) {
        if (coll->ns().isSystemDotProfile()) {
            profilerScansCounter.increment(collectionScans);
            profilerScansTailableCounter.increment(collectionScans - collectionScansNonTailable);
            profilerScansNonTailableCounter.increment(collectionScansNonTailable);
        }

        collectionScansCounter.increment(collectionScans);
        collectionScansNonTailableCounter.increment(collectionScansNonTailable);
    }
}

CollectionIndexUsageTracker::CollectionIndexUsageMap
CollectionIndexUsageTrackerDecoration::getUsageStats(const Collection* coll) {
    return getCollectionIndexUsageTrackerDecoration(coll)
        ._collectionIndexUsageTracker->getUsageStats();
}

CollectionIndexUsageTracker::CollectionScanStats
CollectionIndexUsageTrackerDecoration::getCollectionScanStats(const Collection* coll) {
    return getCollectionIndexUsageTrackerDecoration(coll)
        ._collectionIndexUsageTracker->getCollectionScanStats();
}

}  // namespace mongo
