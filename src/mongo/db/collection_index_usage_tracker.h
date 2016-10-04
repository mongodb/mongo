/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ClockSource;

/**
 * CollectionIndexUsageTracker tracks index usage statistics for a collection.  An index is
 * considered "used" when it appears as part of a winning plan for an operation that uses the
 * query system.
 *
 * Indexes must be registered and deregistered on creation/destruction.
 */
class CollectionIndexUsageTracker {
    MONGO_DISALLOW_COPYING(CollectionIndexUsageTracker);

public:
    struct IndexUsageStats {
        IndexUsageStats() = default;
        explicit IndexUsageStats(Date_t now, const BSONObj& key)
            : trackerStartTime(now), indexKey(key.getOwned()) {}

        IndexUsageStats(const IndexUsageStats& other)
            : accesses(other.accesses.load()),
              trackerStartTime(other.trackerStartTime),
              indexKey(other.indexKey) {}

        IndexUsageStats& operator=(const IndexUsageStats& other) {
            accesses.store(other.accesses.load());
            trackerStartTime = other.trackerStartTime;
            indexKey = other.indexKey;
            return *this;
        }

        // Number of operations that have used this index.
        AtomicInt64 accesses;

        // Date/Time that we started tracking index usage.
        Date_t trackerStartTime;

        // An owned copy of the associated IndexDescriptor's index key.
        BSONObj indexKey;
    };

    /**
     * Constructs a CollectionIndexUsageTracker.
     *
     * Does not take ownership of 'clockSource'. 'clockSource' must refer to a non-null clock
     * source that is valid for the lifetime of the constructed CollectionIndexUsageTracker.
     */
    explicit CollectionIndexUsageTracker(ClockSource* clockSource);

    /**
     * Record that an operation used index 'indexName'. Safe to be called by multiple threads
     * concurrently.
     */
    void recordIndexAccess(StringData indexName);

    /**
     * Add map entry for 'indexName' stats collection. Must be called under exclusive collection
     * lock.
     */
    void registerIndex(StringData indexName, const BSONObj& indexKey);

    /**
     * Erase statistics for index 'indexName'. Must be called under exclusive collection lock.
     * Can be called even if indexName is not registered. This is possible under certain failure
     * scenarios.
     */
    void unregisterIndex(StringData indexName);

    /**
     * Get the current state of the usage statistics map. This map will only include indexes that
     * exist at the time of calling. Must be called while holding the collection lock in any mode.
     */
    StringMap<CollectionIndexUsageTracker::IndexUsageStats> getUsageStats() const;

private:
    // Map from index name to usage statistics.
    StringMap<CollectionIndexUsageTracker::IndexUsageStats> _indexUsageMap;

    // Clock source. Used when the 'trackerStartTime' time for an IndexUsageStats object needs to
    // be set.
    ClockSource* _clockSource;
};

typedef StringMap<CollectionIndexUsageTracker::IndexUsageStats> CollectionIndexUsageMap;

}  // namespace mongo
