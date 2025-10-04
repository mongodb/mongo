/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/local_catalog/collection.h"

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
class CollectionIndexUsageTrackerDecoration {
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
