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

    template <typename MutexType>
    void add(StringData tag, const MutexType& mutex) {
// TODO(SERVER-110898): Remove once TSAN works with ObservableMutex.
#if !__has_feature(thread_sanitizer)
        std::list<NewMutexEntry> newNode;
        newNode.push_back({.tag = std::string(tag),
                           .registrationTime = _clockSource->now(),
                           .token = mutex.token()});
        stdx::lock_guard lk(_registrationMutex);
        _newMutexEntries.splice(_newMutexEntries.end(), newNode);
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

private:
    // `MutexEntry` is what is stored for each registered mutex.
    struct MutexEntry {
        int64_t id;
        Date_t registrationTime;
        std::shared_ptr<ObservationToken> token;
    };

    // When a mutex is `add`ed, a `NewMutexEntry` is stored for later retrieval within `report`.
    // `report` then converts the `NewMutexEntry` into a `MutexEntry`.
    struct NewMutexEntry {
        std::string tag;
        Date_t registrationTime;
        std::shared_ptr<ObservationToken> token;
    };

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

    mutable stdx::mutex _registrationMutex;
    std::list<NewMutexEntry> _newMutexEntries;

    mutable stdx::mutex _collectionMutex;
    int64_t _nextMutexId{0};
    StringMap<std::list<MutexEntry>> _mutexEntries;
    // Maps a Mutex tag to the sum of all its invalidated token stats.
    StringMap<MutexStats> _removedTokensSnapshots;
};

}  // namespace mongo
