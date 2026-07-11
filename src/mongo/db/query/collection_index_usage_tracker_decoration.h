// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {

/**
 * All Collection instances for the same collection share the same CollectionIndexUsageTracker
 * instance. CollectionIndexUsageTrackerDecoration decorates a decorable object that all Collection
 * instances for the same collection hold in shared ownership. See the Collection header file for
 * more details.
 *
 * CollectionIndexUsageTracker needs a wrapper class (this class) to initialize it with a clock
 * source.
 */
class [[MONGO_MOD_PUBLIC]] CollectionIndexUsageTrackerDecoration {
public:
    /**
     * Performs a copy of the CollectionIndexUsageTracker and stores the new instance in the
     * writable collection. Returns this uniquely owned instance that is safe to perform
     * modifications on.
     */
    static CollectionIndexUsageTracker& write(Collection* collection);

    /**
     * Record collection and index usage statistics globally and for this collection.
     */
    static void recordCollectionIndexUsage(const Collection* coll,
                                           long long collectionScans,
                                           long long collectionScansNonTailable,
                                           const std::set<std::string>& indexesUsed);

    /**
     * Returns index usage statistics for this collection.
     */
    static CollectionIndexUsageTracker::CollectionIndexUsageMap getUsageStats(
        const Collection* coll);

    /**
     * Returns collection scan statistics for this collection.
     */
    static CollectionIndexUsageTracker::CollectionScanStats getCollectionScanStats(
        const Collection* coll);

    /**
     * Initializes the CollectionIndexUsageTracker.
     */
    CollectionIndexUsageTrackerDecoration();

private:
    // Tracks index usage statistics for a collection. This is shared between versions of the same
    // Collection instance until a change is made.
    boost::intrusive_ptr<CollectionIndexUsageTracker> _collectionIndexUsageTracker;
};

}  // namespace mongo
