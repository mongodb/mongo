/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/string_map.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The registry keeps track of all registered instances of `ObservableMutex` and provides an
 * interface to collect contention stats.
 */
class ObservableMutexRegistry {
public:
    static constexpr auto kTotalAcquisitionsFieldName = "total"_sd;
    static constexpr auto kTotalContentionsFieldName = "contentions"_sd;
    static constexpr auto kTotalWaitCyclesFieldName = "waitCycles"_sd;
    static constexpr auto kExclusiveFieldName = "exclusive"_sd;
    static constexpr auto kSharedFieldName = "shared"_sd;
    static constexpr auto kMutexFieldName = "mutexes"_sd;
    static constexpr auto kIdFieldName = "id"_sd;
    static constexpr auto kRegisteredFieldName = "registered"_sd;

    struct StatsRecord {
        MutexStats data;
        boost::optional<int64_t> mutexId;
        boost::optional<Date_t> registered;
    };

    static ObservableMutexRegistry& get();

    ObservableMutexRegistry() : ObservableMutexRegistry(SystemClockSource::get()) {}
    explicit ObservableMutexRegistry(ClockSource* clk) : _clockSource(clk) {}

    /**
     * This may have performance implications as it serializes registering
     * with lookups, so avoid using it for registering mutex objects that are
     * created on a hot path, with visible performance implications for the user.
     */
    template <typename MutexType>
    void add(StringData tag, const MutexType& mutex) {
#ifdef MONGO_CONFIG_MUTEX_OBSERVATION
        std::list<MutexEntry> newNode{{_getNextMutexId(), _clockSource->now(), mutex.token()}};
        const auto hashedTag = _mutexEntries.hash_function().hashed_key(tag);
        stdx::lock_guard lk(_registrationMutex);
        auto& mutexList = _mutexEntries[hashedTag];
        mutexList.splice(mutexList.end(), newNode);
#endif
    }

    /**
     * Gathers statistics for each mutex that was added to the registry in the following format.
     * Mutexes that share the same tag will have their stats aggregated under "exclusive" and
     * "shared". These aggregated stats will also include the data of invalidated tokens that were
     * previously added to the registry.
     *
     * If listAll is enabled, all valid mutexes within the registry will be listed separately in
     * the "mutexes" field. Furthermore, when listAll is active, each mutex entry will include a
     * timestamp indicating when the mutex was registered in _mutexEntries, along with a unique
     * identifier.
     *
     * {
     *     [TagName]: {
     *         "exclusive": {
     *             "total": "0",
     *             "contentions": "0",
     *             "waitCycles": "0",
     *         },
     *         "shared": {
     *             "total": "0",
     *             "contentions": "0",
     *             "waitCycles": "0",
     *         }
     *         "mutexes" : [    // Only emitted if listAll == true.
     *             {
     *                 id": 0
     *                 registered: ...
     *                 "exclusive": {
     *                     ...
     *                 }
     *                 "shared": {
     *                     ...
     *                 }
     *             }
     *             ...
     *         ]
     *     }
     * }
     */
    BSONObj report(bool listAll);

    size_t getNumRegistered_forTest() const;

private:
    struct MutexEntry {
        int64_t id;
        Date_t registrationTime;
        std::shared_ptr<ObservationToken> token;
    };

    int64_t _getNextMutexId() {
        return _nextMutexId.fetchAndAdd(1);
    }

    /**
     * Visits all registered mutex objects stored in the registry and collects their stats. For any
     * invalid mutex that is visited, its stats are added to _removedTokensSnapshots. Returns stats
     * mapped by tag for all valid mutex entries along with stats stored in _removedTokensSnapshots.
     */
    StringMap<std::vector<StatsRecord>> _collectStats();

    /**
     * Adds stats from _removedTokensSnapshots into statsMap. Optional fields within a statsMap
     * entry are left blank.
     */
    void _includeRemovedSnapshots(WithLock, StringMap<std::vector<StatsRecord>>& statsMap);

    ClockSource* _clockSource;
    Atomic<int64_t> _nextMutexId{0};

    mutable stdx::mutex _registrationMutex;
    StringMap<std::list<MutexEntry>> _mutexEntries;

    mutable stdx::mutex _collectionMutex;
    // Maps a Mutex tag to the sum of all its invalidated token stats.
    StringMap<MutexStats> _removedTokensSnapshots;
};

}  // namespace mongo
