/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/aggregated_index_usage_tracker.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;
class ServiceContext;

/**
 * CollectionIndexUsageTracker tracks index usage statistics for a collection.  An index is
 * considered "used" when it appears as part of a winning plan for an operation that uses the
 * query system.
 *
 * It also tracks non-usage of indexes. I.e. it collects information about collection scans that
 * occur on a collection.
 *
 * Indexes must be registered and deregistered on creation/destruction.
 *
 * Internally concurrency safe for multiple callers concurrently but with only a SINGLE caller of
 * either registerIndex() or unregisterIndex() at a time. Callers of registerIndex() and
 * unregisterIndex() must hold an exclusive collection lock to ensure serialization.
 */
class CollectionIndexUsageTracker {
    CollectionIndexUsageTracker(const CollectionIndexUsageTracker&) = delete;
    CollectionIndexUsageTracker& operator=(const CollectionIndexUsageTracker&) = delete;

public:
    struct CollectionScanStats {
        unsigned long long collectionScans{0};
        unsigned long long collectionScansNonTailable{0};
    };

    struct IndexUsageStats : public RefCountable {
        IndexUsageStats() = default;
        explicit IndexUsageStats(Date_t now, const BSONObj& key, const IndexFeatures& idxFeatures)
            : trackerStartTime(now), indexKey(key.getOwned()), features(idxFeatures) {}

        IndexUsageStats(const IndexUsageStats& other)
            : accesses(other.accesses.load()),
              trackerStartTime(other.trackerStartTime),
              indexKey(other.indexKey),
              features(other.features) {}

        IndexUsageStats& operator=(const IndexUsageStats& other) {
            accesses.store(other.accesses.load());
            trackerStartTime = other.trackerStartTime;
            indexKey = other.indexKey;
            features = other.features;
            return *this;
        }

        // Number of operations that have used this index.
        AtomicWord<long long> accesses;

        // Date/Time that we started tracking index usage.
        Date_t trackerStartTime;

        // An owned copy of the associated IndexDescriptor's index key.
        BSONObj indexKey;

        // Features in use by this index for global feature usage tracking.
        IndexFeatures features;
    };

    /**
     * The IndexUsageStats entries are stored in the map as pointers so that concurrent updates to
     * an entry's internal values are retained across map copies rather than lost. intrusive_ptrs
     * are used rather than shared_ptrs because: intrusive_ptrs are more performant when copies are
     * made; and the usage is encapsulated by this class and not externally exposed.
     */
    using CollectionIndexUsageMap = StringMap<boost::intrusive_ptr<IndexUsageStats>>;

    /**
     * Constructs a CollectionIndexUsageTracker.
     *
     * Does not take ownership of 'clockSource'. 'clockSource' must refer to a non-null clock
     * source that is valid for the lifetime of the constructed CollectionIndexUsageTracker.
     */
    explicit CollectionIndexUsageTracker(AggregatedIndexUsageTracker* aggregatedIndexUsageTracker,
                                         ClockSource* clockSource);

    /**
     * Record that an operation used index 'indexName'. Safe to be called by multiple threads
     * concurrently.
     */
    void recordIndexAccess(StringData indexName);

    /**
     * Add map entry for 'indexName' stats collection.
     *
     * Must be called under an exclusive collection lock in order to serialize calls to
     * registerIndex() and unregisterIndex().
     */
    void registerIndex(StringData indexName,
                       const BSONObj& indexKey,
                       const IndexFeatures& features);

    /**
     * Erase statistics for index 'indexName'. Can be safely called even if indexName is not
     * registered, which is possible under certain failure scenarios.
     *
     * Must be called under an exclusive collection lock in order to serialize calls to
     * registerIndex() and unregisterIndex().
     */
    void unregisterIndex(StringData indexName);

    /**
     * Get the current state of the usage statistics map. This map will only include indexes that
     * exist at the time of calling.
     */
    std::shared_ptr<CollectionIndexUsageMap> getUsageStats() const;

    /**
     * Get the current state of the usage of collection scans. This struct will only include
     * information about the collection scans that have occured at the time of calling.
     *
     * Can be safely called by multiple threads concurrently.
     */
    CollectionScanStats getCollectionScanStats() const;

    /**
     * Records that an operation did a collection scan.
     *
     * Can be safely called by multiple threads concurrently.
     */
    void recordCollectionScans(unsigned long long collectionScans);
    void recordCollectionScansNonTailable(unsigned long long collectionScansNonTailable);

private:
    // Maps index name to index usage statistics.
    //
    // NOTE: This map must only be accessed via atomic_load and atomic_store!
    //
    // Internal concurrency control is ensured by always using atomic_load/store on this shared_ptr.
    // The map should never be modified outside the protection of atomic_load/atomic_store.
    std::shared_ptr<CollectionIndexUsageMap> _indexUsageStatsMap;

    // Clock source. Used when the 'trackerStartTime' time for an IndexUsageStats object needs to
    // be set.
    ClockSource* _clockSource;

    // All CollectionIndexUsageTrackers also update the AggregatedIndexUsageTracker to report
    // globally aggregated index statistics for the server.
    AggregatedIndexUsageTracker* _aggregatedIndexUsageTracker;

    AtomicWord<unsigned long long> _collectionScans{0};
    AtomicWord<unsigned long long> _collectionScansNonTailable{0};
};

}  // namespace mongo
